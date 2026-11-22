#define _POSIX_C_SOURCE 200809L

#include "dedup.h"

#include <stdio.h>
#include <string.h>

#define EDGE_SAMPLE_BYTES 4096U
#define STREAM_CHUNK_SIZE 32768U

typedef struct {
    uint64_t a;
    uint64_t b;
} MixState;

static inline uint64_t rotl64(uint64_t x, unsigned int k) {
    return (x << k) | (x >> (64U - k));
}

static inline uint64_t load_le64(const uint8_t *p) {
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void mix_init(MixState *st, uint64_t salt) {
    st->a = 0x5238464f52434501ULL ^ salt;
    st->b = 0x9e3779b97f4a7c15ULL ^ rotl64(salt, 29);
}

static void mix_bytes(MixState *st, const uint8_t *buf, size_t len) {
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t w = load_le64(buf + i);
        st->a ^= w;
        st->a = rotl64(st->a, 27) * 0xbf58476d1ce4e5b9ULL;
        st->b += w ^ st->a;
        st->b = rotl64(st->b, 31) * 0x94d049bb133111ebULL;
        i += 8;
    }

    uint64_t tail = 0;
    unsigned int shift = 0;
    while (i < len) {
        tail |= ((uint64_t)buf[i]) << shift;
        shift += 8;
        i++;
    }
    if (shift > 0) {
        st->a ^= tail;
        st->a *= 0xbf58476d1ce4e5b9ULL;
        st->b ^= rotl64(tail, 17);
        st->b *= 0x94d049bb133111ebULL;
    }
}

static void mix_finish(MixState *st, uint64_t total_len) {
    st->a ^= total_len;
    st->b ^= rotl64(total_len, 33);

    st->a ^= st->a >> 30;
    st->a *= 0xbf58476d1ce4e5b9ULL;
    st->a ^= st->a >> 27;
    st->a *= 0x94d049bb133111ebULL;
    st->a ^= st->a >> 31;

    st->b ^= st->b >> 30;
    st->b *= 0x94d049bb133111ebULL;
    st->b ^= st->b >> 27;
    st->b *= 0xbf58476d1ce4e5b9ULL;
    st->b ^= st->b >> 31;
}

static bool seek_pos(FILE *fp, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(fp, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(fp, (off_t)offset, SEEK_SET) == 0;
#endif
}

bool compute_edge_signature(FileEntry *entry) {
    if (entry->edge_ready) return true;

    FILE *fp = fopen(entry->path, "rb");
    if (!fp) return false;

    uint8_t buf[EDGE_SAMPLE_BYTES];
    MixState st;
    mix_init(&st, entry->size);

    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (ferror(fp)) {
        fclose(fp);
        return false;
    }
    mix_bytes(&st, buf, n);

    if (entry->size > EDGE_SAMPLE_BYTES) {
        uint64_t tail_off = entry->size - EDGE_SAMPLE_BYTES;
        if (!seek_pos(fp, tail_off)) {
            fclose(fp);
            return false;
        }
        size_t nt = fread(buf, 1, sizeof(buf), fp);
        if (ferror(fp)) {
            fclose(fp);
            return false;
        }
        mix_bytes(&st, buf, nt);
    }

    fclose(fp);
    mix_finish(&st, entry->size);
    entry->edge_a = st.a;
    entry->edge_b = st.b;
    entry->edge_ready = true;
    return true;
}

bool compute_full_signature(FileEntry *entry) {
    if (entry->full_ready) return true;

    FILE *fp = fopen(entry->path, "rb");
    if (!fp) return false;

    uint8_t buf[STREAM_CHUNK_SIZE];
    MixState st;
    mix_init(&st, entry->size ^ 0x55aa55aa55aa55aaULL);

    uint64_t read_total = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0) {
            mix_bytes(&st, buf, n);
            read_total += (uint64_t)n;
        }
        if (n < sizeof(buf)) {
            if (ferror(fp)) {
                fclose(fp);
                return false;
            }
            break;
        }
    }

    fclose(fp);
    mix_finish(&st, read_total);
    entry->full_a = st.a;
    entry->full_b = st.b;
    entry->full_ready = true;
    return true;
}

bool files_identical_bytes(const char *path_a, const char *path_b) {
    FILE *fa = fopen(path_a, "rb");
    if (!fa) return false;

    FILE *fb = fopen(path_b, "rb");
    if (!fb) {
        fclose(fa);
        return false;
    }

    uint8_t ba[STREAM_CHUNK_SIZE];
    uint8_t bb[STREAM_CHUNK_SIZE];
    bool same = true;

    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);

        if (ferror(fa) || ferror(fb)) {
            same = false;
            break;
        }
        if (na != nb) {
            same = false;
            break;
        }
        if (na == 0) {
            break;
        }
        if (memcmp(ba, bb, na) != 0) {
            same = false;
            break;
        }
    }

    fclose(fa);
    fclose(fb);
    return same;
}
