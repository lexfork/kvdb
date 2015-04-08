#include "kvdb.h"

#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <lz4.h>
#include <string>
#include <map>

#include "kvassert.h"
#include "kvendian.h"
#include "kvtypes.h"
#include "kvprime.h"
#include "kvpaddingutils.h"
#include "kvbloom.h"
#include "kvmurmurhash.h"
#include "kvtable.h"
#include "kvblock.h"
#include "kvserialization.h"

#define MARKER "KVDB"
#define VERSION 5

#define PRE_READ_KEY_SIZE 128
#define MAX_ALLOCA_SIZE 4096

static int kvdb_debug = 0;

static int internal_kvdb_set(kvdb * db, const char * key, size_t key_size, const char * value, size_t value_size);
static int internal_kvdb_get2(kvdb * db, const char * key, size_t key_size,
              char ** p_value, size_t * p_value_size, size_t * p_free_size);
static int kvdb_get2(kvdb * db, const char * key, size_t key_size,
                     char ** p_value, size_t * p_value_size, size_t * p_free_size);
static int kvdb_restore_journal(kvdb * db);

kvdb * kvdb_new(const char * filename)
{
    kvdb * db = (kvdb *) malloc(sizeof(* db));
    KVDBAssert(filename != NULL);
    db->kv_filename = strdup(filename);
    KVDBAssert(db->kv_filename != NULL);
    db->kv_fd = -1;
    db->kv_opened = 0;
    db->kv_firstmaxcount = kv_getnextprime(KV_FIRST_TABLE_MAX_COUNT);
    db->kv_compression_type = KVDB_COMPRESSION_TYPE_LZ4;
    db->kv_filesize = NULL;
    db->kv_free_blocks = NULL;
    db->kv_first_table = NULL;
    db->kv_current_table = NULL;
    db->kv_transaction = NULL;
    
    return db;
}

void kvdb_free(kvdb * db)
{
    if (db->kv_opened) {
        fprintf(stderr, "should be closed before freeing - %s\n", db->kv_filename);
    }
    free(db->kv_filename);
    free(db);
}

void kvdb_set_compression_type(kvdb * db, int compression_type)
{
    if (db->kv_opened) {
        return;
    }
    db->kv_compression_type = compression_type;
}

int kvdb_get_compression_type(kvdb * db)
{
    return db->kv_compression_type;
}

int kvdb_open(kvdb * db)
{
    int r;
    struct stat stat_buf;
    int create_file = 0;
    
    if (db->kv_opened)
        return -1;
    
    db->kv_pagesize = getpagesize();
    
    db->kv_fd = open(db->kv_filename, O_RDWR | O_CREAT, 0600);
    if (db->kv_fd == -1) {
        fprintf(stderr, "open failed\n");
        return -1;
    }
    
    r = kvdb_restore_journal(db);
    if (r < 0) {
        // It will truncate the file.
        kvdb_transaction_begin(db);
        kvdb_transaction_abort(db);
    }
    
    r = fstat(db->kv_fd, &stat_buf);
    if (r < 0) {
        close(db->kv_fd);
        // close file.
        fprintf(stderr, "fstat failed\n");
        return -1;
    }
    
    uint64_t firstmaxcount = kv_getnextprime(KV_FIRST_TABLE_MAX_COUNT);
    uint64_t first_mapping_size = KV_HEADER_SIZE + KV_TABLE_SIZE(firstmaxcount);
    
    char data[4 + 4 + 8 + 1];
    
    if (stat_buf.st_size == 0) {
        create_file = 1;
        r = ftruncate(db->kv_fd, KV_PAGE_ROUND_UP(db, first_mapping_size));
        if (r < 0) {
            close(db->kv_fd);
            // close file.
            fprintf(stderr, "truncate failed\n");
            return -1;
        }
        memcpy(data, MARKER, 4);
        // Write an invalid version while creating the DB.
        uint32_t version = 0;
        memcpy(data + 4, &version, 4);
        h64_to_bytes(&data[4 + 4], firstmaxcount);
        data[4 + 4 + 8] = db->kv_compression_type;
        write(db->kv_fd, data, sizeof(data));
        
        kv_table_header_write(db, KV_HEADER_SIZE, firstmaxcount);
        
        fsync(db->kv_fd);
        
        // Let's write a valid version.
        h32_to_bytes(data, VERSION);
        pwrite(db->kv_fd, data, sizeof(data), 4);
        fsync(db->kv_fd);
        
        char * filename = (char *) alloca(strlen(db->kv_filename) + strlen(".journal") + 1);
        filename[0] = 0;
        strcat(filename, db->kv_filename);
        strcat(filename, ".journal");
        unlink(filename);
    }
    
    char marker[4];
    uint32_t version;
    int compression_type;
    pread(db->kv_fd, data, sizeof(data), 0);
    memcpy(marker, data, 4);
    version = bytes_to_h32(&data[4]);
    firstmaxcount = bytes_to_h64(&data[4 + 4]);
    compression_type = data[4 + 4 + 8];
    
    r = memcmp(marker, MARKER, 4);
    if (r != 0) {
        fprintf(stderr, "file corrupted\n");
        return -1;
    }
    if (version != VERSION) {
        fprintf(stderr, "bad file version\n");
        return -1;
    }
    
    db->kv_firstmaxcount = firstmaxcount;
    db->kv_compression_type = compression_type;
    db->kv_opened = 1;
    
    r = kv_tables_setup(db);
    if (r < 0) {
        fprintf(stderr, "can't map files\n");
        return -1;
    }
    
    char * first_mapping = db->kv_first_table->kv_mapping.kv_bytes;
    db->kv_filesize = (uint64_t *) (first_mapping + KV_HEADER_FILESIZE_OFFSET);
    db->kv_free_blocks = (uint64_t *) (first_mapping + KV_HEADER_FREELIST_OFFSET);
    if (create_file) {
        * db->kv_filesize = hton64(first_mapping_size);
    }
    
    return 0;
}

