// commit.c — Commit creation and history traversal
//
// Commit object format (stored as text, one field per line):
//
//   tree <64-char-hex-hash>
//   parent <64-char-hex-hash>        ← omitted for the first commit
//   author <name> <unix-timestamp>
//   committer <name> <unix-timestamp>
//
//   <commit message>
//
// Note: there is a blank line between the headers and the message.
//
// PROVIDED functions: commit_parse, commit_serialize, commit_walk, head_read, head_update
// TODO functions:     commit_create

#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Parse raw commit data into a Commit struct.
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;
    const char *p = (const char *)data;
    char hex[HASH_HEX_SIZE + 1];

    // "tree <hex>\n"
    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n') + 1;

    // optional "parent <hex>\n"
    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        commit_out->has_parent = 0;
    }

    // "author <name> <timestamp>\n"
    char author_buf[256];
    uint64_t ts;
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;
    // split off trailing timestamp
    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;
    ts = (uint64_t)strtoull(last_space + 1, NULL, 10);
    *last_space = '\0';
    snprintf(commit_out->author, sizeof(commit_out->author), "%s", author_buf);
    commit_out->timestamp = ts;
    p = strchr(p, '\n') + 1;  // skip author line
    p = strchr(p, '\n') + 1;  // skip committer line
    p = strchr(p, '\n') + 1;  // skip blank line

    snprintf(commit_out->message, sizeof(commit_out->message), "%s", p);
    return 0;
}


int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    char parent_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);
 
    char buf[8192];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "tree %s\n", tree_hex);
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "parent %s\n", parent_hex);
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "author %s %" PRIu64 "\n"
                  "committer %s %" PRIu64 "\n"
                  "\n"
                  "%s",
                  commit->author, commit->timestamp,
                  commit->author, commit->timestamp,
                  commit->message);
 
    *data_out = malloc((size_t)n + 1);
    if (!*data_out) return -1;
    memcpy(*data_out, buf, (size_t)n + 1);
    *len_out = (size_t)n;
    return 0;
}
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1; // No commits yet
 
    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;
 
        Commit c;
        int rc = commit_parse(raw, raw_len, &c);
        free(raw);
        if (rc != 0) return -1;
 
        // Fire the callback (e.g. print the commit in pes log)
        callback(&id, &c, ctx);
 
        // Follow the parent pointer — stop at root commit
        if (!c.has_parent) break;
        id = c.parent;
    }
    return 0;
}

int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
 
    // HEAD contains "ref: refs/heads/main" → follow to the branch file
    if (strncmp(line, "ref: ", 5) == 0) {
        char ref_path[512];
        snprintf(ref_path, sizeof(ref_path), "%s/%s", PES_DIR, line + 5);
        f = fopen(ref_path, "r");
        if (!f) return -1; // Branch has no commits yet
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
        fclose(f);
        line[strcspn(line, "\r\n")] = '\0';
    }
    // line now holds the commit hash hex string
    return hex_to_hash(line, id_out);
}
 
int head_update(const ObjectID *new_commit) {
    // Read HEAD to find which file to update
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';
 
    char target_path[520];
    if (strncmp(line, "ref: ", 5) == 0)
        snprintf(target_path, sizeof(target_path), "%s/%s", PES_DIR, line + 5);
    else
        snprintf(target_path, sizeof(target_path), "%s", HEAD_FILE); // detached HEAD
 
    // Write new hash to temp file, then atomically rename
    char tmp_path[528];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target_path);
    f = fopen(tmp_path, "w");
    if (!f) return -1;
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);
    fprintf(f, "%s\n", hex);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
 
    return rename(tmp_path, target_path);
}
// ─── TODO ────────────────────────────────────────────────────────────────────
 

int commit_create(const char *message, ObjectID *commit_id_out) {
    // Step 1: Build a tree object from the current index
    ObjectID tree_id;
    if (tree_from_index(&tree_id) != 0) {
        fprintf(stderr, "error: nothing to commit (index is empty)\n");
        return -1;
    }
 
    // Step 2: Read current HEAD as parent (fails gracefully for first commit)
    ObjectID parent_id;
    int has_parent = (head_read(&parent_id) == 0);
 
    // Step 3: Fill the Commit struct
    Commit c;
    memset(&c, 0, sizeof(c));
    c.tree       = tree_id;
    c.has_parent = has_parent;
    if (has_parent) c.parent = parent_id;
    c.timestamp  = (uint64_t)time(NULL);
    strncpy(c.author,  pes_author(), sizeof(c.author)  - 1);
    strncpy(c.message, message,      sizeof(c.message) - 1);
 
    // Step 4: Serialize commit struct to text buffer
    void  *raw;
    size_t raw_len;
    if (commit_serialize(&c, &raw, &raw_len) != 0) return -1;
 
    // Step 5: Write commit object to the object store
    int ret = object_write(OBJ_COMMIT, raw, raw_len, commit_id_out);
    free(raw);
    if (ret != 0) return -1;
 
    // Step 6: Update HEAD (or the branch it points to) to new commit
    return head_update(commit_id_out);
}