#include "dedup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    size_t duplicate_groups;
    size_t redundant_files;
    uint64_t wasted_bytes;
    size_t deleted_files;
    uint64_t freed_bytes;
    size_t io_errors;
} DedupStats;

static inline const char *clr(bool enabled, const char *code) {
    return enabled ? code : "";
}

static void print_usage(const char *prog) {
    printf("r8-dedup v%s — Fast Multi-Stage Duplicate File Finder (R8Force)\n\n",
           R8_DEDUP_VERSION);
    printf("Usage:\n");
    printf("  %s [options] [dir1 dir2 ...]\n\n", prog);
    printf("Options:\n");
    printf("  --min-size <bytes>  Minimum file size to inspect (default: 1)\n");
    printf("  --hidden            Include hidden files and directories\n");
    printf("  --fast              Skip final byte-by-byte verification\n");
    printf("  --delete            Remove duplicate copies (keeps first file)\n");
    printf("  --dry-run           Preview deletions without removing any file\n");
    printf("  --no-color          Disable ANSI terminal colors\n");
    printf("  -v, --version       Show version\n");
    printf("  -h, --help          Show this help message\n");
}

static bool parse_u64_arg(const char *arg, uint64_t *out) {
    if (!arg || *arg == '\0' || *arg == '-') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(arg, &end, 0);
    if (errno != 0 || end == arg || *end != '\0') return false;
    *out = (uint64_t)val;
    return true;
}

static void format_bytes(uint64_t bytes, char *out, size_t out_sz) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    size_t u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(out, out_sz, "%.2f %s", v, units[u]);
    }
}

static int cmp_by_size(const void *pa, const void *pb) {
    const FileEntry *a = (const FileEntry *)pa;
    const FileEntry *b = (const FileEntry *)pb;
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return strcmp(a->path, b->path);
}

static int cmp_by_edge(const void *pa, const void *pb) {
    const FileEntry *a = (const FileEntry *)pa;
    const FileEntry *b = (const FileEntry *)pb;
    if (a->edge_ready != b->edge_ready) return a->edge_ready ? -1 : 1;
    if (a->edge_a < b->edge_a) return -1;
    if (a->edge_a > b->edge_a) return 1;
    if (a->edge_b < b->edge_b) return -1;
    if (a->edge_b > b->edge_b) return 1;
    return strcmp(a->path, b->path);
}

static int cmp_by_full(const void *pa, const void *pb) {
    const FileEntry *a = (const FileEntry *)pa;
    const FileEntry *b = (const FileEntry *)pb;
    if (a->full_ready != b->full_ready) return a->full_ready ? -1 : 1;
    if (a->full_a < b->full_a) return -1;
    if (a->full_a > b->full_a) return 1;
    if (a->full_b < b->full_b) return -1;
    if (a->full_b > b->full_b) return 1;
    return strcmp(a->path, b->path);
}

static void handle_confirmed_group(FileEntry *group_items[], size_t count,
                                   const DedupConfig *cfg, DedupStats *stats) {
    const bool c = cfg->use_color;
    char sz_str[32];
    format_bytes(group_items[0]->size, sz_str, sizeof(sz_str));

    stats->duplicate_groups++;
    stats->redundant_files += (count - 1);
    stats->wasted_bytes += group_items[0]->size * (uint64_t)(count - 1);

    printf("\n%s[Group #%llu]%s  Size: %s%s%s  Copies: %llu  Digest: %s%016llx%s\n",
           clr(c, ANSI_CYAN), (unsigned long long)stats->duplicate_groups, clr(c, ANSI_RESET),
           clr(c, ANSI_GOLD), sz_str, clr(c, ANSI_RESET),
           (unsigned long long)count,
           clr(c, ANSI_DIM), (unsigned long long)group_items[0]->full_a, clr(c, ANSI_RESET));

    printf("  %s[KEEP]%s   %s\n",
           clr(c, ANSI_GREEN), clr(c, ANSI_RESET), group_items[0]->path);

    for (size_t i = 1; i < count; i++) {
        const char *dup_path = group_items[i]->path;
        if (cfg->delete_dups) {
            if (cfg->dry_run) {
                printf("  %s[DRYRUN]%s %s\n",
                       clr(c, ANSI_GOLD), clr(c, ANSI_RESET), dup_path);
            } else if (remove(dup_path) == 0) {
                stats->deleted_files++;
                stats->freed_bytes += group_items[i]->size;
                printf("  %s[DEL]%s    %s\n",
                       clr(c, ANSI_RED), clr(c, ANSI_RESET), dup_path);
            } else {
                stats->io_errors++;
                printf("  %s[ERR]%s    %s (could not delete)\n",
                       clr(c, ANSI_RED), clr(c, ANSI_RESET), dup_path);
            }
        } else {
            printf("  %s[DUP]%s    %s\n",
                   clr(c, ANSI_RED), clr(c, ANSI_RESET), dup_path);
        }
    }
}

static void process_hash_bucket(FileEntry *items, size_t count,
                                const DedupConfig *cfg, DedupStats *stats) {
    bool *visited = (bool *)calloc(count, sizeof(bool));
    FileEntry **group = (FileEntry **)malloc(count * sizeof(FileEntry *));
    if (!visited || !group) {
        free(visited);
        free(group);
        stats->io_errors++;
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (visited[i]) continue;
        visited[i] = true;

        size_t g_len = 0;
        group[g_len++] = &items[i];

        for (size_t j = i + 1; j < count; j++) {
            if (visited[j]) continue;
            bool match = true;
            if (cfg->verify_bytes) {
                match = files_identical_bytes(items[i].path, items[j].path);
            }
            if (match) {
                visited[j] = true;
                group[g_len++] = &items[j];
            }
        }

        if (g_len >= 2) {
            handle_confirmed_group(group, g_len, cfg, stats);
        }
    }

    free(group);
    free(visited);
}

