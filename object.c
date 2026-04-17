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

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
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

// ─── TODO: Implement these ──────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Map the ObjectType to its header string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Step 2: Build the full object in memory: "<type> <size>\0<data>"
    // The header is a NUL-terminated string; we include that NUL in the object.
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t obj_len = (size_t)header_len + 1 + len; // +1 for the NUL terminator

    uint8_t *obj = malloc(obj_len);
    if (!obj) return -1;
    memcpy(obj, header, (size_t)header_len);
    obj[header_len] = '\0';
    if (len > 0) memcpy(obj + header_len + 1, data, len);

    // Step 3: Compute SHA-256 of the full object (header + data)
    ObjectID id;
    compute_hash(obj, obj_len, &id);
    free(obj);

    // Step 4: Deduplication — if the object already exists, skip writing
    if (object_exists(&id)) {
        if (id_out) *id_out = id;
        return 0;
    }

    // Step 5: Build paths using the 64-char hex hash
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    // Shard directory: .pes/objects/XX/  (first 2 hex chars)
    char shard_dir[300];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

    // Final object path: .pes/objects/XX/YYYY...
    char final_path[400];
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    // Temp path for atomic rename
    char tmp_path[416];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    // Step 6: Create the shard directory (ignore EEXIST)
    mkdir(shard_dir, 0755);

    if (id_out) *id_out = id;
    // TODO: write temp file and rename (next step)
    return -1;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // TODO: Implement
    (void)id; (void)type_out; (void)data_out; (void)len_out;
    return -1;
}
