#define _POSIX_C_SOURCE 200809L

#include "dedup.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

char *r8_str_copy(const char *src) {
    size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, src, len + 1);
    return copy;
}

void list_init(FileList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->skipped_errors = 0;
}

bool list_push(FileList *list, const char *path, uint64_t size) {
    if (list->count == list->capacity) {
        size_t next_cap = (list->capacity == 0) ? 256 : list->capacity * 2;
        FileEntry *grown = (FileEntry *)realloc(list->items, next_cap * sizeof(FileEntry));
        if (!grown) return false;
        list->items = grown;
        list->capacity = next_cap;
    }

    char *owned = r8_str_copy(path);
    if (!owned) return false;

    FileEntry *e = &list->items[list->count++];
    e->path = owned;
    e->size = size;
    e->edge_a = 0;
    e->edge_b = 0;
    e->full_a = 0;
    e->full_b = 0;
    e->edge_ready = false;
    e->full_ready = false;
    return true;
}

void list_free(FileList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].path);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static char *join_path(const char *base, const char *name) {
    size_t blen = strlen(base);
    size_t nlen = strlen(name);
    bool has_sep = (blen > 0 && (base[blen - 1] == '/' || base[blen - 1] == '\\'));

    char *out = (char *)malloc(blen + nlen + (has_sep ? 1 : 2));
    if (!out) return NULL;

    memcpy(out, base, blen);
    size_t pos = blen;
    if (!has_sep) {
#ifdef _WIN32
        out[pos++] = '\\';
#else
        out[pos++] = '/';
#endif
    }
    memcpy(out + pos, name, nlen + 1);
    return out;
}

#ifdef _WIN32

void scan_directory(const char *root, const DedupConfig *cfg, FileList *out) {
    char *pattern = join_path(root, "*");
    if (!pattern) {
        out->skipped_errors++;
        return;
    }

    WIN32_FIND_DATAA fd;
    HANDLE handle = FindFirstFileA(pattern, &fd);
    free(pattern);

    if (handle == INVALID_HANDLE_VALUE) {
        out->skipped_errors++;
        return;
    }

    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        if (!cfg->include_hidden) {
            if (name[0] == '.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
                continue;
            }
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }

        char *full = join_path(root, name);
        if (!full) {
            out->skipped_errors++;
            continue;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            scan_directory(full, cfg, out);
        } else {
            uint64_t fsize = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
            if (fsize >= cfg->min_size) {
                if (!list_push(out, full, fsize)) {
                    out->skipped_errors++;
                }
            }
        }

        free(full);
    } while (FindNextFileA(handle, &fd) != 0);

    FindClose(handle);
}

#else

void scan_directory(const char *root, const DedupConfig *cfg, FileList *out) {
    DIR *dir = opendir(root);
    if (!dir) {
        out->skipped_errors++;
        return;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (!cfg->include_hidden && name[0] == '.') {
            continue;
        }

        char *full = join_path(root, name);
        if (!full) {
            out->skipped_errors++;
            continue;
        }

        struct stat st;
        if (lstat(full, &st) != 0) {
            out->skipped_errors++;
            free(full);
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            free(full);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_directory(full, cfg, out);
        } else if (S_ISREG(st.st_mode) && st.st_size >= 0) {
            uint64_t fsize = (uint64_t)st.st_size;
            if (fsize >= cfg->min_size) {
                if (!list_push(out, full, fsize)) {
                    out->skipped_errors++;
                }
            }
        }

        free(full);
    }

    closedir(dir);
}

#endif
