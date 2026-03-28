/*
 * test.c — unit and integration tests for quine
 *
 * Build:  make test_quine
 * Run:    make test
 *         ./test_quine            # run all
 *         ./test_quine <name>     # run one test by name
 *
 * Each test creates its own isolated directory under /tmp/quine_test_<pid>/
 * and cleans up afterward.  Tests are independent and can run in any order.
 *
 * Exit code: 0 if all tests pass, 1 if any fail.
 */

#include "quine/quine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * §1  Test framework
 * ═══════════════════════════════════════════════════════════════════════════ */

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current = NULL;

#define PASS() do { \
    printf("  PASS  %s\n", g_current); g_pass++; \
} while(0)

#define FAIL(fmt, ...) do { \
    printf("  FAIL  %s: " fmt "\n", g_current, ##__VA_ARGS__); g_fail++; \
    return; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) FAIL("assertion failed: %s (line %d)", #cond, __LINE__); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) FAIL(#a " = %lld, expected %lld (line %d)", \
                         (long long)(a), (long long)(b), __LINE__); \
} while(0)

#define ASSERT_STR(a, b) do { \
    if (strcmp((a),(b))) FAIL(#a " = \"%s\", expected \"%s\" (line %d)", \
                              (a),(b), __LINE__); \
} while(0)

/* ═══════════════════════════════════════════════════════════════════════════
 * §2  Filesystem helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static char g_tmpbase[1024];

static void tmp_init(void) {
    snprintf(g_tmpbase, sizeof(g_tmpbase),
             "/tmp/quine_test_%d", (int)getpid());
    mkdir(g_tmpbase, 0755);
}

/* Build a path under the test temp dir — buf must be at least 512 bytes */
static char *P(char *buf, size_t sz, const char *fmt, ...) {
    char sub[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(sub, sizeof(sub), fmt, ap); va_end(ap);
    snprintf(buf, sz, "%s/%s", g_tmpbase, sub);
    return buf;
}

static void mkdirs(const char *path) {
    char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp+1; *p; p++) {
        if (*p == '/') { *p='\0'; mkdir(tmp,0755); *p='/'; }
    }
    mkdir(tmp, 0755);
}

/* Write arbitrary bytes to a file, creating parent dirs */
static void write_file(const char *path, const void *data, size_t len) {
    char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", path);
    char *sl = strrchr(tmp, '/');
    if (sl) { *sl='\0'; mkdirs(tmp); }
    int fd = open(path, O_CREAT|O_WRONLY|O_TRUNC, 0644);
    if (fd < 0) { perror(path); exit(1); }
    ssize_t wr = write(fd, data, len);
    (void)wr;
    close(fd);
}