void kvdb_close(kvdb * db)
{
    if (!db->kv_opened) {
        return;
    }
    
    kv_tables_unsetup(db);
    close(db->kv_fd);
    db->kv_opened = 0;
}

void kvdb_transaction_begin(kvdb * db)
{
    db->kv_transaction = new kvdb_transaction();
    uint64_t offset = ntoh64(* db->kv_filesize);
    db->kv_transaction->filesize = offset;
    
    struct kvdb_table * current_table = db->kv_first_table;
    while (current_table != NULL) {
        kvdb_transaction_table table;
        table.offset = current_table->kv_offset;
        table.count = ntoh64(* current_table->kv_count);
        table.maxcount = ntoh64(* current_table->kv_maxcount);
        table.bloomsize = ntoh64(* current_table->kv_bloom_filter_size);
        db->kv_transaction->tables.push_back(table);
        current_table = current_table->kv_next_table;
    }
    for(unsigned int i = 0 ; i < 64 ; i ++) {
        db->kv_transaction->first_recycled_blocks[i] = ntoh64(db->kv_free_blocks[i]);
    }
}

void kvdb_transaction_abort(kvdb * db)
{
    uint64_t offset = ntoh64(* db->kv_filesize);
    int r = ftruncate(db->kv_fd, offset);
    if (r < 0) {
        // could not truncate. ignore.
    }
    delete db->kv_transaction;
    db->kv_transaction = NULL;
}

static inline std::string string_with_uint64(int64_t value)
{
    std::string buffer;
    value = hton64(value);
    buffer.append((char *) &value, sizeof(value));
    return buffer;
}

