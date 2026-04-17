// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"         // needed for tree_from_index to read the staging area
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration — object_write is implemented in object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf

        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Comparator used to sort index entries by path before tree building.
static int compare_index_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Recursive helper: build and write one tree level.
// entries[0..count-1] are all index entries whose paths share the same
// prefix (already consumed). prefix_len is the byte offset into each
// path where the current level begins.
static int write_tree_level(IndexEntry *entries, int count, int prefix_len, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count && tree.count < MAX_TREE_ENTRIES) {
        const char *rel   = entries[i].path + prefix_len;
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // Plain file at this level — add a blob entry directly
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            size_t name_len = strlen(rel);
            if (name_len >= sizeof(te->name)) name_len = sizeof(te->name) - 1;
            memcpy(te->name, rel, name_len);
            te->name[name_len] = '\0';
            i++;
        } else {
            // Directory: the first path component before '/' names the subtree.
            size_t dir_len = (size_t)(slash - rel);
            char   dir_name[256];
            if (dir_len >= sizeof(dir_name)) { i++; continue; }
            memcpy(dir_name, rel, dir_len);
            dir_name[dir_len] = '\0';

            // Collect all entries whose relative path starts with "<dir_name>/"
            int sub_start      = i;
            int sub_count      = 0;
            int new_prefix_len = prefix_len + (int)dir_len + 1; // +1 for '/'

            while (i < count) {
                const char *r = entries[i].path + prefix_len;
                if (strncmp(r, dir_name, dir_len) == 0 && r[dir_len] == '/') {
                    sub_count++;
                    i++;
                } else {
                    break;
                }
            }

            // Recursively write the subtree and capture its root hash
            ObjectID sub_id;
            if (write_tree_level(entries + sub_start, sub_count, new_prefix_len, &sub_id) != 0)
                return -1;

            // Add a directory (040000) entry pointing at the subtree object
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000;
            te->hash = sub_id;
            memcpy(te->name, dir_name, dir_len);
            te->name[dir_len] = '\0';
        }
    }

    void  *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    // Index is ~5.6 MB — heap-allocate to avoid blowing the default 8 MB stack
    Index *index = malloc(sizeof(Index));
    if (!index) return -1;

    if (index_load(index) != 0) {
        free(index);
        return -1;
    }

    if (index->count == 0) {
        // Nothing staged — write an empty root tree so commit_create still works
        free(index);
        Tree empty; empty.count = 0;
        void *data; size_t data_len;
        if (tree_serialize(&empty, &data, &data_len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, data_len, id_out);
        free(data);
        return rc;
    }

    // Sort by path so the linear grouping pass in write_tree_level works correctly
    qsort(index->entries, (size_t)index->count, sizeof(IndexEntry), compare_index_by_path);

    int rc = write_tree_level(index->entries, index->count, 0, id_out);
    free(index);
    return rc;
}
