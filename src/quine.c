/*
 * quine.c — CDC delta compression with unified flat address space
 *
 * See quine.h for the full algorithm description.
 *
 * Platform: Linux/POSIX.
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
#include <pthread.h>
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
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RABIN_POLY   0xbfe6b8a5bf378d83ULL
#define CHUNK_MASK   ((1u << 11) - 1)          /* avg 2 KB */

static uint64_t g_rabin_table[256];
static pthread_once_t g_rabin_once = PTHREAD_ONCE_INIT;

static void rabin_init_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t h = (uint64_t)i;
        for (int j = 0; j < 8; j++)
            h = (h >> 1) ^ (RABIN_POLY & -(h & 1));
        g_rabin_table[i] = h;
    }
}

typedef struct {
    uint8_t  win[QN_RABIN_WINDOW];
    int      wpos;
    uint64_t hash;
    uint32_t len;
} Rabin;

static void rabin_reset(Rabin *r) {
    pthread_once(&g_rabin_once, rabin_init_tables);
    memset(r, 0, sizeof(*r));
}

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
    char    *rel;
    uint64_t size;
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

static int collect_files(const char *root, FileList *fl) {
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
 * §2b  Mmap table
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *data;
    size_t         size;
} MmapFile;

static MmapFile *mmap_all(const char *root, const FileList *fl, int prefetch) {
    MmapFile *maps = calloc(fl->count, sizeof(MmapFile));
    if (!maps) return NULL;
    for (uint32_t i = 0; i < fl->count; i++) {
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s/%s", root, fl->e[i].rel);
        int fd = open(abs, O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        fstat(fd, &st);
        if (st.st_size == 0) { close(fd); continue; }
        const uint8_t *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) continue;
        madvise((void*)p, st.st_size, MADV_SEQUENTIAL);
        if (prefetch)
            madvise((void*)p, st.st_size, MADV_WILLNEED);
        maps[i].data = p;
        maps[i].size = (size_t)st.st_size;
    }
    return maps;
}

static void munmap_all(MmapFile *maps, uint32_t count) {
    if (!maps) return;
    for (uint32_t i = 0; i < count; i++)
        if (maps[i].data) munmap((void*)maps[i].data, maps[i].size);
    free(maps);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §2c  Pre-computed chunk list
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t file_offset;
    uint32_t len;
    uint8_t  sha[32];
} PreChunk;

typedef struct {
    PreChunk *c;
    uint32_t  count;
    uint32_t  cap;
} PreChunkList;

static void pcl_free(PreChunkList *l) { free(l->c); l->c = NULL; l->count = l->cap = 0; }

static int pcl_add(PreChunkList *l, uint64_t off, uint32_t len, const uint8_t *sha) {
    if (l->count == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 64;
        PreChunk *n = realloc(l->c, nc * sizeof(PreChunk));
        if (!n) return -1;
        l->c = n; l->cap = nc;
    }
    l->c[l->count].file_offset = off;
    l->c[l->count].len         = len;
    memcpy(l->c[l->count].sha, sha, 32);
    l->count++;
    return 0;
}

/*
 * CDC + SHA-256 a byte range, storing results in pcl.
 * base_offset is added to each chunk's file_offset (for mid-file segments).
 */