/* Read entire file into heap buffer; caller frees. Returns size via *sz. */
static uint8_t *read_file(const char *path, size_t *sz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { *sz = 0; return NULL; }
    struct stat st; fstat(fd, &st);
    *sz = (size_t)st.st_size;
    uint8_t *buf = malloc(*sz + 1);
    size_t done = 0;
    while (done < *sz) {
        ssize_t n = read(fd, buf + done, *sz - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    buf[*sz] = 0;
    return buf;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int64_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

/* Compare two files byte-for-byte; return 0 if identical */
static int files_equal(const char *a, const char *b) {
    size_t sa, sb;
    uint8_t *da = read_file(a, &sa);
    uint8_t *db = read_file(b, &sb);
    int eq = (sa == sb) && da && db && memcmp(da, db, sa) == 0;
    free(da); free(db);
    return eq;
}

/* Recursively compare two directories; return 0 if identical */
static int dirs_equal(const char *a, const char *b);
static int dirs_equal(const char *a, const char *b) {
    DIR *da = opendir(a);
    if (!da) return 0;
    struct dirent *de;
    int ok = 1;
    while (ok && (de = readdir(da))) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
        char pa[4096], pb[4096];
        snprintf(pa, sizeof(pa), "%s/%s", a, de->d_name);
        snprintf(pb, sizeof(pb), "%s/%s", b, de->d_name);
        struct stat st;
        if (lstat(pa, &st)) { ok=0; break; }
        if (S_ISDIR(st.st_mode)) ok = dirs_equal(pa, pb);
        else                     ok = files_equal(pa, pb);
    }
    closedir(da);
    return ok;
}

/* Deterministic pseudo-random bytes (LCG, no libc rand dependency) */
static void prng_fill(uint8_t *buf, size_t len, uint64_t seed) {
    uint64_t s = seed;
    for (size_t i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 56);
    }
}

/* Remove a directory tree */
static void rmtree(const char *path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    int rc = system(cmd); (void)rc;
}

/* ── Round-trip helper ───────────────────────────────────────────────────── */
/*
 * compress dir_a → dir_b → patch, decompress → dir_out, verify dir_out == dir_b
 * Returns 0 on success.
 */
static int roundtrip(const char *dir_a, const char *dir_b,
                     const char *patch, const char *dir_out) {
    rmtree(dir_out);
    if (quine_compress(dir_a, dir_b, patch) != 0) return -1;
    if (quine_decompress(dir_a, patch, dir_out) != 0) return -2;
    return dirs_equal(dir_b, dir_out) ? 0 : -3;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3  Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── T01: empty directories ─────────────────────────────────────────────── */
static void t_empty_dirs(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t01/a"));
    mkdirs(P(b,sizeof(b),"t01/b"));
    P(p,sizeof(p),"t01/out.patch");
    P(o,sizeof(o),"t01/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T02: single identical file ─────────────────────────────────────────── */
static void t_identical_file(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t02/a"));
    mkdirs(P(b,sizeof(b),"t02/b"));

    uint8_t data[128*1024];
    prng_fill(data, sizeof(data), 0xDEAD);

    char fa[1024], fb[512];
    snprintf(fa,sizeof(fa),"%s/data.bin",a);
    snprintf(fb,sizeof(fb),"%s/data.bin",b);
    write_file(fa, data, sizeof(data));
    write_file(fb, data, sizeof(data));

    P(p,sizeof(p),"t02/out.patch");
    P(o,sizeof(o),"t02/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Patch should be tiny: one NEWFILE + REF opcodes, no LIT body */
    int64_t psz = file_size(p);
    /* header + one NEWFILE + handful of REFs — well under 1 KB */
    if (psz > 1024) FAIL("patch too large (%lld bytes) for identical file", (long long)psz);
    PASS();
}

/* ── T03: B file is entirely new (no match in A) ────────────────────────── */
static void t_entirely_new_file(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t03/a"));
    mkdirs(P(b,sizeof(b),"t03/b"));

    uint8_t da[64*1024], db[64*1024];
    prng_fill(da, sizeof(da), 0xAAAA);
    prng_fill(db, sizeof(db), 0xBBBB);  /* completely different seed */

    char fa[1024], fb[512];
    snprintf(fa,sizeof(fa),"%s/a.bin",a);
    snprintf(fb,sizeof(fb),"%s/b.bin",b);
    write_file(fa, da, sizeof(da));
    write_file(fb, db, sizeof(db));

    P(p,sizeof(p),"t03/out.patch");
    P(o,sizeof(o),"t03/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T04: small text file, one line changed ─────────────────────────────── */
static void t_small_text_diff(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t04/a"));
    mkdirs(P(b,sizeof(b),"t04/b"));

    /* Build a text file large enough to produce multiple chunks */
    size_t lines = 20000;
    size_t llen  = 80;
    size_t total = lines * llen;
    uint8_t *ta  = malloc(total);
    uint8_t *tb  = malloc(total);
    for (size_t i = 0; i < lines; i++) {
        char line[96];
        snprintf(line, sizeof(line), "line %06zu: %.*s\n",
                 i, (int)(llen-17), "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        memcpy(ta + i*llen, line, llen);
        memcpy(tb + i*llen, line, llen);
    }
    /* Change just the last line in B */
    char lastline[96];
    snprintf(lastline, sizeof(lastline),
             "line %06zu: CHANGED_CONTENT_HERE             \n", lines-1);
    memcpy(tb + (lines-1)*llen, lastline, llen);

    char fa[1024], fb[512];
    snprintf(fa,sizeof(fa),"%s/log.txt",a);
    snprintf(fb,sizeof(fb),"%s/log.txt",b);
    write_file(fa, ta, total);
    write_file(fb, tb, total);
    free(ta); free(tb);

    P(p,sizeof(p),"t04/out.patch");
    P(o,sizeof(o),"t04/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Patch should be much smaller than the full file */
    int64_t psz = file_size(p);
    int64_t fsz = (int64_t)total;
    if (psz >= fsz / 2) FAIL("patch (%lld) not smaller than half the file (%lld)",
                              (long long)psz, (long long)fsz);
    PASS();
}

/* ── T05: multiple files, mixed match/new ───────────────────────────────── */
static void t_multiple_files(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t05/a/sub"));
    mkdirs(P(b,sizeof(b),"t05/b/sub"));

    uint8_t blob[256*1024];
    prng_fill(blob, sizeof(blob), 0x1234);

    char path[1024];

    /* A: base.bin, sub/shared.bin */
    snprintf(path,sizeof(path),"%s/base.bin",a);   write_file(path,blob,sizeof(blob));
    snprintf(path,sizeof(path),"%s/sub/shared.bin",a); write_file(path,blob,sizeof(blob)/2);

    /* B: base.bin identical, sub/shared.bin identical, new.bin all-new */
    snprintf(path,sizeof(path),"%s/base.bin",b);   write_file(path,blob,sizeof(blob));
    snprintf(path,sizeof(path),"%s/sub/shared.bin",b); write_file(path,blob,sizeof(blob)/2);
    uint8_t newdata[64*1024];
    prng_fill(newdata,sizeof(newdata),0xFFFF);
    snprintf(path,sizeof(path),"%s/new.bin",b);    write_file(path,newdata,sizeof(newdata));

    P(p,sizeof(p),"t05/out.patch");
    P(o,sizeof(o),"t05/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T06: intra-file self-reference ─────────────────────────────────────── */
static void t_intra_file_selfref(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t06/a"));
    mkdirs(P(b,sizeof(b),"t06/b"));

    /* B has a file whose second half is identical to its first half.
     * A is empty so neither half matches A.
     * After compressing, the patch should be significantly smaller than
     * 2 × block_size because the second half is a REF into the first. */
    uint8_t block[96*1024];
    prng_fill(block, sizeof(block), 0xCAFE);

    uint8_t *file_data = malloc(2 * sizeof(block));
    memcpy(file_data,                 block, sizeof(block));
    memcpy(file_data + sizeof(block), block, sizeof(block));

    char fb[1024];
    snprintf(fb,sizeof(fb),"%s/repeat.bin",b);
    write_file(fb, file_data, 2*sizeof(block));
    free(file_data);

    P(p,sizeof(p),"t06/out.patch");
    P(o,sizeof(o),"t06/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Patch should contain the block roughly once as LIT, not twice */
    int64_t psz = file_size(p);
    int64_t raw  = (int64_t)(2 * sizeof(block));
    /* Allow generous headroom for headers and opcodes */
    if (psz > raw * 3 / 4)
        FAIL("patch (%lld B) not significantly smaller than raw (%lld B) "
             "— intra-file dedup may not be working", (long long)psz, (long long)raw);
    PASS();
}

/* ── T07: inter-B deduplication ─────────────────────────────────────────── */
static void t_inter_b_dedup(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t07/a"));
    mkdirs(P(b,sizeof(b),"t07/b"));

    /* Two identical B files, content not in A.
     * copy2 should be a full REF into copy1. */
    uint8_t blob[128*1024];
    prng_fill(blob, sizeof(blob), 0xBEEF);

    char fc1[1024], fc2[512];
    snprintf(fc1,sizeof(fc1),"%s/copy1.bin",b);
    snprintf(fc2,sizeof(fc2),"%s/copy2.bin",b);
    write_file(fc1, blob, sizeof(blob));
    write_file(fc2, blob, sizeof(blob));

    P(p,sizeof(p),"t07/out.patch");
    P(o,sizeof(o),"t07/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Patch should hold the blob approximately once, not twice */
    int64_t psz = file_size(p);
    int64_t single = (int64_t)sizeof(blob);
    if (psz > single * 3 / 2)
        FAIL("patch (%lld B) suggests copy2 was not deduplicated against copy1 (%lld B each)",
             (long long)psz, (long long)single);
    PASS();
}

/* ── T08: empty file in B ───────────────────────────────────────────────── */
static void t_empty_file_in_b(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t08/a"));
    mkdirs(P(b,sizeof(b),"t08/b"));

    /* A has a normal file; B has an empty file and a normal file */
    uint8_t data[32*1024];
    prng_fill(data, sizeof(data), 0x1111);

    char fa[1024], fb1[512], fb2[512];
    snprintf(fa, sizeof(fa),  "%s/normal.bin", a);
    snprintf(fb1,sizeof(fb1), "%s/normal.bin", b);
    snprintf(fb2,sizeof(fb2), "%s/empty.bin",  b);
    write_file(fa,  data, sizeof(data));
    write_file(fb1, data, sizeof(data));
    write_file(fb2, NULL, 0);

    P(p,sizeof(p),"t08/out.patch");
    P(o,sizeof(o),"t08/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Verify empty file exists in output with size 0 */
    char fe[1024];
    snprintf(fe, sizeof(fe), "%s/empty.bin", o);
    if (!file_exists(fe)) FAIL("empty.bin missing from output");
    if (file_size(fe) != 0) FAIL("empty.bin has non-zero size in output");
    PASS();
}

/* ── T09: chunk boundary at exact file boundary (stress CDC) ────────────── */
static void t_exact_chunk_boundary(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t09/a"));
    mkdirs(P(b,sizeof(b),"t09/b"));

    /* Files sized at exact multiples of chunk_min and chunk_max */
    uint8_t *d_min = malloc(QN_CHUNK_MIN);
    uint8_t *d_max = malloc(QN_CHUNK_MAX);
    uint8_t *d_avg = malloc(QN_CHUNK_AVG);
    prng_fill(d_min, QN_CHUNK_MIN, 0xAAA1);
    prng_fill(d_max, QN_CHUNK_MAX, 0xAAA2);
    prng_fill(d_avg, QN_CHUNK_AVG, 0xAAA3);

    char path[1024];
    snprintf(path,sizeof(path),"%s/min.bin",a); write_file(path,d_min,QN_CHUNK_MIN);
    snprintf(path,sizeof(path),"%s/max.bin",a); write_file(path,d_max,QN_CHUNK_MAX);
    snprintf(path,sizeof(path),"%s/min.bin",b); write_file(path,d_min,QN_CHUNK_MIN);
    snprintf(path,sizeof(path),"%s/max.bin",b); write_file(path,d_max,QN_CHUNK_MAX);
    snprintf(path,sizeof(path),"%s/avg.bin",b); write_file(path,d_avg,QN_CHUNK_AVG);
    free(d_min); free(d_max); free(d_avg);

    P(p,sizeof(p),"t09/out.patch");
    P(o,sizeof(o),"t09/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T10: B has files in subdirectories ─────────────────────────────────── */
static void t_subdirectories(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t10/a/x/y"));
    mkdirs(P(b,sizeof(b),"t10/b/x/y/z"));

    uint8_t data[64*1024];
    prng_fill(data, sizeof(data), 0x7777);

    char path[1024];
    snprintf(path,sizeof(path),"%s/x/base.bin",a);     write_file(path,data,sizeof(data));
    snprintf(path,sizeof(path),"%s/x/base.bin",b);     write_file(path,data,sizeof(data));
    prng_fill(data,sizeof(data),0x8888);
    snprintf(path,sizeof(path),"%s/x/y/mid.bin",b);    write_file(path,data,sizeof(data));
    prng_fill(data,sizeof(data),0x9999);
    snprintf(path,sizeof(path),"%s/x/y/z/deep.bin",b); write_file(path,data,sizeof(data));

    P(p,sizeof(p),"t10/out.patch");
    P(o,sizeof(o),"t10/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T11: chunk spans A file boundary (flat space key feature) ───────────── */
static void t_cross_file_boundary_match(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t11/a"));
    mkdirs(P(b,sizeof(b),"t11/b"));

    /*
     * A has two files: tail.bin and head.bin.
     * B has one file whose content = tail end of A/file1 + head of A/file2.
     * In the flat space [file1|file2], that region is a contiguous run that
     * CDC will find as a chunk.  This verifies cross-file-boundary matching.
     *
     * We use large padding so the CDC boundary lands inside our crafted region.
     */
    size_t pad = 200*1024;
    size_t mid =  32*1024;

    uint8_t *file1 = malloc(pad + mid);
    uint8_t *file2 = malloc(mid + pad);
    prng_fill(file1,       pad,        0xF001);
    prng_fill(file1 + pad, mid,        0xF002);  /* tail */
    prng_fill(file2,       mid,        0xF002);  /* head — same as tail */
    prng_fill(file2 + mid, pad,        0xF003);

    char fa1[1024], fa2[512], fb[512];
    snprintf(fa1,sizeof(fa1),"%s/aaa_file1.bin",a);
    snprintf(fa2,sizeof(fa2),"%s/bbb_file2.bin",a);
    snprintf(fb, sizeof(fb), "%s/combined.bin", b);

    write_file(fa1, file1, pad + mid);
    write_file(fa2, file2, mid + pad);

    /* B file: the overlapping mid region */
    write_file(fb, file1 + pad, mid);

    free(file1); free(file2);

    P(p,sizeof(p),"t11/out.patch");
    P(o,sizeof(o),"t11/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T12: all-zeros file (pathological for CDC, triggers max-chunk cuts) ── */
static void t_all_zeros(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t12/a"));
    mkdirs(P(b,sizeof(b),"t12/b"));

    size_t sz = 4 * QN_CHUNK_MAX;   /* 4 × 64 KB = 256 KB */
    uint8_t *zeros = calloc(1, sz);

    char fa[1024], fb[512];
    snprintf(fa,sizeof(fa),"%s/zeros.bin",a);
    snprintf(fb,sizeof(fb),"%s/zeros.bin",b);
    write_file(fa, zeros, sz);
    write_file(fb, zeros, sz);
    free(zeros);

    P(p,sizeof(p),"t12/out.patch");
    P(o,sizeof(o),"t12/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");
    PASS();
}

/* ── T13: bad magic in patch file ───────────────────────────────────────── */
static void t_bad_magic(void) {
    char a[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t13/a"));
    P(p,sizeof(p),"t13/bad.patch");
    P(o,sizeof(o),"t13/out");

    /* Write a patch file with corrupt magic */
    uint8_t bad[16] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0,0,0,0,0,0,0,0,0,0,0 };
    write_file(p, bad, sizeof(bad));

    int r = quine_decompress(a, p, o);
    if (r == 0) FAIL("decompress should fail on bad magic but returned 0");
    /* errmsg should mention magic */
    const char *msg = quine_errmsg();
    if (!strstr(msg, "magic") && !strstr(msg, "Magic"))
        FAIL("error message '%s' does not mention magic", msg);
    PASS();
}

/* ── T14: version mismatch in patch file ────────────────────────────────── */
static void t_version_mismatch(void) {
    char a[1024], b[1024], p[1024], o[1024], p2[256];
    mkdirs(P(a,sizeof(a),"t14/a"));
    mkdirs(P(b,sizeof(b),"t14/b"));

    uint8_t data[16*1024];
    prng_fill(data,sizeof(data),0x1414);
    char fb[1024]; snprintf(fb,sizeof(fb),"%s/f.bin",b);
    write_file(fb, data, sizeof(data));

    P(p, sizeof(p),  "t14/good.patch");
    P(p2,sizeof(p2), "t14/bad_ver.patch");
    P(o, sizeof(o),  "t14/out");

    quine_compress(a, b, p);

    /* Read good patch, flip version byte (offset 4), write bad patch */
    size_t psz;
    uint8_t *pbuf = read_file(p, &psz);
    pbuf[4] = 0xFF;  /* corrupt version */
    write_file(p2, pbuf, psz);
    free(pbuf);

    int r = quine_decompress(a, p2, o);
    if (r == 0) FAIL("decompress should fail on version mismatch");
    const char *msg = quine_errmsg();
    if (!strstr(msg,"version") && !strstr(msg,"Version"))
        FAIL("error message '%s' does not mention version", msg);
    PASS();
}

/* ── T15: large realistic workload ──────────────────────────────────────── */
static void t_large_realistic(void) {
    char a[1024], b[1024], p[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t15/a/src"));
    mkdirs(P(b,sizeof(b),"t15/b/src"));
    mkdirs(P(b,sizeof(b),"t15/b/assets"));

    /*
     * Simulate a software release:
     * A = v1.0: several source files + a binary blob
     * B = v1.1: mostly the same, two files changed, one file added
     */
    char path[1024];
    uint8_t buf[64*1024];

    /* shared binary (identical in A and B) */
    prng_fill(buf, sizeof(buf), 0xABCD);
    snprintf(path,sizeof(path),"%s/src/engine.bin",a); write_file(path,buf,sizeof(buf));
    snprintf(path,sizeof(path),"%s/src/engine.bin",b); write_file(path,buf,sizeof(buf));

    /* large shared asset (identical) */
    uint8_t *asset = malloc(512*1024);
    prng_fill(asset, 512*1024, 0x5555);
    snprintf(path,sizeof(path),"%s/assets/sprites.dat",a); write_file(path,asset,512*1024);
    snprintf(path,sizeof(path),"%s/assets/sprites.dat",b); write_file(path,asset,512*1024);
    free(asset);

    /* source file: 95% same, small patch at the end */
    uint8_t *src = malloc(200*1024);
    prng_fill(src, 200*1024, 0x1234);
    snprintf(path,sizeof(path),"%s/src/main.c",a); write_file(path,src,200*1024);
    prng_fill(src + 195*1024, 5*1024, 0x9999);     /* last 5 KB changed */
    snprintf(path,sizeof(path),"%s/src/main.c",b); write_file(path,src,200*1024);
    free(src);

    /* entirely new file in B */
    prng_fill(buf, sizeof(buf), 0xCCDD);
    snprintf(path,sizeof(path),"%s/src/newfeature.bin",b); write_file(path,buf,sizeof(buf));

    P(p,sizeof(p),"t15/out.patch");
    P(o,sizeof(o),"t15/out");

    int r = roundtrip(a, b, p, o);
    if (r == -1) FAIL("compress: %s", quine_errmsg());
    if (r == -2) FAIL("decompress: %s", quine_errmsg());
    if (r == -3) FAIL("output mismatch");

    /* Sanity: patch should be well under the total size of B */
    int64_t psz = file_size(p);
    /* B total ~ 512+64+200+64 KB = ~840 KB. Patch should be << that. */
    if (psz > 200*1024)
        FAIL("patch (%lld KB) unexpectedly large for this workload",
             (long long)(psz/1024));
    PASS();
}

/* ── T16: compress idempotence — compress twice, same patch size ─────────── */
static void t_idempotent(void) {
    char a[1024], b[1024], p1[1024], p2[1024], o[1024];
    mkdirs(P(a,sizeof(a),"t16/a"));
    mkdirs(P(b,sizeof(b),"t16/b"));

    uint8_t data[128*1024];
    prng_fill(data, sizeof(data), 0xEEFF);
    char fb[1024]; snprintf(fb,sizeof(fb),"%s/data.bin",b);
    write_file(fb, data, sizeof(data));

    P(p1,sizeof(p1),"t16/p1.patch");
    P(p2,sizeof(p2),"t16/p2.patch");
    P(o, sizeof(o), "t16/out");

    quine_compress(a, b, p1);
    quine_compress(a, b, p2);

    int64_t s1 = file_size(p1), s2 = file_size(p2);
    if (s1 != s2) FAIL("patch sizes differ across two compress runs: %lld vs %lld",
                        (long long)s1, (long long)s2);

    int r = quine_decompress(a, p1, o);
    if (r != 0) FAIL("decompress p1: %s", quine_errmsg());
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4  Test registry and runner
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct { const char *name; void (*fn)(void); } Test;

static const Test TESTS[] = {
    { "empty_dirs",              t_empty_dirs              },
    { "identical_file",          t_identical_file          },
    { "entirely_new_file",       t_entirely_new_file       },
    { "small_text_diff",         t_small_text_diff         },
    { "multiple_files",          t_multiple_files          },
    { "intra_file_selfref",      t_intra_file_selfref      },
    { "inter_b_dedup",           t_inter_b_dedup           },
    { "empty_file_in_b",         t_empty_file_in_b         },
    { "exact_chunk_boundary",    t_exact_chunk_boundary    },
    { "subdirectories",          t_subdirectories          },
    { "cross_file_boundary",     t_cross_file_boundary_match},
    { "all_zeros",               t_all_zeros               },
    { "bad_magic",               t_bad_magic               },
    { "version_mismatch",        t_version_mismatch        },
    { "large_realistic",         t_large_realistic         },
    { "idempotent",              t_idempotent              },
};
static const int NTEST = (int)(sizeof(TESTS)/sizeof(TESTS[0]));

int main(int argc, char **argv) {
    tmp_init();
    printf("quine test suite  (tmp: %s)\n\n", g_tmpbase);

    int run = 0;
    for (int i = 0; i < NTEST; i++) {
        /* If a name filter was given, skip non-matching tests */
        if (argc > 1 && strcmp(argv[1], TESTS[i].name) != 0) continue;
        g_current = TESTS[i].name;
        TESTS[i].fn();
        run++;
    }

    if (argc > 1 && run == 0) {
        fprintf(stderr, "no test named '%s'\n", argv[1]);
        fprintf(stderr, "available tests:\n");
        for (int i = 0; i < NTEST; i++)
            fprintf(stderr, "  %s\n", TESTS[i].name);
        return 1;
    }

    printf("\n%d passed, %d failed  (%d run)\n", g_pass, g_fail, run);

    /* Clean up temp dir only on full success */
    if (g_fail == 0) rmtree(g_tmpbase);

    return g_fail ? 1 : 0;
}