static void process_size_bucket(FileEntry *items, size_t count,
                                const DedupConfig *cfg, DedupStats *stats) {
    for (size_t i = 0; i < count; i++) {
        if (!compute_edge_signature(&items[i])) {
            stats->io_errors++;
        }
    }

    qsort(items, count, sizeof(FileEntry), cmp_by_edge);

    size_t i = 0;
    while (i < count) {
        if (!items[i].edge_ready) break;

        size_t j = i + 1;
        while (j < count && items[j].edge_ready &&
               items[j].edge_a == items[i].edge_a &&
               items[j].edge_b == items[i].edge_b) {
            j++;
        }

        size_t edge_len = j - i;
        if (edge_len >= 2) {
            for (size_t k = i; k < j; k++) {
                if (!compute_full_signature(&items[k])) {
                    stats->io_errors++;
                }
            }

            qsort(items + i, edge_len, sizeof(FileEntry), cmp_by_full);

            size_t m = i;
            while (m < j) {
                if (!items[m].full_ready) break;

                size_t n = m + 1;
                while (n < j && items[n].full_ready &&
                       items[n].full_a == items[m].full_a &&
                       items[n].full_b == items[m].full_b) {
                    n++;
                }

                if (n - m >= 2) {
                    process_hash_bucket(items + m, n - m, cfg, stats);
                }
                m = n;
            }
        }
        i = j;
    }
}

int main(int argc, char **argv) {
    DedupConfig cfg = {
        .min_size = 1,
        .include_hidden = false,
        .verify_bytes = true,
        .delete_dups = false,
        .dry_run = false,
        .use_color = true
    };

    const char **dirs = (const char **)malloc((size_t)(argc + 1) * sizeof(const char *));
    if (!dirs) {
        fprintf(stderr, "Error: Out of memory.\n");
        return 1;
    }
    size_t dir_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            free(dirs);
            return 0;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("r8-dedup %s (R8Force)\n", R8_DEDUP_VERSION);
            free(dirs);
            return 0;
        } else if (strcmp(arg, "--min-size") == 0) {
            if (i + 1 >= argc || !parse_u64_arg(argv[++i], &cfg.min_size)) {
                fprintf(stderr, "Error: Invalid value for --min-size.\n");
                free(dirs);
                return 1;
            }
        } else if (strcmp(arg, "--hidden") == 0) {
            cfg.include_hidden = true;
        } else if (strcmp(arg, "--fast") == 0) {
            cfg.verify_bytes = false;
        } else if (strcmp(arg, "--delete") == 0) {
            cfg.delete_dups = true;
        } else if (strcmp(arg, "--dry-run") == 0) {
            cfg.delete_dups = true;
            cfg.dry_run = true;
        } else if (strcmp(arg, "--no-color") == 0) {
            cfg.use_color = false;
        } else if (arg[0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'. Use --help.\n", arg);
            free(dirs);
            return 1;
        } else {
            dirs[dir_count++] = arg;
        }
    }

    if (dir_count == 0) {
        dirs[dir_count++] = ".";
    }

    const bool c = cfg.use_color;
    printf("%s==================================================%s\n",
           clr(c, ANSI_DIM), clr(c, ANSI_RESET));
    printf(" %sR8Force Duplicate Scanner%s v%s\n",
           clr(c, ANSI_BOLD), clr(c, ANSI_RESET), R8_DEDUP_VERSION);
    printf("%s==================================================%s\n",
           clr(c, ANSI_DIM), clr(c, ANSI_RESET));

    clock_t t_start = clock();

    FileList list;
    list_init(&list);

    for (size_t d = 0; d < dir_count; d++) {
        scan_directory(dirs[d], &cfg, &list);
    }

    DedupStats stats = {0};

    if (list.count >= 2) {
        qsort(list.items, list.count, sizeof(FileEntry), cmp_by_size);

        size_t i = 0;
        while (i < list.count) {
            size_t j = i + 1;
            while (j < list.count && list.items[j].size == list.items[i].size) {
                j++;
            }
            if (j - i >= 2) {
                process_size_bucket(list.items + i, j - i, &cfg, &stats);
            }
            i = j;
        }
    }

    double elapsed = (double)(clock() - t_start) / (double)CLOCKS_PER_SEC;
    char wasted_str[32];
    char freed_str[32];
    format_bytes(stats.wasted_bytes, wasted_str, sizeof(wasted_str));
    format_bytes(stats.freed_bytes, freed_str, sizeof(freed_str));

    printf("\n%s--------------------------------------------------%s\n",
           clr(c, ANSI_DIM), clr(c, ANSI_RESET));
    printf("  Files Scanned   : %llu\n", (unsigned long long)list.count);
    printf("  Duplicate Groups: %llu\n", (unsigned long long)stats.duplicate_groups);
    printf("  Redundant Files : %llu\n", (unsigned long long)stats.redundant_files);
    printf("  Reclaimable     : %s%s%s\n",
           clr(c, stats.wasted_bytes > 0 ? ANSI_GOLD : ANSI_GREEN),
           wasted_str, clr(c, ANSI_RESET));
    if (cfg.delete_dups && !cfg.dry_run) {
        printf("  Deleted / Freed : %llu file(s) (%s)\n",
               (unsigned long long)stats.deleted_files, freed_str);
    }
    if (list.skipped_errors > 0 || stats.io_errors > 0) {
        printf("  Skipped / I/O   : %llu\n",
               (unsigned long long)(list.skipped_errors + stats.io_errors));
    }
    printf("  Elapsed Time    : %.3f sec\n", elapsed);

    list_free(&list);
    free(dirs);
    return 0;
}