int kvdb_transaction_commit(kvdb * db)
{
    // 1. fsync kvdb: it will write created blocks and tables.
    fsync(db->kv_fd);
    
    // compute journal, file size, tables, links for blocks, recycled blocks => list of {offset, size, data}
    std::map<uint64_t, std::string> writes;
    std::map<uint64_t, std::string>::iterator it;
    // file size.
    writes.insert(std::pair<uint64_t, std::string>(KV_HEADER_FILESIZE_OFFSET, string_with_uint64(db->kv_transaction->filesize)));
    // tables.
    unsigned int tables_count = 0;
    struct kvdb_table * current_table = db->kv_first_table;
    while (current_table != NULL) {
        tables_count ++;
        current_table = current_table->kv_next_table;
    }
    for(unsigned int i = 0 ; i < db->kv_transaction->tables.size() ; i ++) {
        writes.insert(std::pair<uint64_t, std::string>(db->kv_transaction->tables[i].offset + 8, string_with_uint64(db->kv_transaction->tables[i].count)));
        if (i >= tables_count) {
            if (i == db->kv_transaction->tables.size() - 1) {
                writes.insert(std::pair<uint64_t, std::string>(db->kv_transaction->tables[i].offset, string_with_uint64(0)));
            }
            else {
                writes.insert(std::pair<uint64_t, std::string>(db->kv_transaction->tables[i].offset, string_with_uint64(db->kv_transaction->tables[i + 1].offset)));
            }
        }
    }
    // recycled blocks.
    uint64_t recycled_data[64];
    for(unsigned int i = 0 ; i < 64 ; i ++) {
        if (db->kv_transaction->recycled_blocks[i].size() > 0) {
            recycled_data[i] = hton64(db->kv_transaction->recycled_blocks[i][0]);
        }
        else {
            recycled_data[i] = hton64(db->kv_transaction->first_recycled_blocks[i]);
        }
    }
    writes.insert(std::pair<uint64_t, std::string>(KV_HEADER_FREELIST_OFFSET, std::string((char *) recycled_data, sizeof(recycled_data))));
    // links for recycled blocks.
    for(unsigned int i = 0 ; i < 64 ; i ++) {
        for(unsigned int k = 0 ; k < db->kv_transaction->recycled_blocks[i].size() ; k ++) {
            if (k == db->kv_transaction->recycled_blocks[i].size() - 1) {
                writes.insert(std::pair<uint64_t, std::string>(db->kv_transaction->recycled_blocks[i][k], string_with_uint64(db->kv_transaction->first_recycled_blocks[i])));
            }
            else {
                writes.insert(std::pair<uint64_t, std::string>(db->kv_transaction->recycled_blocks[i][k], string_with_uint64(db->kv_transaction->recycled_blocks[i][k + 1])));
            }
        }
    }
    // links between data blocks.
    {
        //fprintf(stderr, "blocks to write: %i\n", (int) db->kv_transaction->items.size());
        std::unordered_map<std::string, kvdb_transaction_item>::iterator it = db->kv_transaction->items.begin();
        while (it != db->kv_transaction->items.end()) {
            uint64_t items_offset = db->kv_transaction->tables[it->second.table_index].offset + KV_TABLE_ITEMS_OFFSET_OFFSET(db->kv_transaction->tables[it->second.table_index].maxcount);
            if (it->second.block_offsets.size() == 0) {
                writes.insert(std::pair<uint64_t, std::string>(items_offset + 8 * it->second.cell_index, string_with_uint64(0)));
            }
            else {
                writes.insert(std::pair<uint64_t, std::string>(items_offset + 8 * it->second.cell_index, string_with_uint64(it->second.block_offsets[0])));
            }
            for(unsigned int k = 0 ; k < it->second.block_offsets.size() ; k ++) {
                if (k == it->second.block_offsets.size() - 1) {
                    writes.insert(std::pair<uint64_t, std::string>(it->second.block_offsets[k], string_with_uint64(0)));
                }
                else {
                    writes.insert(std::pair<uint64_t, std::string>(it->second.block_offsets[k], string_with_uint64(it->second.block_offsets[k + 1])));
                }
            }
            it ++;
        }
    }
    
    // bloom filter tables modifications.
    current_table = db->kv_first_table;
    for(unsigned int i = 0 ; i < db->kv_transaction->tables.size() ; i ++) {
        std::unordered_map<uint64_t, uint8_t>::iterator it = db->kv_transaction->tables[i].bloom_table.begin();
        while (it != db->kv_transaction->tables[i].bloom_table.end()) {
            uint64_t offset = db->kv_transaction->tables[i].offset + KV_TABLE_BLOOM_FILTER_OFFSET + it->first;
            if (i >= tables_count) {
                writes.insert(std::pair<uint64_t, std::string>(offset, std::string((char *) &(it->second), 1)));
            }
            else {
                uint8_t value = current_table->kv_bloom_filter[it->first] | it->second;
                writes.insert(std::pair<uint64_t, std::string>(offset, std::string((char *) &value, 1)));
            }
            it ++;
        }
        if (i < tables_count) {
            current_table = current_table->kv_next_table;
        }
    }
    
    // 2. write journal to disk (list of {offset, size, data})
    int r;
    off_t journal_size = 0;
    void * mapping = NULL;
    char * journal_current = NULL;
    uint32_t checksum = 0;
    char * filename = (char *) alloca(strlen(db->kv_filename) + strlen(".journal") + 1);
    filename[0] = 0;
    strcat(filename, db->kv_filename);
    strcat(filename, ".journal");
    int fd_journal = open(filename, O_RDWR | O_CREAT, 0600);
    it = writes.begin();
    while (it != writes.end()) {
        journal_size += sizeof(uint64_t);
        journal_size += sizeof(uint16_t);
        journal_size += it->second.length();
        it ++;
    }
    
    ftruncate(fd_journal, journal_size + 8);
    mapping = mmap(NULL, journal_size + 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd_journal, 0);
    if (mapping == (void *) MAP_FAILED) {
        goto transaction_failed;
    }
    memcpy(mapping, "KVJL", 4);
    it = writes.begin();
    journal_current = ((char *) mapping) + 8;
    
    while (it != writes.end()) {
        uint64_t offset = hton64(it->first);
        uint16_t size = htons(it->second.length());
        const char * data = it->second.c_str();
        memcpy(journal_current, &offset, sizeof(offset));
        journal_current += sizeof(uint64_t);
        memcpy(journal_current, &size, sizeof(size));
        journal_current += sizeof(uint16_t);
        memcpy(journal_current, data, it->second.length());
        journal_current += it->second.length();
        it ++;
    }
    checksum = kv_murmur_hash(((const char *) mapping) + 8, journal_size, 0);
    checksum = htonl(checksum);
    memcpy(((char *) mapping) + 4, &checksum, sizeof(checksum));
    munmap(mapping, journal_size + 8);
    fsync(fd_journal);
    close(fd_journal);
    
    // 6. restore journal (kvdb_restore_journal)
    r = kvdb_restore_journal(db);
    if (r < 0) {
        goto transaction_failed;
    }
    
    current_table = db->kv_first_table;
    while (current_table != NULL) {
        if (current_table->kv_next_table != NULL) {
            current_table = current_table->kv_next_table;
        }
        else {
            break;
        }
    }
    if (tables_count < db->kv_transaction->tables.size()) {
        r = kv_map_table(db, &current_table->kv_next_table, db->kv_transaction->tables[tables_count].offset);
        if (r < 0) {
            goto transaction_failed;
        }
    }
    
    return 0;
    
transaction_failed:
    if (fd_journal != -1) {
        close(fd_journal);
    }
    unlink(filename);
    kvdb_transaction_abort(db);
    return -2;
}

// journal format:
// KVJL
// 0 or 1 (1: valid): 32 bits
// offset: 64 bits, size: 16 bits, data

#define DEFAULT_MAPPING_SIZE (256 * 1024)