static void chunk_range(const uint8_t *data, size_t size,
                         uint64_t base_offset, PreChunkList *pcl) {
    if (!data || size == 0) return;

    Rabin r; rabin_reset(&r);
    size_t chunk_start = 0;

    for (size_t i = 0; i < size; i++) {
        if (rabin_roll(&r, data[i]) || i == size - 1) {
            uint32_t clen = (uint32_t)(i - chunk_start + 1);
            uint8_t  sha[32];
            SHA256(data + chunk_start, clen, sha);
            pcl_add(pcl, base_offset + chunk_start, clen, sha);
            chunk_start = i + 1;
            rabin_reset(&r);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Chunk index
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IDX_INIT_CAP  (1u << 20)
#define IDX_LOAD_NUM  3
#define IDX_LOAD_DEN  4

typedef struct {
    uint8_t  sha[32];
    uint64_t global_offset;
    uint32_t len;
} IdxEntry;

typedef struct {
    IdxEntry *b;
    size_t    cap;
    size_t    count;
} ChunkIdx;

static int idx_init_cap(ChunkIdx *x, size_t hint) {
    size_t cap = IDX_INIT_CAP;
    while (cap < hint) cap *= 2;
    x->cap   = cap;
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

static const IdxEntry *idx_lookup_before(const ChunkIdx *x,
                                          const uint8_t *sha,
                                          uint64_t max_end) {
    size_t i = idx_slot(sha, x->cap);
    while (x->b[i].len) {
        if (!memcmp(x->b[i].sha, sha, 32)) {
            if (x->b[i].global_offset + x->b[i].len <= max_end)
                return &x->b[i];
            return NULL;
        }
        i = (i + 1) & (x->cap - 1);
    }
    return NULL;
}

static int idx_merge(ChunkIdx *dst, const ChunkIdx *src) {
    for (size_t i = 0; i < src->cap; i++) {
        if (!src->b[i].len) continue;
        if (idx_insert(dst, src->b[i].sha,
                        src->b[i].global_offset, src->b[i].len))
            return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Patch writer (buffered fwrite)
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
static int pw_close(PW *w) { int r = pw_flush(w); if (fclose(w->f)) r = -1; return r; }

/* ═══════════════════════════════════════════════════════════════════════════
 * §5  Segment-based parallel indexing
 *
 * Files are split into segments of ~(total_bytes / nproc) each.  Large
 * files get multiple segments; small files are one segment.  This ensures
 * all cores stay busy even with few large files.
 *
 * CDC restarts at each segment boundary, so chunk boundaries at splits
 * may differ from serial processing — negligible dedup loss for large files.
 *
 * A segments only produce an index.  B segments also store pre-computed
 * chunk lists (PreChunkList) for the sequential encode phase.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t file_idx;
    uint64_t file_start;    /* byte offset within file */
    uint64_t file_end;      /* exclusive */
    uint32_t seg_idx;       /* sequential index within this file's segments */
} Segment;

typedef struct {
    Segment *s;
    uint32_t count;
    uint32_t cap;
} SegList;

static int seg_add(SegList *sl, uint32_t fi, uint64_t start, uint64_t end, uint32_t si) {
    if (sl->count == sl->cap) {
        uint32_t nc = sl->cap ? sl->cap * 2 : 64;
        Segment *n = realloc(sl->s, nc * sizeof(Segment));
        if (!n) return -1;
        sl->s = n; sl->cap = nc;
    }
    sl->s[sl->count++] = (Segment){ fi, start, end, si };
    return 0;
}

/*
 * Build segments from files, splitting any file larger than seg_size.
 * seg_size = total_bytes / nproc (minimum 1 MB to avoid excessive splits).
 */
#define MIN_SEG_SIZE (1u * 1024 * 1024)

static SegList build_segments_sz(const uint64_t *sizes, uint32_t count, int nthreads) {
    SegList sl = {0};
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++)
        total += sizes[i];

    uint64_t target = total / (uint64_t)nthreads;
    if (target < MIN_SEG_SIZE) target = MIN_SEG_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t fsz = sizes[i];
        if (fsz == 0) continue;
        uint64_t off = 0;
        uint32_t si = 0;
        while (off < (uint64_t)fsz) {
            uint64_t end = off + target;
            if (end > (uint64_t)fsz || (uint64_t)fsz - end < MIN_SEG_SIZE)
                end = (uint64_t)fsz;  /* don't leave a tiny tail */
            seg_add(&sl, i, off, end, si++);
            off = end;
        }
    }
    return sl;
}

static SegList build_segments(const MmapFile *maps, uint32_t count, int nthreads) {
    uint64_t *sizes = malloc(count * sizeof(uint64_t));
    if (!sizes) { SegList empty = {0}; return empty; }
    for (uint32_t i = 0; i < count; i++) sizes[i] = maps[i].size;
    SegList sl = build_segments_sz(sizes, count, nthreads);
    free(sizes);
    return sl;
}

/* Per-segment output: a PreChunkList.  Array is indexed by segment index
 * in the SegList and filled by the owning worker thread. */

/* ── B segment worker: reads from mmap'd data ── */

typedef struct {
    /* inputs */
    const MmapFile *maps;
    const uint64_t *offsets;
    const Segment  *segs;
    uint32_t        seg_start;
    uint32_t        seg_end;
    PreChunkList   *seg_chunks;
    /* output */
    ChunkIdx        idx;
} SegWork;

static void *seg_worker(void *arg) {
    SegWork *w = arg;
    uint64_t total_bytes = 0;
    for (uint32_t i = w->seg_start; i < w->seg_end; i++)
        total_bytes += w->segs[i].file_end - w->segs[i].file_start;
    size_t est = (size_t)(total_bytes / QN_CHUNK_AVG + 64);
    idx_init_cap(&w->idx, est * IDX_LOAD_DEN / IDX_LOAD_NUM + 1);

    for (uint32_t i = w->seg_start; i < w->seg_end; i++) {
        const Segment *s = &w->segs[i];
        const uint8_t *data = w->maps[s->file_idx].data;
        if (!data) continue;

        PreChunkList *pcl = &w->seg_chunks[i];
        chunk_range(data + s->file_start,
                    s->file_end - s->file_start,
                    s->file_start, pcl);

        uint64_t gbase = w->offsets[s->file_idx];
        for (uint32_t c = 0; c < pcl->count; c++)
            idx_insert(&w->idx, pcl->c[c].sha,
                        gbase + pcl->c[c].file_offset, pcl->c[c].len);
    }
    return NULL;
}

/* ── A segment worker: reads via read() — no mmap, low RSS ── */

typedef struct {
    /* inputs */
    const char     *root;           /* dir_a path */
    const FileList *fl;
    const uint64_t *offsets;
    const Segment  *segs;
    uint32_t        seg_start;
    uint32_t        seg_end;
    /* output */
    ChunkIdx        idx;
} ASegWork;

/* Read exactly n bytes from fd at offset off into buf. */
static int full_pread(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t got = pread(fd, p, n, off);
        if (got <= 0) return -1;
        p += got; off += got; n -= (size_t)got;
    }
    return 0;
}

static void *a_seg_worker(void *arg) {
    ASegWork *w = arg;
    uint64_t total_bytes = 0;
    for (uint32_t i = w->seg_start; i < w->seg_end; i++)
        total_bytes += w->segs[i].file_end - w->segs[i].file_start;
    size_t est = (size_t)(total_bytes / QN_CHUNK_AVG + 64);
    idx_init_cap(&w->idx, est * IDX_LOAD_DEN / IDX_LOAD_NUM + 1);

    int cur_fd = -1;
    uint32_t cur_fi = UINT32_MAX;

    for (uint32_t i = w->seg_start; i < w->seg_end; i++) {
        const Segment *s = &w->segs[i];
        size_t seg_sz = (size_t)(s->file_end - s->file_start);
        if (seg_sz == 0) continue;

        /* Open file if different from last segment */
        if (s->file_idx != cur_fi) {
            if (cur_fd >= 0) close(cur_fd);
            char abs[4096];
            snprintf(abs, sizeof(abs), "%s/%s", w->root, w->fl->e[s->file_idx].rel);
            cur_fd = open(abs, O_RDONLY);
            cur_fi = s->file_idx;
            if (cur_fd < 0) continue;
        }

        /* Read segment into temporary buffer */
        uint8_t *buf = malloc(seg_sz);
        if (!buf) continue;
        if (full_pread(cur_fd, buf, seg_sz, (off_t)s->file_start)) {
            free(buf); continue;
        }

        /* CDC + SHA-256 + index */
        PreChunkList tmp = {0};
        chunk_range(buf, seg_sz, s->file_start, &tmp);

        uint64_t gbase = w->offsets[s->file_idx];
        for (uint32_t c = 0; c < tmp.count; c++)
            idx_insert(&w->idx, tmp.c[c].sha,
                        gbase + tmp.c[c].file_offset, tmp.c[c].len);
        pcl_free(&tmp);
        free(buf);
    }
    if (cur_fd >= 0) close(cur_fd);
    return NULL;
}

static int nproc(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

/*
 * Reassemble per-file PreChunkLists from per-segment lists.
 * Segments within a file are ordered by seg_idx, so we just concatenate.
 */
static PreChunkList *reassemble_file_chunks(
        const SegList *sl, const PreChunkList *seg_chunks, uint32_t file_count) {
    PreChunkList *out = calloc(file_count, sizeof(PreChunkList));
    if (!out) return NULL;
    for (uint32_t i = 0; i < sl->count; i++) {
        uint32_t fi = sl->s[i].file_idx;
        const PreChunkList *src = &seg_chunks[i];
        for (uint32_t c = 0; c < src->count; c++)
            pcl_add(&out[fi], src->c[c].file_offset, src->c[c].len, src->c[c].sha);
    }
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6  Zero-copy literal tracker
 *
 * Tracks a contiguous run of LIT data as a pointer into the mmap'd B file.
 * When flushed, writes directly from mmap to PW — one copy, no heap buffer.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t *start;
    size_t         len;
} LitRun;

static void lit_flush(LitRun *lr, PW *pw) {
    if (!lr->len) return;
    const uint8_t *p = lr->start;
    size_t rem = lr->len;
    while (rem) {
        uint32_t take = (uint32_t)(rem > 32u*1024*1024 ? 32u*1024*1024 : rem);
        pw_u8(pw, QN_OP_LIT);
        pw_u32(pw, take);
        pw_write(pw, p, take);
        p   += take;
        rem -= take;
    }
    lr->start = NULL;
    lr->len   = 0;
}

static void lit_extend(LitRun *lr, PW *pw,
                        const uint8_t *data, uint32_t len) {
    if (lr->len && lr->start + lr->len != data)
        lit_flush(lr, pw);
    if (!lr->len) lr->start = data;
    lr->len += len;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7  quine_compress
 *
 * Phase 1: mmap all files, prefetch
 * Phase 2: build segments, parallel CDC + SHA-256 + index (A and B)
 * Phase 3: merge indexes (A first, then B in thread order)
 * Phase 4: sequential encode using pre-computed chunks + filtered lookup
 * ═══════════════════════════════════════════════════════════════════════════ */

int quine_compress(const char *dir_a, const char *dir_b,
                      const char *patch_path) {
    int ret = -1;
    int nth = nproc();

    /* ── 1. Collect file lists ── */
    FileList fla = {0}, flb = {0};
    progress("scan_a", NULL, 0, 0);
    if (collect_files(dir_a, &fla)) { SET_ERR("walk A failed"); goto done; }
    progress("scan_b", NULL, 0, 0);
    if (collect_files(dir_b, &flb)) { SET_ERR("walk B failed"); goto done; }

    /* ── 2. Mmap B files (needed for encode); A uses read() for indexing ── */
    progress("mmap_b", NULL, 0, 0);
    MmapFile *maps_b = mmap_all(dir_b, &flb, 1);
    if (!maps_b) { SET_ERR("mmap B files"); goto done; }

    /* ── 3. Build global offset tables ── */
    uint64_t *off_a = malloc(fla.count * sizeof(uint64_t));
    uint64_t *off_b = malloc(flb.count * sizeof(uint64_t));
    if ((!off_a && fla.count) || (!off_b && flb.count)) {
        SET_ERR("OOM offsets"); free(off_a); free(off_b);
        munmap_all(maps_b, flb.count); goto done;
    }
    uint64_t flat_pos = 0;
    for (uint32_t i = 0; i < fla.count; i++) {
        off_a[i] = flat_pos; flat_pos += fla.e[i].size;
    }
    for (uint32_t i = 0; i < flb.count; i++) {
        off_b[i] = flat_pos; flat_pos += flb.e[i].size;
    }

    /* ── 4. Build segments and launch parallel workers ── */
    /* A segments from file sizes (no mmap); B segments from mmap'd data */
    uint64_t *a_sizes = malloc(fla.count * sizeof(uint64_t));
    if (!a_sizes && fla.count) {
        SET_ERR("OOM"); free(off_a); free(off_b);
        munmap_all(maps_b, flb.count); goto done;
    }
    for (uint32_t i = 0; i < fla.count; i++) a_sizes[i] = fla.e[i].size;
    SegList segs_a = build_segments_sz(a_sizes, fla.count, nth);
    free(a_sizes);
    SegList segs_b = build_segments(maps_b, flb.count, nth);

    int nth_a = nth;
    if (nth_a > (int)segs_a.count) nth_a = (int)segs_a.count;
    if (nth_a < 1) nth_a = 1;

    int nth_b = nth;
    if (nth_b > (int)segs_b.count) nth_b = (int)segs_b.count;
    if (nth_b < 1) nth_b = 1;

    int total_threads = nth_a + nth_b;

    PreChunkList *seg_chunks_b = calloc(segs_b.count, sizeof(PreChunkList));
    ASegWork     *a_works      = calloc(nth_a, sizeof(ASegWork));
    SegWork      *b_works      = calloc(nth_b, sizeof(SegWork));
    pthread_t    *tids         = calloc(total_threads, sizeof(pthread_t));

    if (!seg_chunks_b || !a_works || !b_works || !tids) {
        SET_ERR("OOM threads");
        free(seg_chunks_b);
        free(a_works); free(b_works); free(tids);
        free(segs_a.s); free(segs_b.s);
        free(off_a); free(off_b);
        munmap_all(maps_b, flb.count); goto done;
    }

    /* Distribute segments across threads */
    {
        /* A threads — read()-based workers */
        uint32_t per = segs_a.count / (uint32_t)nth_a;
        uint32_t rem = segs_a.count % (uint32_t)nth_a;
        uint32_t pos = 0;
        for (int t = 0; t < nth_a; t++) {
            a_works[t].root      = dir_a;
            a_works[t].fl        = &fla;
            a_works[t].offsets   = off_a;
            a_works[t].segs      = segs_a.s;
            a_works[t].seg_start = pos;
            uint32_t n = per + (t < (int)rem ? 1 : 0);
            a_works[t].seg_end   = pos + n;
            pos += n;
        }
        /* B threads */
        per = segs_b.count / (uint32_t)nth_b;
        rem = segs_b.count % (uint32_t)nth_b;
        pos = 0;
        for (int t = 0; t < nth_b; t++) {
            b_works[t].maps       = maps_b;
            b_works[t].offsets    = off_b;
            b_works[t].segs       = segs_b.s;
            b_works[t].seg_chunks = seg_chunks_b;
            b_works[t].seg_start  = pos;
            uint32_t n = per + (t < (int)rem ? 1 : 0);
            b_works[t].seg_end    = pos + n;
            pos += n;
        }
    }

    progress("index", NULL, 0, 0);

    int ti = 0;
    for (int t = 0; t < nth_a; t++)
        pthread_create(&tids[ti++], NULL, a_seg_worker, &a_works[t]);
    for (int t = 0; t < nth_b; t++)
        pthread_create(&tids[ti++], NULL, seg_worker, &b_works[t]);
    for (int t = 0; t < total_threads; t++)
        pthread_join(tids[t], NULL);
    free(tids);

    /* ── 5. Merge indexes: A first (smallest offsets), then B in order ── */
    progress("merge_index", NULL, 0, 0);
    size_t total_chunks = 0;
    for (int t = 0; t < nth_a; t++) total_chunks += a_works[t].idx.count;
    for (int t = 0; t < nth_b; t++) total_chunks += b_works[t].idx.count;

    ChunkIdx idx;
    idx_init_cap(&idx, total_chunks * IDX_LOAD_DEN / IDX_LOAD_NUM + 1);

    for (int t = 0; t < nth_a; t++) {
        idx_merge(&idx, &a_works[t].idx);
        idx_free(&a_works[t].idx);
    }
    free(a_works);

    for (int t = 0; t < nth_b; t++) {
        idx_merge(&idx, &b_works[t].idx);
        idx_free(&b_works[t].idx);
    }
    free(b_works);
    free(off_a);

    /* Reassemble per-file chunk lists from per-segment lists */
    PreChunkList *b_chunks = reassemble_file_chunks(&segs_b, seg_chunks_b, flb.count);

    /* Free segment data (A has no seg_chunks — workers used temp buffers) */
    free(segs_a.s);
    for (uint32_t i = 0; i < segs_b.count; i++) pcl_free(&seg_chunks_b[i]);
    free(seg_chunks_b); free(segs_b.s);

    if (!b_chunks && flb.count) {
        SET_ERR("OOM reassemble"); idx_free(&idx); free(off_b);
        munmap_all(maps_b, flb.count); goto done;
    }

    /* ── 6. Open patch file and write header ── */
    progress("write_header", NULL, 0, 0);

    if (fla.count > 0xFFFF) { SET_ERR("too many A files"); goto cleanup_all; }
    if (flb.count > 0xFFFF) { SET_ERR("too many B files"); goto cleanup_all; }

    PW pw;
    if (pw_open(&pw, patch_path)) {
        SET_ERR("open patch: %s", strerror(errno)); goto cleanup_all;
    }

    pw_write(&pw, QN_MAGIC, 4);
    pw_u8(&pw, QN_VERSION);
    pw_u32(&pw, QN_CHUNK_MIN);
    pw_u32(&pw, QN_CHUNK_AVG);
    pw_u32(&pw, QN_CHUNK_MAX);

    pw_u16(&pw, (uint16_t)fla.count);
    for (uint32_t i = 0; i < fla.count; i++) {
        size_t plen = strlen(fla.e[i].rel); if (plen > 0xFFFF) plen = 0xFFFF;
        pw_u16(&pw, (uint16_t)plen);
        pw_write(&pw, fla.e[i].rel, plen);
        pw_u64(&pw, fla.e[i].size);
    }

    pw_u16(&pw, (uint16_t)flb.count);
    for (uint32_t i = 0; i < flb.count; i++) {
        size_t plen = strlen(flb.e[i].rel); if (plen > 0xFFFF) plen = 0xFFFF;
        pw_u16(&pw, (uint16_t)plen);
        pw_write(&pw, flb.e[i].rel, plen);
        pw_u64(&pw, flb.e[i].size);
    }

    /* ── 7. Sequential encode with zero-copy LIT ── */
    {
        LitRun lr = {0};
        for (uint32_t fi = 0; fi < flb.count; fi++) {
            size_t plen = strlen(flb.e[fi].rel); if (plen > 0xFFFF) plen = 0xFFFF;
            pw_u8(&pw, QN_OP_NEWFILE);
            pw_u16(&pw, (uint16_t)plen);
            pw_write(&pw, flb.e[fi].rel, plen);

            progress("encode_b", flb.e[fi].rel, fi + 1, flb.count);

            const PreChunkList *pcl = &b_chunks[fi];
            const uint8_t *fdata = maps_b[fi].data;

            if (!fdata && flb.e[fi].size > 0) {
                SET_ERR("mmap failed for B file: %s", flb.e[fi].rel);
                goto cleanup_all;
            }

            for (uint32_t ci = 0; ci < pcl->count; ci++) {
                const PreChunk *pc = &pcl->c[ci];
                uint64_t chunk_global = off_b[fi] + pc->file_offset;

                const IdxEntry *e = idx_lookup_before(&idx, pc->sha, chunk_global);
                if (e) {
                    lit_flush(&lr, &pw);
                    pw_u8(&pw, QN_OP_REF);
                    pw_u64(&pw, e->global_offset);
                    pw_u32(&pw, e->len);
                } else {
                    lit_extend(&lr, &pw, fdata + pc->file_offset, pc->len);
                }
            }
            lit_flush(&lr, &pw);
        }
    }

    pw_u8(&pw, QN_OP_END);
    pw_close(&pw);
    ret = 0;

cleanup_all:
    idx_free(&idx);
    free(off_b);
    for (uint32_t i = 0; i < flb.count; i++) pcl_free(&b_chunks[i]);
    free(b_chunks);
    munmap_all(maps_b, flb.count);

done:
    fl_free(&fla);
    fl_free(&flb);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8  Slot table
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t global_start;
    uint64_t size;
    int      fd;
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

static int st_resolve(const SlotTable *t, uint64_t off,
                       int *fd_out, uint64_t *local_out) {
    int lo = 0, hi = (int)t->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < t->s[mid].global_start)          hi = mid - 1;
        else if (off >= t->s[mid].global_start + t->s[mid].size)
                                                   lo = mid + 1;
        else {
            if (t->s[mid].fd < 0) return -1;
            *fd_out    = t->s[mid].fd;
            *local_out = off - t->s[mid].global_start;
            return 0;
        }
    }
    return -1;
}

static void st_free(SlotTable *t) {
    for (uint32_t i = 0; i < t->count; i++)
        if (t->s[i].fd >= 0) close(t->s[i].fd);
    free(t->s); t->s = NULL; t->count = t->cap = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9  Buffered patch reader (fd-based, no mmap)
 *
 * Reads the patch via read() with a 4 MB buffer.  Keeps RSS low —
 * only the buffer is resident, not the entire patch file.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PR_BUF (4u * 1024 * 1024)

typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   buf_pos;   /* next byte to consume in buf */
    size_t   buf_len;   /* valid bytes in buf           */
    uint64_t file_pos;  /* absolute position in file    */
    uint64_t file_size;
} PR;

static int pr_init(PR *r, int fd) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    r->fd        = fd;
    r->buf       = malloc(PR_BUF);
    r->buf_pos   = 0;
    r->buf_len   = 0;
    r->file_pos  = 0;
    r->file_size = (uint64_t)st.st_size;
    return r->buf ? 0 : -1;
}

static void pr_free(PR *r) { free(r->buf); r->buf = NULL; }

/* Refill buffer from fd. */
static int pr_refill(PR *r) {
    if (r->buf_pos < r->buf_len) {
        /* move remaining bytes to front */
        size_t rem = r->buf_len - r->buf_pos;
        memmove(r->buf, r->buf + r->buf_pos, rem);
        r->buf_len = rem;
    } else {
        r->buf_len = 0;
    }
    r->buf_pos = 0;
    size_t want = PR_BUF - r->buf_len;
    if (want) {
        ssize_t got = read(r->fd, r->buf + r->buf_len, want);
        if (got > 0) r->buf_len += (size_t)got;
    }
    return 0;
}

static int pr_read(PR *r, void *d, size_t n) {
    uint8_t *dst = d;
    while (n) {
        if (r->buf_pos >= r->buf_len) {
            pr_refill(r);
            if (r->buf_pos >= r->buf_len) {
                SET_ERR("patch truncated at %llu",
                        (unsigned long long)r->file_pos);
                return -1;
            }
        }
        size_t avail = r->buf_len - r->buf_pos;
        size_t take  = n < avail ? n : avail;
        memcpy(dst, r->buf + r->buf_pos, take);
        r->buf_pos += take;
        r->file_pos += take;
        dst += take;
        n -= take;
    }
    return 0;
}

/* Skip n bytes without copying. */
static int pr_skip(PR *r, size_t n) {
    while (n) {
        if (r->buf_pos >= r->buf_len) {
            pr_refill(r);
            if (r->buf_pos >= r->buf_len) {
                SET_ERR("patch truncated"); return -1;
            }
        }
        size_t avail = r->buf_len - r->buf_pos;
        size_t take  = n < avail ? n : avail;
        r->buf_pos += take;
        r->file_pos += take;
        n -= take;
    }
    return 0;
}

/*
 * Write n bytes from the patch directly to out_fd without intermediate
 * buffer copy.  Reads from patch buffer (or refills) in chunks and
 * writes directly to the output.
 */
static int pr_write_to(PR *r, int out_fd, size_t n) {
    while (n) {
        if (r->buf_pos >= r->buf_len) {
            pr_refill(r);
            if (r->buf_pos >= r->buf_len) {
                SET_ERR("patch truncated"); return -1;
            }
        }
        size_t avail = r->buf_len - r->buf_pos;
        size_t take  = n < avail ? n : avail;
        if (write(out_fd, r->buf + r->buf_pos, take) != (ssize_t)take) {
            SET_ERR("write lit: %s", strerror(errno)); return -1;
        }
        r->buf_pos += take;
        r->file_pos += take;
        n -= take;
    }
    return 0;
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

    /* All resources declared upfront with safe defaults.
     * Cleanup at the bottom is always safe regardless of where we stop. */
    int          pfd     = -1;
    PR           r       = {0};   /* buf=NULL → pr_free is safe */
    SlotTable    st      = {0};
    char       **b_paths = NULL;
    uint8_t     *copybuf = NULL;
    int          out_fd  = -1;
    uint16_t     b_count = 0;
    uint16_t     a_count = 0;

    do {
        pfd = open(patch_path, O_RDONLY);
        if (pfd < 0) { SET_ERR("open patch: %s", strerror(errno)); break; }

        if (pr_init(&r, pfd)) { SET_ERR("init patch reader"); break; }

        /* ── header ── */
        progress("read_header", NULL, 0, 0);

        uint8_t magic[4];
        if (pr_read(&r, magic, 4)) break;
        if (memcmp(magic, QN_MAGIC, 4)) { SET_ERR("bad magic"); break; }

        uint8_t ver;
        if (pr_u8(&r, &ver)) break;
        if (ver != QN_VERSION) {
            SET_ERR("version mismatch (got %u want %u)", ver, QN_VERSION);
            break;
        }

        uint32_t cmin, cavg, cmax;
        if (pr_u32(&r,&cmin) || pr_u32(&r,&cavg) || pr_u32(&r,&cmax)) break;

        /* ── A manifest ── */
        uint64_t flat_pos = 0;
        if (pr_u16(&r, &a_count)) break;

        int ok = 1;
        for (uint16_t i = 0; i < a_count && ok; i++) {
            uint16_t plen = 0;
            if (pr_u16(&r, &plen)) { ok = 0; break; }
            char abs[4096];
            int n = snprintf(abs, sizeof(abs), "%s/", dir_a);
            if (n + plen + 1 > (int)sizeof(abs)) {
                SET_ERR("A path too long"); ok = 0; break;
            }
            if (pr_read(&r, abs + n, plen)) { ok = 0; break; }
            abs[n + plen] = '\0';
            uint64_t sz = 0;
            if (pr_u64(&r, &sz)) { ok = 0; break; }
            int fd = open(abs, O_RDONLY);
            st_add(&st, flat_pos, sz, fd);
            flat_pos += sz;
        }
        if (!ok) break;
        uint64_t a_total = flat_pos;

        /* ── B manifest ── */
        if (pr_u16(&r, &b_count)) break;
        b_paths = calloc(b_count, sizeof(char*));
        if (!b_paths) { SET_ERR("OOM"); break; }

        flat_pos = a_total;
        for (uint16_t i = 0; i < b_count && ok; i++) {
            uint16_t plen = 0;
            if (pr_u16(&r, &plen)) { ok = 0; break; }
            char *rel = malloc(plen + 1);
            if (!rel) { SET_ERR("OOM"); ok = 0; break; }
            if (pr_read(&r, rel, plen)) { free(rel); ok = 0; break; }
            rel[plen] = '\0';
            uint64_t sz = 0;
            if (pr_u64(&r, &sz)) { free(rel); ok = 0; break; }
            char abs[4096];
            snprintf(abs, sizeof(abs), "%s/%s", out_dir, rel);
            free(rel);
            b_paths[i] = strdup(abs);
            st_add(&st, flat_pos, sz, -1);
            flat_pos += sz;
        }
        if (!ok) break;

        { char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",out_dir); mkdirp(tmp); }

        copybuf = malloc(cmax);
        if (!copybuf) { SET_ERR("OOM copybuf"); break; }

        /* ── opcode loop ── */
        uint16_t b_idx = 0;

        while (ok) {
            uint8_t op;
            if (pr_u8(&r, &op)) { ok = 0; break; }
            if (op == QN_OP_END) break;

            if (op == QN_OP_NEWFILE) {
                if (out_fd >= 0) { close(out_fd); out_fd = -1; }

                uint16_t plen = 0;
                if (pr_u16(&r, &plen) || pr_skip(&r, plen)) { ok = 0; break; }

                const char *outpath = b_paths[b_idx];
                char tmp[4096]; snprintf(tmp,sizeof(tmp),"%s",outpath);
                char *sl = strrchr(tmp,'/');
                if (sl && sl != tmp) { *sl='\0'; mkdirp(tmp); }

                out_fd = open(outpath, O_CREAT|O_WRONLY|O_TRUNC, 0644);
                if (out_fd < 0) {
                    SET_ERR("open output %s: %s", outpath, strerror(errno));
                    ok = 0; break;
                }

                int out_rd = open(outpath, O_RDONLY);
                if (out_rd < 0) {
                    SET_ERR("open read-fd %s: %s", outpath, strerror(errno));
                    ok = 0; break;
                }

                int slot_idx = (int)a_count + (int)b_idx;
                st.s[slot_idx].fd = out_rd;

                b_idx++;
                progress("restore", b_paths[b_idx - 1], b_idx, b_count);
                continue;
            }

            if (op == QN_OP_REF) {
                uint64_t off; uint32_t len;
                if (pr_u64(&r,&off) || pr_u32(&r,&len)) { ok = 0; break; }
                if (out_fd < 0) { SET_ERR("REF before NEWFILE"); ok = 0; break; }
                if (len > cmax) { SET_ERR("REF len %u > chunk_max", len); ok = 0; break; }

                int src_fd; uint64_t local_off;
                if (st_resolve(&st, off, &src_fd, &local_off)) {
                    SET_ERR("REF offset %llu not in slot table",
                            (unsigned long long)off);
                    ok = 0; break;
                }
                ssize_t got = pread(src_fd, copybuf, len, (off_t)local_off);
                if (got != (ssize_t)len) {
                    SET_ERR("pread at global %llu: got %zd expected %u",
                            (unsigned long long)off, got, len);
                    ok = 0; break;
                }
                if (write(out_fd, copybuf, len) != (ssize_t)len) {
                    SET_ERR("write: %s", strerror(errno)); ok = 0; break;
                }
                continue;
            }

            if (op == QN_OP_LIT) {
                uint32_t len;
                if (pr_u32(&r,&len)) { ok = 0; break; }
                if (out_fd < 0) { SET_ERR("LIT before NEWFILE"); ok = 0; break; }
                if (pr_write_to(&r, out_fd, len)) { ok = 0; break; }
                continue;
            }

            SET_ERR("unknown opcode 0x%02x at %llu", op,
                    (unsigned long long)(r.file_pos - 1));
            ok = 0;
        }
        if (!ok) break;

        ret = 0;
    } while (0);

    /* cleanup — all safe with NULL / -1 / zero-count defaults */
    free(copybuf);
    if (out_fd >= 0) close(out_fd);
    for (uint16_t i = 0; i < b_count; i++) free(b_paths[i]);
    free(b_paths);
    st_free(&st);
    pr_free(&r);
    if (pfd >= 0) close(pfd);
    return ret;
}
