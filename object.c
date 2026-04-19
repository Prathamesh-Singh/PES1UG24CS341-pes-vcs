// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}


void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}
 
int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}
// ─── TODO ────────────────────────────────────────────────────────────────────
 
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build header string e.g. "blob 42\0"
    const char *type_str = (type == OBJ_BLOB)   ? "blob"   :
                           (type == OBJ_TREE)   ? "tree"   : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
 
    // Step 2: Concatenate header + data into one buffer
    size_t total = (size_t)hlen + len;
    uint8_t *full = malloc(total);
    if (!full) return -1;
    memcpy(full, header, (size_t)hlen);
    memcpy(full + hlen, data, len);
 
    // Step 3: Compute SHA-256 of the FULL object (header + data)
    compute_hash(full, total, id_out);
 
    // Step 4: Deduplication — if already stored, skip writing
    if (object_exists(id_out)) { free(full); return 0; }
 
    // Step 5: Create shard directory (.pes/objects/XX/)
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir, 0755);
 
    // Step 6: Build final object path
    char path[300];
    object_path(id_out, path, sizeof(path));
 
    // Step 7: Write to a temp file using mkstemp (safe unique name)
    char tmp_path[316];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/tmp_XXXXXX", OBJECTS_DIR, hex);
    int fd = mkstemp(tmp_path);
    if (fd < 0) { free(full); return -1; }
    ssize_t written = write(fd, full, total);
    if (written < 0 || (size_t)written != total) {
        close(fd); unlink(tmp_path); free(full); return -1;
    }
 
    // Step 8: fsync temp file to ensure data hits disk
    fsync(fd);
    close(fd);
    free(full);
 
    // Step 9: Atomic rename — on POSIX this is guaranteed atomic
    if (rename(tmp_path, path) != 0) { unlink(tmp_path); return -1; }
 
    // Step 10: fsync the shard directory to persist the rename
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
 
    return 0;
}