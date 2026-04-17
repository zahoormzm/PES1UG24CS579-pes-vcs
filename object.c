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

    // Step 4: Deduplication — if the object already exists, skip writing
    if (object_exists(&id)) {
        free(obj);
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

    // Step 7: Write the object to a temp file (O_CREAT|O_WRONLY|O_TRUNC, mode 0644)
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(obj);
        return -1;
    }

    ssize_t written = write(fd, obj, obj_len);
    free(obj);
    if (written < 0 || (size_t)written != obj_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // Step 8: fsync the file data to disk before renaming
    fsync(fd);
    close(fd);

    // Step 9: Atomically rename temp -> final path (POSIX guarantees atomicity)
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // Step 10: fsync the shard directory to persist the directory entry
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    if (id_out) *id_out = id;
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Locate the object file
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Read the entire file into memory
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return -1; }

    if (file_size > 0 && fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    buf[file_size] = '\0';

    // Step 3: Verify integrity — recompute hash and compare to expected
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // Step 4: Find the '\0' that separates the header from the data payload
    uint8_t *null_ptr = memchr(buf, '\0', (size_t)file_size);
    if (!null_ptr) { free(buf); return -1; }

    // Step 5: Parse the type and declared size from the header
    const char *sp = memchr(buf, ' ', (size_t)(null_ptr - buf));
    if (!sp) { free(buf); return -1; }

    if      (strncmp((char *)buf, "blob ",   5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char *)buf, "tree ",   5) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char *)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Step 6: Copy the data portion (everything after the '\0') to a new buffer
    size_t header_end = (size_t)(null_ptr - buf) + 1;
    size_t data_len   = (size_t)file_size - header_end;

    // Validate: the size encoded in the header must match actual data length
    size_t declared = (size_t)strtoull(sp + 1, NULL, 10);
    if (declared != data_len) { free(buf); return -1; }

    void *data = malloc(data_len + 1);
    if (!data) { free(buf); return -1; }
    if (data_len > 0) memcpy(data, buf + header_end, data_len);
    ((char *)data)[data_len] = '\0';

    free(buf);
    *data_out = data;
    *len_out  = data_len;
    return 0;
}
