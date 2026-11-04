#include "pair_store.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint8_t address[6], key[16], type; } record;
_Static_assert(sizeof(record) == 23, "pairing record format");
static record *records;
static size_t record_count;
static char folder[PATH_MAX], filename[PATH_MAX];
static int healthy = 1;
static bd_addr_t *trusted;
static size_t trusted_count;
static char trust_filename[PATH_MAX];
static int trust_healthy = 1;
static void load_trusted(void);
static const uint8_t magic[8] = {'B','T','P','A','I','R','0','1'};

static void error(void) {
    healthy = 0;
    puts("{\"event\":\"error\",\"message\":\"配对记录无法读写；原文件已保留\"}");
}

int pair_store_init(const char *directory) {
    if (!directory || strlen(directory) > sizeof(folder) - 60) return -1;
    strcpy(folder, directory);
    if (mkdir(folder, 0700) && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(folder, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return -1;
    if (chmod(folder, 0700)) return -1;
    return 0;
}

static void set_local(bd_addr_t address) {
    snprintf(filename, sizeof(filename), "%s/pairings-%02x%02x%02x%02x%02x%02x.bin",
        folder, address[0], address[1], address[2], address[3], address[4], address[5]);
    free(records); records = NULL; record_count = 0;
    load_trusted();
    int fd = open(filename, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { if (errno != ENOENT) error(); return; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_size < 8 || st.st_size > 1024*1024 || (st.st_size - 8) % sizeof(record)) {
        close(fd); error(); return;
    }
    if (fchmod(fd, 0600)) { close(fd); error(); return; }
    uint8_t header[8];
    if (read(fd, header, 8) != 8 || memcmp(header, magic, 8)) { close(fd); error(); return; }
    size_t bytes = (size_t)st.st_size - 8;
    record *loaded = bytes ? malloc(bytes) : NULL;
    if (bytes && !loaded) { close(fd); error(); return; }
    size_t offset = 0;
    while (offset < bytes) {
        ssize_t count = read(fd, (uint8_t *)loaded + offset, bytes - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { free(loaded); close(fd); error(); return; }
        offset += (size_t)count;
    }
    close(fd); records = loaded; record_count = bytes / sizeof(record);
    printf("{\"event\":\"pairings\",\"count\":%zu}\n", record_count);
    for (size_t i=0; i<record_count; i++) pair_store_trust(records[i].address);
}

static int write_all(int fd, const void *buffer, size_t length) {
    const uint8_t *p = buffer;
    while (length) {
        ssize_t count = write(fd, p, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        p += count; length -= (size_t)count;
    }
    return 0;
}

static void save(void) {
    if (!healthy || !filename[0]) return;
    char temporary[PATH_MAX];
    snprintf(temporary, sizeof(temporary), "%s/.pairings-XXXXXX", folder);
    int fd = mkstemp(temporary);
    if (fd < 0) { error(); return; }
    int failed = fchmod(fd, 0600) || write_all(fd, magic, sizeof(magic)) ||
        write_all(fd, records, record_count * sizeof(record)) || fsync(fd);
    if (close(fd)) failed = 1;
    if (!failed && rename(temporary, filename)) failed = 1;
    if (failed) { unlink(temporary); error(); return; }
    int directory = open(folder, O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { fsync(directory); close(directory); }
    printf("{\"event\":\"pairings\",\"count\":%zu}\n", record_count);
}

static size_t find(const bd_addr_t address) {
    for (size_t i = 0; i < record_count; i++) if (!memcmp(records[i].address, address, 6)) return i;
    return record_count;
}
static void db_open(void) {}
static void db_close(void) {}
static int get(bd_addr_t address, link_key_t key, link_key_type_t *type) {
    size_t index = find(address);
    if (index == record_count || !healthy) return 0;
    memcpy(key, records[index].key, 16); *type = (link_key_type_t)records[index].type; return 1;
}
static void put(bd_addr_t address, link_key_t key, link_key_type_t type) {
    if (!healthy) return;
    size_t index = find(address);
    if (index == record_count) {
        record *grown = realloc(records, (record_count + 1) * sizeof(record));
        if (!grown) { error(); return; }
        records = grown; record_count++;
    }
    memcpy(records[index].address, address, 6); memcpy(records[index].key, key, 16);
    records[index].type = (uint8_t)type; save();
    if (healthy) pair_store_trust(address);
}
static void remove_key(bd_addr_t address) {
    if (!healthy) return;
    size_t index = find(address);
    if (index == record_count) return;
    memmove(&records[index], &records[index+1], (record_count - index - 1) * sizeof(record));
    record_count--; save();
}
static int iterator_init(btstack_link_key_iterator_t *it) {
    it->context = calloc(1, sizeof(size_t)); return it->context != NULL;
}
static int iterator_next(btstack_link_key_iterator_t *it, bd_addr_t address, link_key_t key, link_key_type_t *type) {
    size_t *index = it->context;
    if (!index || *index >= record_count) return 0;
    memcpy(address, records[*index].address, 6); memcpy(key, records[*index].key, 16);
    *type = (link_key_type_t)records[*index].type; (*index)++; return 1;
}
static void iterator_done(btstack_link_key_iterator_t *it) { free(it->context); it->context = NULL; }
const btstack_link_key_db_t *pair_store_instance(void) {
    static const btstack_link_key_db_t database = {db_open, set_local, db_close, get, put,
        remove_key, iterator_init, iterator_next, iterator_done};
    return &database;
}
int pair_store_ok(void) { return healthy; }
size_t pair_store_count(void) { return record_count; }

/* Address-only consent history is independent of reusable link keys: some
 * successful SSP sessions deliberately do not bond. Never log addresses/keys. */
static const uint8_t trust_magic[8] = {'B','T','T','R','U','S','T','1'};
static int trust_contains(const bd_addr_t address) {
    for (size_t i=0; i<trusted_count; i++)
        if (!memcmp(trusted[i],address,6)) return 1;
    return 0;
}
static void trust_error(void) {
    trust_healthy=0;
    puts("{\"event\":\"error\",\"message\":\"设备信任名单无法读写；原文件已保留，请暂时允许新设备配对\"}");
}
static void load_trusted(void) {
    free(trusted);trusted=NULL;trusted_count=0;trust_healthy=1;
    snprintf(trust_filename,sizeof(trust_filename),"%s.trusted",filename);
    int fd=open(trust_filename,O_RDONLY|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT)trust_error();return;}
    struct stat st;uint8_t header[8];
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||
       st.st_size<8||st.st_size>1024*1024||(st.st_size-8)%6||
       fchmod(fd,0600)||read(fd,header,8)!=8||memcmp(header,trust_magic,8)) {
        close(fd);trust_error();return;
    }
    size_t bytes=(size_t)st.st_size-8;
    bd_addr_t *loaded=bytes?malloc(bytes):NULL;
    if(bytes&&!loaded){close(fd);trust_error();return;}
    size_t offset=0;
    while(offset<bytes){
        ssize_t n=read(fd,(uint8_t *)loaded+offset,bytes-offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){free(loaded);close(fd);trust_error();return;}
        offset+=(size_t)n;
    }
    close(fd);trusted=loaded;trusted_count=bytes/6;
}
int pair_store_is_trusted(const bd_addr_t address) {
    if(!healthy||!trust_healthy)return 0;
    return trust_contains(address)||find(address)!=record_count;
}
void pair_store_trust(const bd_addr_t address) {
    if(!healthy||!trust_healthy||!trust_filename[0]||trust_contains(address))return;
    bd_addr_t *grown=realloc(trusted,(trusted_count+1)*sizeof(bd_addr_t));
    if(!grown){trust_error();return;}
    trusted=grown;memcpy(trusted[trusted_count++],address,6);
    char temporary[PATH_MAX];snprintf(temporary,sizeof(temporary),"%s/.trust-XXXXXX",folder);
    int fd=mkstemp(temporary);
    if(fd<0){trust_error();return;}
    int failed=fchmod(fd,0600)||write_all(fd,trust_magic,8)||
        write_all(fd,trusted,trusted_count*sizeof(bd_addr_t))||fsync(fd);
    if(close(fd))failed=1;
    if(!failed&&rename(temporary,trust_filename))failed=1;
    if(failed){unlink(temporary);trust_error();return;}
    int dir=open(folder,O_RDONLY|O_DIRECTORY);
    if(dir>=0){fsync(dir);close(dir);}
}

size_t pair_store_trusted_count(void) { return trust_healthy ? trusted_count : 0; }