static int kvdb_restore_journal(kvdb * db)
{
    char * filename = (char *) alloca(strlen(db->kv_filename) + strlen(".journal") + 1);
    filename[0] = 0;
    strcat(filename, db->kv_filename);
    strcat(filename, ".journal");
    struct stat stat_buf;
    char * journal = NULL;
    int fd = -1;
    int r;
    char * journal_current;
    size_t remaining;
    unsigned int changes = 0;
    uint32_t checksum;
    uint32_t stored_checksum;
    void * current_mapping = NULL;
    size_t current_mapping_size = 0;
    uint64_t current_mapping_offset = 0;
    
    r = stat(filename, &stat_buf);
    if (r < 0) {
        return 0;
    }
    
    // 1. if the journal not valid, remove it.
    if (stat_buf.st_size < 8) {
        goto invalid_journal;
    }
    fprintf(stderr, "restore journal %llu\n", (unsigned long long) stat_buf.st_size);
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        goto invalid_journal;
    }
    journal = (char *) mmap(NULL, stat_buf.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (journal == (void *) MAP_FAILED) {
        goto invalid_journal;
    }
    journal_current = journal;
    remaining = stat_buf.st_size;
    if (memcmp(journal_current, "KVJL", 4) != 0) {
        goto invalid_journal;
    }
    journal_current += 4;
    remaining -= 4;
    stored_checksum = ntohl(* (uint32_t *) journal_current);
    journal_current += 4;
    remaining -= 4;
    
    checksum = kv_murmur_hash(journal_current, stat_buf.st_size - 8, 0);
    if (checksum != stored_checksum) {
        goto invalid_journal;
    }
    
    // 3. write changes to disk.
    while (remaining > 0) {
        uint64_t offset;
        uint16_t data_size;
        char * data;
        
        offset = ntoh64(* (uint64_t *) journal_current);
        journal_current += 8;
        remaining -= 8;
        data_size = ntohs(* (uint16_t *) journal_current);
        journal_current += 2;
        remaining -= 2;
        data = journal_current;
        journal_current += data_size;
        remaining -= data_size;
        
        changes ++;
        
        if ((current_mapping == NULL) || (offset + data_size > current_mapping_offset + current_mapping_size)) {
            if (current_mapping != NULL) {
                munmap(current_mapping, current_mapping_size);
            }
            current_mapping_offset = (offset / db->kv_pagesize) * db->kv_pagesize;
            current_mapping = mmap(NULL, DEFAULT_MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, db->kv_fd, current_mapping_offset);
            current_mapping_size = DEFAULT_MAPPING_SIZE;
        }
        if (data_size == 8) {
            * (uint64_t * ) (((char *) current_mapping) + offset - current_mapping_offset) = * (uint64_t *) data;
        }
        else if (data_size == 4) {
            * (uint32_t * ) (((char *) current_mapping) + offset - current_mapping_offset) = * (uint32_t *) data;
        }
        else {
            memcpy(((char *) current_mapping) + offset - current_mapping_offset, data, data_size);
        }
    }
    //fprintf(stderr, "wrote %u changes\n", (unsigned int) changes);
    
    if (current_mapping != NULL) {
        munmap(current_mapping, current_mapping_size);
        current_mapping = NULL;
    }
    
    munmap(journal, stat_buf.st_size);
    // 4. fsync kvdb
    fsync(fd);
    close(fd);
    
    //fprintf(stderr, "committed journal successfully - %u changes\n", changes);
    // 5. remove journal
    unlink(filename);
    
    return 0;
    
invalid_journal:
    if (journal != NULL) {
        munmap(journal, stat_buf.st_size);
    }
    if (fd != -1) {
        close(fd);
    }
    unlink(filename);
    return -1;
    
write_failed:
    return -2;
}

static kvdb_transaction_item * collect_blocks(kvdb * db, unsigned int table_index, uint32_t cell_index, std::string & transaction_key)
{
    // Select the correct table.
    struct kvdb_table * table = db->kv_first_table;
    for(unsigned int i = 0 ; i < table_index ; i ++) {
        table = table->kv_next_table;
        if (table == NULL) {
            break;
        }
    }
    
    kvdb_transaction_item transaction_item;
    transaction_item.changed = false;
    transaction_item.cell_index = cell_index;
    transaction_item.table_index = table_index;
    
    if (table == NULL) {
        // the table doesn't even exist yet.
    }
    else {
        struct kvdb_item * item = &table->kv_items[cell_index];
        uint64_t next_offset = ntoh64(item->kv_offset);
        
        // Run through all chained blocks in the bucket.
        while (next_offset != 0) {
            uint64_t current_offset;
            ssize_t r;
            
            current_offset = next_offset;
            transaction_item.block_offsets.push_back(current_offset);
            
            char block_header_data[8];
            r = pread(db->kv_fd, block_header_data, sizeof(block_header_data), (off_t) next_offset);
            if (r < 0)
                return NULL;
            char * p = block_header_data;
            next_offset = bytes_to_h64(p);
            p += 8;
        }
    }

    db->kv_transaction->items.insert(std::pair<std::string, kvdb_transaction_item>(transaction_key, transaction_item));
    std::unordered_map<std::string, kvdb_transaction_item>::iterator iterator = db->kv_transaction->items.find(transaction_key);
    return &iterator->second;
}

