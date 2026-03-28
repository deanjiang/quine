/*
 * quine.c — CDC delta compression with unified flat address space
 *
 * See quine.h for the full algorithm description.
 *
 * Platform: Linux/POSIX.
 * Build:    cc -O2 -D_GNU_SOURCE -o quine quine.c main.c -lssl -lcrypto
 */

#include "quine/quine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>
#include <openssl/sha.h>

/* ── Error state (thread-local) ──────────────────────────────────────────── */

static __thread char _errbuf[256];
#define SET_ERR(...) snprintf(_errbuf, sizeof(_errbuf), __VA_ARGS__)
const char *quine_errmsg(void) { return _errbuf; }

/* ── Progress callback (thread-local) ────────────────────────────────────── */

static __thread quine_progress_fn _progress_fn  = NULL;
static __thread void             *_progress_ctx = NULL;

void quine_set_progress(quine_progress_fn fn, void *ctx) {
    _progress_fn  = fn;
    _progress_ctx = ctx;
}

static void progress(const char *stage, const char *file,
                     uint32_t current, uint32_t total) {
    if (_progress_fn) {
        quine_progress_t info = { stage, file, current, total };
        _progress_fn(&info, _progress_ctx);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Rabin rolling hash CDC
 *
 * The 48-byte window slides one byte at a time.  At each position we compute
 * a 64-bit hash over the window contents.  When the low 14 bits are zero we
 * declare a chunk boundary (average spacing ~16 KB).  Hard min/max clamps
 * prevent degenerate tiny or huge chunks on pathological inputs.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RABIN_POLY   0xbfe6b8a5bf378d83ULL
#define CHUNK_MASK   ((1u << 14) - 1)          /* avg 16 KB */

static uint64_t g_rabin_table[256];
static int      g_rabin_init = 0;

static void rabin_init_tables(void) {
    if (g_rabin_init) return;
    for (int i = 0; i < 256; i++) {
        uint64_t h = (uint64_t)i;
        for (int j = 0; j < 8; j++)
            h = (h >> 1) ^ (RABIN_POLY & -(h & 1));
        g_rabin_table[i] = h;
    }
    g_rabin_init = 1;
}

typedef struct {
    uint8_t  win[QN_RABIN_WINDOW];
    int      wpos;
    uint64_t hash;
    uint32_t len;       /* bytes since last boundary */
} Rabin;

static void rabin_reset(Rabin *r) {
    rabin_init_tables();
    memset(r, 0, sizeof(*r));
}

/* Returns 1 if byte b completes a chunk boundary. */
static inline int rabin_roll(Rabin *r, uint8_t b) {
    r->hash ^= g_rabin_table[r->win[r->wpos]];
    r->win[r->wpos] = b;
    r->wpos = (r->wpos + 1) % QN_RABIN_WINDOW;
    r->hash = (r->hash << 1) ^ (uint64_t)b;
    r->len++;
    if (r->len < QN_CHUNK_MIN) return 0;
    if (r->len >= QN_CHUNK_MAX) { r->len = 0; return 1; }
    if ((r->hash & CHUNK_MASK) == 0) { r->len = 0; return 1; }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  File list — sorted collection of (path, size) pairs
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char    *rel;       /* relative path (heap-allocated) */
    uint64_t size;      /* original file size in bytes    */
} FileEntry;

typedef struct {
    FileEntry *e;
    uint32_t   count;
    uint32_t   cap;
} FileList;

static void fl_free(FileList *fl) {
    for (uint32_t i = 0; i < fl->count; i++) free(fl->e[i].rel);
    free(fl->e);
    fl->e = NULL; fl->count = fl->cap = 0;
}

static int fl_add(FileList *fl, const char *rel, uint64_t size) {
    if (fl->count == fl->cap) {
        uint32_t nc = fl->cap ? fl->cap * 2 : 64;
        FileEntry *ne = realloc(fl->e, nc * sizeof(FileEntry));
        if (!ne) return -1;
        fl->e = ne; fl->cap = nc;
    }
    fl->e[fl->count].rel  = strdup(rel);
    fl->e[fl->count].size = size;
    fl->count++;
    return 0;
}

static int fe_cmp(const void *a, const void *b) {
    return strcmp(((const FileEntry*)a)->rel, ((const FileEntry*)b)->rel);
}

/* Collect all regular files under root into fl, then sort lexicographically. */
static int collect_files(const char *root, FileList *fl) {
    /* iterative walk with explicit stack */
    char **stack = malloc(4096 * sizeof(char*));
    if (!stack) return -1;
    int top = 0;
    stack[top++] = strdup(root);

    while (top > 0) {
        char *cur = stack[--top];
        DIR *d = opendir(cur);
        if (!d) { free(cur); continue; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
            size_t n = strlen(cur) + 1 + strlen(de->d_name) + 1;
            char *full = malloc(n);
            snprintf(full, n, "%s/%s", cur, de->d_name);
            struct stat st;
            if (lstat(full, &st)) { free(full); continue; }
            if (S_ISDIR(st.st_mode)) {
                if (top < 4095) stack[top++] = full; else free(full);
            } else if (S_ISREG(st.st_mode)) {
                size_t rlen = strlen(root);
                const char *rel = full + rlen + (full[rlen] == '/');
                fl_add(fl, rel, (uint64_t)st.st_size);
                free(full);
            } else {
                free(full);
            }
        }
        closedir(d);
        free(cur);
    }
    free(stack);
    qsort(fl->e, fl->count, sizeof(FileEntry), fe_cmp);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Chunk index — sha256 → global_offset (compression side only)
 *
 * Open-addressed hash table.  Key = SHA-256[0..31].
 * Value = global offset in the unified flat address space.
 *
 * Note: len == 0 is used as the "empty bucket" sentinel.  Zero-length chunks
 * are never inserted (CDC min is 4 KB).
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IDX_INIT_CAP  (1u << 20)    /* 1M buckets (~64 MB) */
#define IDX_LOAD_NUM  3
#define IDX_LOAD_DEN  4             /* rehash at 75% load  */

typedef struct {
    uint8_t  sha[32];
    uint64_t global_offset;
    uint32_t len;           /* 0 = empty */
} IdxEntry;

typedef struct {
    IdxEntry *b;
    size_t    cap;
    size_t    count;
} ChunkIdx;

static int idx_init(ChunkIdx *x) {
    x->cap   = IDX_INIT_CAP;
    x->count = 0;
    x->b     = calloc(x->cap, sizeof(IdxEntry));
    return x->b ? 0 : -1;
}

static void idx_free(ChunkIdx *x) { free(x->b); x->b = NULL; }

static size_t idx_slot(const uint8_t *sha, size_t cap) {
    uint64_t h; memcpy(&h, sha, 8);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (size_t)(h & (cap - 1));
}

static int idx_rehash(ChunkIdx *x) {
    size_t nc = x->cap * 2;
    IdxEntry *nb = calloc(nc, sizeof(IdxEntry));
    if (!nb) return -1;
    for (size_t i = 0; i < x->cap; i++) {
        if (!x->b[i].len) continue;
        size_t j = idx_slot(x->b[i].sha, nc);
        while (nb[j].len) j = (j + 1) & (nc - 1);
        nb[j] = x->b[i];
    }
    free(x->b); x->b = nb; x->cap = nc;
    return 0;
}

/*
 * Insert sha → (global_offset, len).
 * Skips duplicates (same sha already present).
 * Returns 0 ok, -1 OOM.
 */
static int idx_insert(ChunkIdx *x, const uint8_t *sha,
                       uint64_t global_offset, uint32_t len) {
    if (x->count * IDX_LOAD_DEN >= x->cap * IDX_LOAD_NUM)
        if (idx_rehash(x)) return -1;
    size_t i = idx_slot(sha, x->cap);
    while (x->b[i].len) {
        if (!memcmp(x->b[i].sha, sha, 32)) return 0;  /* dup — keep first */
        i = (i + 1) & (x->cap - 1);
    }
    memcpy(x->b[i].sha, sha, 32);
    x->b[i].global_offset = global_offset;
    x->b[i].len           = len;
    x->count++;
    return 0;
}

/* Returns pointer to entry or NULL. */
static const IdxEntry *idx_lookup(const ChunkIdx *x, const uint8_t *sha) {
    size_t i = idx_slot(sha, x->cap);
    while (x->b[i].len) {
        if (!memcmp(x->b[i].sha, sha, 32)) return &x->b[i];
        i = (i + 1) & (x->cap - 1);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Patch writer (buffered)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PW_BUF (256u * 1024u)

typedef struct { FILE *f; uint8_t buf[PW_BUF]; size_t pos; } PW;

static int pw_open(PW *w, const char *p) {
    w->f = fopen(p, "wb"); w->pos = 0; return w->f ? 0 : -1;
}
static int pw_flush(PW *w) {
    if (!w->pos) return 0;
    int ok = fwrite(w->buf, 1, w->pos, w->f) == w->pos;
    w->pos = 0; return ok ? 0 : -1;
}
static int pw_write(PW *w, const void *d, size_t n) {
    const uint8_t *p = d;
    while (n) {
        size_t a = PW_BUF - w->pos, t = n < a ? n : a;
        memcpy(w->buf + w->pos, p, t);
        w->pos += t; p += t; n -= t;
        if (w->pos == PW_BUF && pw_flush(w)) return -1;
    }
    return 0;
}
static int pw_u8 (PW *w, uint8_t  v) { return pw_write(w,&v,1); }
static int pw_u16(PW *w, uint16_t v) {
    uint8_t b[2]={v,v>>8}; return pw_write(w,b,2);
}
static int pw_u32(PW *w, uint32_t v) {
    uint8_t b[4]={v,v>>8,v>>16,v>>24}; return pw_write(w,b,4);
}
static int pw_u64(PW *w, uint64_t v) {
    uint8_t b[8];
    for(int i=0;i<8;i++) b[i]=(uint8_t)(v>>(8*i));
    return pw_write(w,b,8);
}
static int pw_close(PW *w) { pw_flush(w); return fclose(w->f); }

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Index one file into the chunk index
 *
 * mmap the file, slide the Rabin window, SHA-256 each chunk, insert into idx
 * at global_base + chunk_start.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int index_file(ChunkIdx *idx, const char *abs_path,
                       uint64_t global_base) {
    int fd = open(abs_path, O_RDONLY);
    if (fd < 0) return 0;               /* skip unreadable */
    struct stat st; fstat(fd, &st);
    if (st.st_size == 0) { close(fd); return 0; }

    const uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return 0;
    madvise((void*)data, st.st_size, MADV_SEQUENTIAL);

    Rabin r; rabin_reset(&r);
    size_t chunk_start = 0;

    for (size_t i = 0; i < (size_t)st.st_size; i++) {
        if (rabin_roll(&r, data[i]) || i == (size_t)st.st_size - 1) {
            uint32_t clen = (uint32_t)(i - chunk_start + 1);
            uint8_t  sha[32];
            SHA256(data + chunk_start, clen, sha);
            idx_insert(idx, sha, global_base + chunk_start, clen);
            chunk_start = i + 1;
            rabin_reset(&r);
        }
    }
    munmap((void*)data, st.st_size);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Encode one B file
 *
 * mmap the file, CDC-chunk, lookup each chunk in the index.
 *   HIT  → flush pending literals, emit REF(global_offset, len)
 *   MISS → accumulate into lit buffer
 * After processing, index this file's chunks at their global offsets so
 * later B files can reference them.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Growing literal accumulation buffer */
typedef struct { uint8_t *buf; size_t len, cap; } LitBuf;

static int lb_append(LitBuf *lb, const uint8_t *data, size_t n) {
    if (lb->len + n > lb->cap) {
        size_t nc = (lb->len + n) * 2 + QN_CHUNK_MAX;
        uint8_t *nb = realloc(lb->buf, nc);
        if (!nb) return -1;
        lb->buf = nb; lb->cap = nc;
    }
    memcpy(lb->buf + lb->len, data, n);
    lb->len += n;
    return 0;
}

static int lb_flush(LitBuf *lb, PW *pw) {
    if (!lb->len) return 0;
    /* split at 32 MB so u32 len field is never a problem */
    size_t off = 0;
    while (off < lb->len) {
        uint32_t take = (uint32_t)(lb->len - off);
        if (take > 32u*1024*1024) take = 32u*1024*1024;
        pw_u8(pw, QN_OP_LIT);
        pw_u32(pw, take);
        pw_write(pw, lb->buf + off, take);
        off += take;
    }
    lb->len = 0;
    return 0;
}

static int encode_file(ChunkIdx *idx, PW *pw, LitBuf *lb,
                        const char *abs_path, uint64_t global_base) {
    int fd = open(abs_path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st; fstat(fd, &st);
    if (st.st_size == 0) { close(fd); return 0; }

    const uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return 0;
    madvise((void*)data, st.st_size, MADV_SEQUENTIAL);

    Rabin r; rabin_reset(&r);
    size_t chunk_start = 0;

    for (size_t i = 0; i < (size_t)st.st_size; i++) {
        if (rabin_roll(&r, data[i]) || i == (size_t)st.st_size - 1) {
            uint32_t       clen = (uint32_t)(i - chunk_start + 1);
            uint8_t        sha[32];
            SHA256(data + chunk_start, clen, sha);

            const IdxEntry *e = idx_lookup(idx, sha);
            if (e) {
                lb_flush(lb, pw);
                pw_u8 (pw, QN_OP_REF);
                pw_u64(pw, e->global_offset);
                pw_u32(pw, e->len);
            } else {
                lb_append(lb, data + chunk_start, clen);
            }

            /*
             * Index this chunk immediately after encoding it.
             * global_base + chunk_start is strictly less than the global
             * offset of any subsequent chunk, so the strictly-before
             * invariant holds for both later chunks in this same file
             * and for all subsequent B files.
             * idx_insert is a no-op if the sha is already present (dedup).
             */
            idx_insert(idx, sha, global_base + chunk_start, clen);

            chunk_start = i + 1;
            rabin_reset(&r);
        }
    }
    lb_flush(lb, pw);

    munmap((void*)data, st.st_size);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  quine_compress
 * ═══════════════════════════════════════════════════════════════════════════ */

int quine_compress(const char *dir_a, const char *dir_b,
                      const char *patch_path) {
    int ret = -1;

    /* ── 1. Collect file lists ── */
    FileList fla = {0}, flb = {0};
    progress("scan_a", NULL, 0, 0);
    if (collect_files(dir_a, &fla)) { SET_ERR("walk A failed"); goto done; }
    progress("scan_b", NULL, 0, 0);
    if (collect_files(dir_b, &flb)) { SET_ERR("walk B failed"); goto done; }

    /* ── 2. Build flat-A offset table and index all A chunks ── */
    ChunkIdx idx; idx_init(&idx);

    uint64_t flat_pos = 0;
    for (uint32_t i = 0; i < fla.count; i++) {
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s/%s", dir_a, fla.e[i].rel);
        progress("index_a", fla.e[i].rel, i + 1, fla.count);
        index_file(&idx, abs, flat_pos);
        flat_pos += fla.e[i].size;
    }
    uint64_t a_total = flat_pos;   /* B offsets start here */

    /* ── 3. Open patch file and write header ── */
    progress("write_header", NULL, 0, 0);
    PW pw;
    if (pw_open(&pw, patch_path)) {
        SET_ERR("open patch: %s", strerror(errno)); goto done;
    }

    pw_write(&pw, QN_MAGIC, 4);
    pw_u8(&pw, QN_VERSION);
    pw_u32(&pw, QN_CHUNK_MIN);
    pw_u32(&pw, QN_CHUNK_AVG);
    pw_u32(&pw, QN_CHUNK_MAX);

    /* A manifest */
    if (fla.count > 0xFFFF) { SET_ERR("too many A files"); goto done; }
    pw_u16(&pw, (uint16_t)fla.count);
    for (uint32_t i = 0; i < fla.count; i++) {
        size_t plen = strlen(fla.e[i].rel); if (plen > 0xFFFF) plen = 0xFFFF;
        pw_u16(&pw, (uint16_t)plen);
        pw_write(&pw, fla.e[i].rel, plen);
        pw_u64(&pw, fla.e[i].size);
    }

    /* B manifest — original file sizes, known from stat() */
    if (flb.count > 0xFFFF) { SET_ERR("too many B files"); goto done; }
    pw_u16(&pw, (uint16_t)flb.count);
    for (uint32_t i = 0; i < flb.count; i++) {
        size_t plen = strlen(flb.e[i].rel); if (plen > 0xFFFF) plen = 0xFFFF;
        pw_u16(&pw, (uint16_t)plen);
        pw_write(&pw, flb.e[i].rel, plen);
        pw_u64(&pw, flb.e[i].size);
    }

    /* ── 4. Encode each B file; index it afterward ── */
    LitBuf lb = {0};
    flat_pos = a_total;

    for (uint32_t i = 0; i < flb.count; i++) {
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s/%s", dir_b, flb.e[i].rel);

        size_t plen = strlen(flb.e[i].rel); if (plen > 0xFFFF) plen = 0xFFFF;
        pw_u8(&pw, QN_OP_NEWFILE);
        pw_u16(&pw, (uint16_t)plen);
        pw_write(&pw, flb.e[i].rel, plen);

        progress("encode_b", flb.e[i].rel, i + 1, flb.count);
        encode_file(&idx, &pw, &lb, abs, flat_pos);
        flat_pos += flb.e[i].size;
    }
    free(lb.buf);

    pw_u8(&pw, QN_OP_END);
    pw_close(&pw);
    idx_free(&idx);
    ret = 0;

done:
    fl_free(&fla);
    fl_free(&flb);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Slot table — maps global_offset → (fd, local_offset)
 *
 * Used by the decompressor to resolve a REF without knowing which file
 * it falls in.  Built from the A+B manifests in the patch header.
 * Slots are sorted by global_start; binary search resolves any offset.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t global_start;  /* first byte of this file in the flat space */
    uint64_t size;
    int      fd;            /* -1 = not yet available (future B file)     */
} Slot;

typedef struct {
    Slot    *s;
    uint32_t count;
    uint32_t cap;
} SlotTable;

static int st_add(SlotTable *t, uint64_t start, uint64_t size, int fd) {
    if (t->count == t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 64;
        Slot *ns = realloc(t->s, nc * sizeof(Slot));
        if (!ns) return -1;
        t->s = ns; t->cap = nc;
    }
    t->s[t->count++] = (Slot){ start, size, fd };
    return 0;
}

/*
 * Resolve global_offset to (fd, local_offset).
 * Returns -1 if the slot is not yet available or out of range.
 */
static int st_resolve(const SlotTable *t, uint64_t off,
                       int *fd_out, uint64_t *local_out) {
    /* binary search — slots are added in order so always sorted */
    int lo = 0, hi = (int)t->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < t->s[mid].global_start)          hi = mid - 1;
        else if (off >= t->s[mid].global_start + t->s[mid].size)
                                                   lo = mid + 1;
        else {
            if (t->s[mid].fd < 0) return -1;       /* not yet written */
            *fd_out    = t->s[mid].fd;
            *local_out = off - t->s[mid].global_start;
            return 0;
        }
    }
    return -1;
}

static void st_free(SlotTable *t) {
    /* close all fds we own */
    for (uint32_t i = 0; i < t->count; i++)
        if (t->s[i].fd >= 0) close(t->s[i].fd);
    free(t->s); t->s = NULL; t->count = t->cap = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Patch reader helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { const uint8_t *base; size_t size, pos; } PR;

static int pr_need(PR *r, size_t n) {
    if (r->pos + n > r->size) {
        SET_ERR("patch truncated at %zu", r->pos); return -1;
    }
    return 0;
}
static int pr_read(PR *r, void *d, size_t n) {
    if (pr_need(r,n)) return -1;
    memcpy(d, r->base + r->pos, n); r->pos += n; return 0;
}
static int pr_u8 (PR *r, uint8_t  *v) { return pr_read(r,v,1); }
static int pr_u16(PR *r, uint16_t *v) {
    uint8_t b[2]; if (pr_read(r,b,2)) return -1;
    *v=(uint16_t)(b[0]|(b[1]<<8)); return 0;
}
static int pr_u32(PR *r, uint32_t *v) {
    uint8_t b[4]; if (pr_read(r,b,4)) return -1;
    *v=b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24); return 0;
}
static int pr_u64(PR *r, uint64_t *v) {
    uint8_t b[8]; if (pr_read(r,b,8)) return -1;
    *v=0; for(int i=7;i>=0;i--) *v=(*v<<8)|b[i]; return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10  mkdir -p
 * ═══════════════════════════════════════════════════════════════════════════ */

static void mkdirp(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') { *p='\0'; mkdir(path,0755); *p='/'; }
    }
    mkdir(path, 0755);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11  quine_decompress
 * ═══════════════════════════════════════════════════════════════════════════ */

int quine_decompress(const char *dir_a,
                        const char *patch_path,
                        const char *out_dir) {
    int ret = -1;

    /* ── mmap patch ── */
    int pfd = open(patch_path, O_RDONLY);
    if (pfd < 0) { SET_ERR("open patch: %s", strerror(errno)); return -1; }
    struct stat pst; fstat(pfd, &pst);
    const uint8_t *pmem = mmap(NULL, pst.st_size,
                                PROT_READ, MAP_PRIVATE, pfd, 0);
    close(pfd);
    if (pmem == MAP_FAILED) { SET_ERR("mmap patch"); return -1; }
    madvise((void*)pmem, pst.st_size, MADV_SEQUENTIAL);

    PR r = { pmem, (size_t)pst.st_size, 0 };

    /* ── verify header ── */
    progress("read_header", NULL, 0, 0);
    {
        uint8_t magic[4]; pr_read(&r, magic, 4);
        if (memcmp(magic, QN_MAGIC, 4)) {
            SET_ERR("bad magic"); goto out_unmap;
        }
        uint8_t ver; pr_u8(&r, &ver);
        if (ver != QN_VERSION) {
            SET_ERR("version mismatch (got %u want %u)", ver, QN_VERSION);
            goto out_unmap;
        }
    }
    uint32_t cmin, cavg, cmax;
    pr_u32(&r,&cmin); pr_u32(&r,&cavg); pr_u32(&r,&cmax);

    /* ── build slot table from A manifest ── */
    SlotTable st = {0};
    uint64_t flat_pos = 0;

    uint16_t a_count = 0; pr_u16(&r, &a_count);
    for (uint16_t i = 0; i < a_count; i++) {
        uint16_t plen = 0; pr_u16(&r, &plen);
        if (pr_need(&r, plen)) goto out_slots;
        char abs[4096];
        int n = snprintf(abs, sizeof(abs), "%s/", dir_a);
        if (n + plen + 1 > (int)sizeof(abs)) {
            SET_ERR("A path too long"); goto out_slots;
        }
        memcpy(abs + n, r.base + r.pos, plen);
        abs[n + plen] = '\0';
        r.pos += plen;
        uint64_t sz = 0; pr_u64(&r, &sz);

        int fd = open(abs, O_RDONLY);
        /* fd == -1 is stored; st_resolve will return -1 if referenced */
        st_add(&st, flat_pos, sz, fd);
        flat_pos += sz;
    }
    uint64_t a_total = flat_pos;

    /* ── read B manifest to pre-populate slot table with fd=-1 ── */
    uint16_t b_count = 0; pr_u16(&r, &b_count);

    /*
     * We need B file info (path, size) during the opcode loop.
     * Store them in a small parallel array — just path strings and sizes.
     */
    char    **b_paths = calloc(b_count, sizeof(char*));
    uint64_t *b_sizes = calloc(b_count, sizeof(uint64_t));
    if (!b_paths || !b_sizes) { SET_ERR("OOM"); goto out_slots; }

    flat_pos = a_total;
    for (uint16_t i = 0; i < b_count; i++) {
        uint16_t plen = 0; pr_u16(&r, &plen);
        if (pr_need(&r, plen)) goto out_binfo;
        char *rel = malloc(plen + 1);
        if (!rel) { SET_ERR("OOM"); goto out_binfo; }
        memcpy(rel, r.base + r.pos, plen); rel[plen] = '\0';
        r.pos += plen;
        uint64_t sz = 0; pr_u64(&r, &sz);

        /* absolute output path */
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s/%s", out_dir, rel);
        free(rel);
        b_paths[i] = strdup(abs);
        b_sizes[i] = sz;

        /* register slot with fd=-1; fd filled in as each file completes */
        st_add(&st, flat_pos, sz, -1);
        flat_pos += sz;
    }

    /* ── ensure output root exists ── */
    { char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",out_dir); mkdirp(tmp); }

    /* ── ONE copy buffer for all REF operations ── */
    uint8_t *copybuf = malloc(cmax);
    if (!copybuf) { SET_ERR("OOM copybuf"); goto out_binfo; }

    /* ── opcode loop ── */
    int     out_fd    = -1;    /* current output file write-fd  */
    int     out_rd    = -1;    /* current output file read-fd   */
    uint16_t b_idx    = 0;     /* which B file we are writing   */
    int      slot_idx = (int)a_count;   /* index into st for current B file */

    /*
     * Index into st.s for the B file currently being written.
     * We open a read-fd on NEWFILE so self-referential REFs work immediately.
     */

    while (1) {
        uint8_t op;
        if (pr_u8(&r, &op)) goto out_copy;
        if (op == QN_OP_END) break;

        /* ── NEWFILE ── */
        if (op == QN_OP_NEWFILE) {
            /* Seal the previous B file: close write-fd, install read-fd in
             * slot table so subsequent files can reference it. */
            if (out_fd >= 0) {
                close(out_fd); out_fd = -1;
                /* out_rd already registered in slot; leave it open */
                out_rd = -1;
            }

            uint16_t plen = 0; pr_u16(&r, &plen);
            if (pr_need(&r, plen)) goto out_copy;
            /* path is already stored in b_paths[b_idx]; just advance pos */
            r.pos += plen;

            const char *outpath = b_paths[b_idx];

            /* create parent directories */
            char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",outpath);
            char *sl = strrchr(tmp,'/');
            if (sl && sl != tmp) { *sl='\0'; mkdirp(tmp); }

            /* open for writing */
            out_fd = open(outpath, O_CREAT|O_WRONLY|O_TRUNC, 0644);
            if (out_fd < 0) {
                SET_ERR("open output %s: %s", outpath, strerror(errno));
                goto out_copy;
            }

            /*
             * Open a separate read-fd on the same file immediately.
             * This allows self-referential REFs (later chunks referencing
             * earlier chunks of the same file) to work via pread() without
             * touching the write position.
             */
            out_rd = open(outpath, O_RDONLY);
            if (out_rd < 0) {
                SET_ERR("open read-fd %s: %s", outpath, strerror(errno));
                goto out_copy;
            }

            /* Register read-fd in the slot table for this B file */
            slot_idx = (int)a_count + (int)b_idx;
            st.s[slot_idx].fd = out_rd;

            b_idx++;
            progress("restore", b_paths[b_idx - 1], b_idx, b_count);
            continue;
        }

        /* ── REF ── */
        if (op == QN_OP_REF) {
            uint64_t off; uint32_t len;
            if (pr_u64(&r,&off) || pr_u32(&r,&len)) goto out_copy;
            if (out_fd < 0) { SET_ERR("REF before NEWFILE"); goto out_copy; }
            if (len > cmax)  { SET_ERR("REF len %u > chunk_max", len); goto out_copy; }

            int      src_fd;
            uint64_t local_off;
            if (st_resolve(&st, off, &src_fd, &local_off)) {
                SET_ERR("REF offset %llu not in slot table",
                        (unsigned long long)off);
                goto out_copy;
            }

            ssize_t got = pread(src_fd, copybuf, len, (off_t)local_off);
            if (got != (ssize_t)len) {
                SET_ERR("pread at global %llu: got %zd expected %u",
                        (unsigned long long)off, got, len);
                goto out_copy;
            }
            if (write(out_fd, copybuf, len) != (ssize_t)len) {
                SET_ERR("write: %s", strerror(errno)); goto out_copy;
            }
            continue;
        }

        /* ── LIT ── */
        if (op == QN_OP_LIT) {
            uint32_t len; pr_u32(&r,&len);
            if (pr_need(&r,len)) goto out_copy;
            if (out_fd < 0) { SET_ERR("LIT before NEWFILE"); goto out_copy; }
            if (write(out_fd, r.base + r.pos, len) != (ssize_t)len) {
                SET_ERR("write lit: %s", strerror(errno)); goto out_copy;
            }
            r.pos += len;
            continue;
        }

        SET_ERR("unknown opcode 0x%02x at %zu", op, r.pos-1);
        goto out_copy;
    }
    ret = 0;   /* success */

out_copy:
    free(copybuf);
    if (out_fd >= 0) close(out_fd);
    /* out_rd is registered in st and will be closed by st_free */
out_binfo:
    for (uint16_t i = 0; i < b_count; i++) free(b_paths[i]);
    free(b_paths); free(b_sizes);
out_slots:
    st_free(&st);
out_unmap:
    munmap((void*)pmem, pst.st_size);
    return ret;
}
