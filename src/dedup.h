
#ifndef R8_DEDUP_H
#define R8_DEDUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define R8_DEDUP_VERSION "1.0.0"

#define ANSI_RESET "\x1b[0m"
#define ANSI_BOLD  "\x1b[1m"
#define ANSI_CYAN  "\x1b[36m"
#define ANSI_GOLD  "\x1b[33m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_RED   "\x1b[31m"
#define ANSI_DIM   "\x1b[90m"

typedef struct {
    char *path;
    uint64_t size;
    uint64_t edge_a;
    uint64_t edge_b;
    uint64_t full_a;
    uint64_t full_b;
    bool edge_ready;
    bool full_ready;
} FileEntry;

typedef struct {
    FileEntry *items;
    size_t count;
    size_t capacity;
    size_t skipped_errors;
} FileList;

typedef struct {
    uint64_t min_size;
    bool include_hidden;
    bool verify_bytes;
    bool delete_dups;
    bool dry_run;
    bool use_color;
} DedupConfig;

char *r8_str_copy(const char *src);
void list_init(FileList *list);
bool list_push(FileList *list, const char *path, uint64_t size);
void list_free(FileList *list);

void scan_directory(const char *root, const DedupConfig *cfg, FileList *out);

bool compute_edge_signature(FileEntry *entry);
bool compute_full_signature(FileEntry *entry);
bool files_identical_bytes(const char *path_a, const char *path_b);

#endif /* R8_DEDUP_H */