static int internal_kvdb_set(kvdb * db, const char * key, size_t key_size, const char * value, size_t value_size)
{
    KVDBAssert(db->kv_transaction != NULL);
    
    int r;
    r = kvdb_delete(db, key, key_size);
    if (r == -1) {
        // Not found: ignore.
    }
    else if (r == -2) {
        return -2;
    }
    
    uint32_t hash_values[KV_BLOOM_FILTER_HASH_COUNT];
    table_bloom_filter_compute_hash(hash_values, KV_BLOOM_FILTER_HASH_COUNT, key, key_size);
    
    uint32_t table_index = 0;
    while (table_index < db->kv_transaction->tables.size()) {
        if (db->kv_transaction->tables[table_index].count < db->kv_transaction->tables[table_index].maxcount * KV_MAX_MEAN_COLLISION) {
            break;
        }
        table_index ++;
    }
    
    if (table_index >= db->kv_transaction->tables.size()) {
        kvdb_transaction_table table;
        table.count = 0;
        uint64_t nextsize = kv_getnextprime(db->kv_transaction->tables[table_index - 1].maxcount * 2);
        table.maxcount = nextsize;
        
        uint64_t offset = kv_table_create(db, nextsize, &db->kv_current_table->kv_next_table);
        if (offset == 0) {
            return -1;
        }
        table.offset = offset;
        
        table.bloomsize = kv_getnextprime(nextsize * KV_TABLE_BITS_FOR_BLOOM_FILTER);
        
        db->kv_transaction->tables.push_back(table);
    }
    
    table_transaction_bloom_filter_set(db, table_index, hash_values + 1, KV_BLOOM_FILTER_HASH_COUNT - 1);
    
    uint32_t cell_index = hash_values[0] % db->kv_transaction->tables[table_index].maxcount;
    
    std::string transaction_key;
    transaction_key.append((const char *) &table_index, sizeof(table_index));
    transaction_key.append((const char *) &cell_index, sizeof(cell_index));
    
    kvdb_transaction_item * p_item = NULL;
    std::unordered_map<std::string, kvdb_transaction_item>::iterator found_item_iterator = db->kv_transaction->items.find(transaction_key);
    if (found_item_iterator == db->kv_transaction->items.end()) {
        p_item = collect_blocks(db, table_index, cell_index, transaction_key);
        KVDBAssert(p_item != NULL);
    }
    else {
        p_item = &found_item_iterator->second;
    }
    
    uint64_t offset = kv_block_create(db, 0, hash_values[0], key, key_size, value, value_size);
    p_item->block_offsets.push_back(offset);
    p_item->changed = true;
    
    db->kv_transaction->tables[table_index].count ++;
    
    return 0;
}

int kvdb_set(kvdb * db, const char * key, size_t key_size, const char * value, size_t value_size)
{
#warning implement implicit creation of transaction and flush.
    if (db->kv_compression_type == KVDB_COMPRESSION_TYPE_RAW) {
        return internal_kvdb_set(db, key, key_size, value, value_size);
    }
    else if (db->kv_compression_type == KVDB_COMPRESSION_TYPE_LZ4) {
        if (value_size == 0) {
            return internal_kvdb_set(db, key, key_size, value, value_size);
        }
        else {
            int max_compressed_size = LZ4_compressBound((int) value_size);
            char * compressed_value = NULL;
            int allocated = 0;
            if (max_compressed_size < 4096) {
                compressed_value = (char *) alloca(sizeof(uint32_t) + max_compressed_size);
            }
            else {
                allocated = 1;
                compressed_value = (char *) malloc(sizeof(uint32_t) + max_compressed_size);
            }
            * (uint32_t *) compressed_value = htonl(value_size);
            int compressed_value_size = LZ4_compress(value, compressed_value + sizeof(uint32_t), (int) value_size);
            int r = internal_kvdb_set(db, key, key_size, compressed_value, sizeof(uint32_t) + compressed_value_size);
            if (allocated) {
                free(compressed_value);
            }
            return r;
        }
    }
    else {
        KVDBAssert(0);
        return 0;
    }
}

static void show_bucket(kvdb * db, uint32_t idx)
{
    struct kvdb_table * table = db->kv_first_table;
    struct kvdb_item * item = &table->kv_items[idx];
    uint64_t next_offset = ntoh64(item->kv_offset);
    
    fprintf(stderr, "bucket: %llu\n", (unsigned long long) idx);
    
    uint64_t previous_offset = 0;
    
    // Run through all chained blocks in the bucket.
    while (next_offset != 0) {
        uint32_t current_hash_value;
        uint64_t current_offset;
        uint8_t log2_size;
        uint64_t current_key_size;
        char * current_key;
        ssize_t r;
        
        current_offset = next_offset;
        char block_header_data[KV_BLOCK_KEY_BYTES_OFFSET + PRE_READ_KEY_SIZE];
        
        r = pread(db->kv_fd, block_header_data, sizeof(block_header_data), (off_t) next_offset);
        if (r < 0)
            return;
        char * p = block_header_data;
        next_offset = bytes_to_h64(p);
        p += 8;
        current_hash_value = bytes_to_h32(p);
        p += 4;
        log2_size = bytes_to_h8(p);
        p += 1;
        current_key_size = bytes_to_h64(p);
        p += 8;
        current_key = block_header_data + KV_BLOCK_KEY_BYTES_OFFSET;
        
        fprintf(stderr, "previous, current, next: %llu, %llu , %llu\n", (unsigned long long) previous_offset, (unsigned long long) current_offset, (unsigned long long) next_offset);
        fprintf(stderr, "hash: %llu\n", (unsigned long long) current_hash_value);
        
        char * allocated = NULL;
        if (current_key_size > PRE_READ_KEY_SIZE) {
            if (current_key_size <= MAX_ALLOCA_SIZE) {
                current_key = (char *) alloca(current_key_size);
            }
            else {
                allocated = (char *) malloc((size_t) current_key_size);
                current_key = allocated;
            }
            r = pread(db->kv_fd, current_key, (size_t) current_key_size, (off_t) (current_offset + KV_BLOCK_KEY_BYTES_OFFSET));
            if (r < 0) {
                if (allocated != NULL) {
                    free(allocated);
                }
                return;
            }
        }
        fprintf(stderr, "key: %.*s\n", (int) current_key_size, current_key);
        if (allocated != NULL) {
            free(allocated);
        }
        previous_offset = current_offset;
    }
    fprintf(stderr, "-----\n");
}

static int match_block_with_key(kvdb * db, uint64_t offset, uint32_t hash_value, const char * key, size_t key_size, struct find_key_cb_params * params)
{
    uint32_t current_hash_value;
    uint8_t log2_size;
    uint64_t current_key_size;
    uint64_t next_offset;
    char * current_key;
    ssize_t r;
    
    char block_header_data[KV_BLOCK_KEY_BYTES_OFFSET + PRE_READ_KEY_SIZE];
    
    r = pread(db->kv_fd, block_header_data, sizeof(block_header_data), (off_t) offset);
    if (r < 0)
        return -1;
    char * p = block_header_data;
    next_offset = bytes_to_h64(p);
    params->next_offset = next_offset;
    p += 8;
    current_hash_value = bytes_to_h32(p);
    p += 4;
    log2_size = bytes_to_h8(p);
    p += 1;
    current_key_size = bytes_to_h64(p);
    p += 8;
    current_key = block_header_data + KV_BLOCK_KEY_BYTES_OFFSET;
    
    if (current_hash_value != hash_value) {
        return 0;
    }
    
    int cmp_result;
    
    if (current_key_size != key_size) {
        return 0;
    }
    char * allocated = NULL;
    if (current_key_size > PRE_READ_KEY_SIZE) {
        if (current_key_size <= MAX_ALLOCA_SIZE) {
            current_key = (char *) alloca(current_key_size);
        }
        else {
            allocated = (char *) malloc((size_t) current_key_size);
            current_key = allocated;
        }
        r = pread(db->kv_fd, current_key, (size_t) current_key_size, (off_t) (offset + KV_BLOCK_KEY_BYTES_OFFSET));
        if (r < 0) {
            if (allocated != NULL) {
                free(allocated);
            }
            return -1;
        }
    }
    cmp_result = memcmp(key, current_key, key_size) != 0;
    if (allocated != NULL) {
        free(allocated);
    }
    if (cmp_result != 0) {
        return 0;
    }
    
    params->key = key;
    params->key_size = key_size;
    params->current_offset = offset;
    params->log2_size = log2_size;
    
    return 1;
}

static int find_key(kvdb * db, const char * key, size_t key_size,
                    findkey_callback callback, void * cb_data)
{
    int r;
    uint32_t hash_values[KV_BLOOM_FILTER_HASH_COUNT];
    table_bloom_filter_compute_hash(hash_values, KV_BLOOM_FILTER_HASH_COUNT, key, key_size);
    
    if (db->kv_transaction != NULL) {
        for(uint32_t i = 0 ; i < db->kv_transaction->tables.size() ; i ++) {
            uint32_t cell_index = hash_values[0] % db->kv_transaction->tables[i].maxcount;
            
            std::string transaction_key;
            transaction_key.append((const char *) &i, sizeof(i));
            transaction_key.append((const char *) &cell_index, sizeof(cell_index));
            std::unordered_map<std::string, kvdb_transaction_item>::iterator iterator = db->kv_transaction->items.find(transaction_key);
            if (iterator != db->kv_transaction->items.end()) {
                kvdb_transaction_item * item = &iterator->second;
                for(unsigned int k = 0 ; k < item->block_offsets.size() ; k ++) {
                    struct find_key_cb_params params;
                    r = match_block_with_key(db, item->block_offsets[k], hash_values[0], key, key_size, &params);
                    if (r < 0) {
                        return r;
                    }
                    if (r == 1) {
                        params.table_index = i;
                        params.cell_index = cell_index;
                        params.is_transaction = 1;
                        callback(db, &params, cb_data);
                        return 0;
                    }
                }
            }
        }
    }
    
    // Run through all tables.
    struct kvdb_table * table = db->kv_first_table;
    uint32_t table_index = 0;
    while (table != NULL) {
        // Is the key likely to be in this table?
        // Use a bloom filter to guess.
        if (!table_bloom_filter_might_contain(table, hash_values + 1, KV_BLOOM_FILTER_HASH_COUNT - 1)) {
            table_index ++;
            table = table->kv_next_table;
            continue;
        }
        
        // Find a bucket.
        uint32_t idx = hash_values[0] % ntoh64(* table->kv_maxcount);
        if (db->kv_transaction != NULL) {
            // skip if the bucket is in transaction.
            std::string transaction_key;
            transaction_key.append((const char *) &table_index, sizeof(table_index));
            transaction_key.append((const char *) &idx, sizeof(idx));
            if (db->kv_transaction->items.find(transaction_key) != db->kv_transaction->items.end()) {
                table_index ++;
                table = table->kv_next_table;
                continue;
            }
        }
        struct kvdb_item * item = &table->kv_items[idx];
        uint64_t next_offset = ntoh64(item->kv_offset);
        if (kvdb_debug) {
            fprintf(stderr, "before\n");
            show_bucket(db, idx);
        }
        
        // Run through all chained blocks in the bucket.
        while (next_offset != 0) {
            uint64_t current_offset = next_offset;
            struct find_key_cb_params params;
            r = match_block_with_key(db, current_offset, hash_values[0], key, key_size, &params);
            if (r < 0) {
                return r;
            }
            if (r == 1) {
                params.table_index = table_index;
                params.cell_index = idx;
                params.is_transaction = 0;
                callback(db, &params, cb_data);
                return 0;
            }
            
            next_offset = params.next_offset;
        }
        table_index ++;
        table = table->kv_next_table;
    }

    return 0;
}

struct delete_key_params {
    int result;
    int found;
};

static void delete_key_callback(kvdb * db, struct find_key_cb_params * params,
                                void * data)
{
    struct delete_key_params * deletekeyparams = (struct delete_key_params *) data;
    kvdb_transaction_item * p_item = NULL;
    if (params->is_transaction) {
        std::string transaction_key;
        transaction_key.append((const char *) &params->table_index, sizeof(params->table_index));
        transaction_key.append((const char *) &params->cell_index, sizeof(params->cell_index));
        std::unordered_map<std::string, kvdb_transaction_item>::iterator found_item_iterator = db->kv_transaction->items.find(transaction_key);
        p_item = &found_item_iterator->second;
    }
    else {
        std::string transaction_key;
        transaction_key.append((const char *) &params->table_index, sizeof(params->table_index));
        transaction_key.append((const char *) &params->cell_index, sizeof(params->cell_index));
        p_item = collect_blocks(db, params->table_index, params->cell_index, transaction_key);
        KVDBAssert(p_item != NULL);
    }
    for(unsigned int i = 0 ; i < p_item->block_offsets.size() ; i ++) {
        if (p_item->block_offsets[i] == params->current_offset) {
            p_item->block_offsets.erase(p_item->block_offsets.begin() + i);
            p_item->changed = true;
            break;
        }
    }
    kv_block_recycle(db, params->current_offset);
    db->kv_transaction->tables[params->table_index].count --;
    deletekeyparams->result = 0;
    deletekeyparams->found = 1;
}

int kvdb_delete(kvdb * db, const char * key, size_t key_size)
{
    KVDBAssert(db->kv_transaction != NULL);
    
    int r;
    struct delete_key_params data;
    
    data.found = 0;
    data.result = -1;
    
    r = find_key(db, key, key_size, delete_key_callback, &data);
    if (r < 0) {
        return -2;
    }
    if (data.result < 0) {
        return data.result;
    }
    if (!data.found) {
        return -1;
    }
    
    return 0;
}

struct read_value_params {
    uint64_t value_size;
    char * value;
    int result;
    int found;
    size_t free_size;
};

static void read_value_callback(kvdb * db, struct find_key_cb_params * params,
                                void * data)
{
    struct read_value_params * readparams = (struct read_value_params *) data;
    ssize_t r;
    
    uint64_t value_size;
    r = pread(db->kv_fd, &value_size, sizeof(value_size),
              params->current_offset + 8 + 4 + 1 + 8 + params->key_size);
    if (r < 0) {
        readparams->result = -2;
        return;
    }
    
    value_size = ntoh64(value_size);
    readparams->value_size = value_size;
    readparams->value = (char *) malloc((size_t) value_size);
    
    r = pread(db->kv_fd, readparams->value, (size_t) value_size,
              params->current_offset + 8 + 4 + 1 + 8 + params->key_size + 8);
    if (r < 0) {
        readparams->result = -2;
        free(readparams->value);
        return;
    }
    
    readparams->result = 0;
    readparams->found = 1;
    readparams->free_size = (1 << params->log2_size) - (value_size + params->key_size);
}

int kvdb_get(kvdb * db, const char * key, size_t key_size,
             char ** p_value, size_t * p_value_size)
{
    return kvdb_get2(db, key, key_size, p_value, p_value_size, NULL);
}

static int kvdb_get2(kvdb * db, const char * key, size_t key_size,
                     char ** p_value, size_t * p_value_size, size_t * p_free_size)
{
    if (db->kv_compression_type == KVDB_COMPRESSION_TYPE_RAW) {
        return internal_kvdb_get2(db, key, key_size, p_value, p_value_size, p_free_size);
    }
    else if (db->kv_compression_type == KVDB_COMPRESSION_TYPE_LZ4) {
        char * compressed_value;
        size_t compressed_value_size;
        int r = internal_kvdb_get2(db, key, key_size, &compressed_value, &compressed_value_size, p_free_size);
        if (r < 0) {
            return r;
        }
        if (compressed_value_size == 0) {
            * p_value = NULL;
            * p_value_size = 0;
            return 0;
        }
    
        size_t value_size = ntohl(* (uint32_t *) compressed_value);
        char * value = (char *) malloc(value_size);
        LZ4_decompress_fast(compressed_value + sizeof(uint32_t), value, (int) value_size);
        free(compressed_value);
        if (p_free_size != NULL) {
            * p_free_size = 0;
        }
        * p_value_size = value_size;
        * p_value = value;
        return 0;
    }
    else {
        KVDBAssert(0);
        return 0;
    }
}

static int internal_kvdb_get2(kvdb * db, const char * key, size_t key_size,
              char ** p_value, size_t * p_value_size, size_t * p_free_size)
{
    int r;
    struct read_value_params data;
    
    data.value_size = 0;
    data.value = NULL;
    data.result = -1;
    data.found = 0;
    data.free_size = 0;

    r = find_key(db, key, key_size, read_value_callback, &data);
    if (r < 0) {
        return -2;
    }
    if (data.result < 0) {
        return data.result;
    }
    if (!data.found) {
        return -1;
    }
    
    if (p_free_size != NULL) {
        * p_free_size = data.free_size;
    }
    
    * p_value = data.value;
    * p_value_size = (size_t) data.value_size;
    
    return 0;
}

static int enumerate_offset(kvdb * db, uint64_t current_offset,
                            kvdb_enumerate_callback callback, void * cb_data, int * stop,
                            uint64_t * next_block_offset)
{
    struct kvdb_enumerate_cb_params cb_params;
    char block_header_data[KV_BLOCK_KEY_BYTES_OFFSET + PRE_READ_KEY_SIZE];
    ssize_t r = pread(db->kv_fd, block_header_data, sizeof(block_header_data), (off_t) current_offset);
    if (r < 0) {
        return -2;
    }
    char * p = block_header_data;
    if (next_block_offset != NULL) {
        * next_block_offset = bytes_to_h64(p);
    }
    p += 8+4+1; // ignore hash_value and log2_size
    size_t current_key_size = (size_t) bytes_to_h64(p);
    p += 8;
    char * current_key = block_header_data + KV_BLOCK_KEY_BYTES_OFFSET;
    char * allocated = NULL;
    if (current_key_size > PRE_READ_KEY_SIZE) {
        if (current_key_size <= MAX_ALLOCA_SIZE) {
            current_key = (char *) alloca(current_key_size);
        }
        else {
            allocated = (char *) malloc(current_key_size);
            current_key = allocated;
        }
        r = pread(db->kv_fd, current_key, current_key_size, (off_t) (current_offset + KV_BLOCK_KEY_BYTES_OFFSET));
        if (r < 0) {
            if (allocated != NULL) {
                free(allocated);
            }
            return -2;
        }
    }
    cb_params.key = current_key;
    cb_params.key_size = current_key_size;
    callback(db, &cb_params, cb_data, stop);
    if (allocated != NULL) {
        free(allocated);
    }
    return 0;
}

int kvdb_enumerate_keys(kvdb * db, kvdb_enumerate_callback callback, void * cb_data)
{
    struct kvdb_table * table = db->kv_first_table;
    int stop = 0;
	
    if (db->kv_transaction != NULL) {
        std::unordered_map<std::string, kvdb_transaction_item>::iterator iterator = db->kv_transaction->items.begin();
        while (iterator != db->kv_transaction->items.end()) {
            kvdb_transaction_item * item = &iterator->second;
            for(unsigned int i = 0 ; i < item->block_offsets.size() ; i ++) {
                uint64_t current_offset = item->block_offsets[i];
                int r = enumerate_offset(db, current_offset, callback, cb_data, &stop, NULL);
                if (r < 0) {
                    return r;
                }
                if (stop) {
                    return 0;
                }
            }
            iterator ++;
        }
    }
    
    // Run through all tables.
    uint32_t table_index = 0;
    while (table != NULL) {
		struct kvdb_item * item = table->kv_items;
		// Run through all buckets.
		uint64_t count = ntoh64(* table->kv_maxcount);
        uint32_t cell_index = 0;
		while (count) {
            if (db->kv_transaction != NULL) {
                std::string transaction_key;
                transaction_key.append((const char *) &table_index, sizeof(table_index));
                transaction_key.append((const char *) &cell_index, sizeof(cell_index));
                std::unordered_map<std::string, kvdb_transaction_item>::iterator iterator = db->kv_transaction->items.find(transaction_key);
                if (iterator != db->kv_transaction->items.end()) {
                    item ++;
                    count --;
                    cell_index ++;
                    continue;
                }
            }
			uint64_t current_offset = ntoh64(item->kv_offset);
			// Run through all chained blocks in the bucket.
			while (current_offset != 0) {
                uint64_t next_offset;
                int r = enumerate_offset(db, current_offset, callback, cb_data, &stop, &next_offset);
                if (r < 0) {
                    return r;
                }
				if (stop) {
					return 0;
				}
                current_offset = next_offset;
			}
			item ++;
			count --;
            cell_index ++;
		}
		table = table->kv_next_table;
        table_index ++;
	}
	return 0;
}