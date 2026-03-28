/*
 * main.c — CLI driver for quine
 *
 * Usage:
 *   quine compress   <dir_a> <dir_b> <output.patch>
 *   quine decompress <dir_a> <input.patch> <out_dir>
 *
 * After each operation, prints:
 *   - Wall time
 *   - CPU time (user + sys) and effective core utilisation
 *   - Peak RSS (resident set size) in KB
 *   - Input / output sizes and compression ratio (compress only)
 *
 * Compression also runs a verification pass: decompress the patch into a
 * temporary directory and byte-compare against dir_b.
 */
#include "quine/quine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

/* ── Progress callback ─────────────────────────────────────────────────── */

/*
 * Prints step name when it begins, elapsed time when the next step starts.
 * Example output:
 *   [scan_a]                                0.0s
 *   [index] (1/9) big_model.bin            42.3s
 *   [index] (2/9) small.json                0.1s
 *   [encode_b] ...
 */

static struct timespec g_step_start;
static int             g_step_pending = 0; /* 1 = a line is open, needs elapsed */

static void progress_timer_reset(void) {
    g_step_pending = 0;
}

static void progress_finish_line(void) {
    if (!g_step_pending) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec  - g_step_start.tv_sec) +
                     (now.tv_nsec - g_step_start.tv_nsec) * 1e-9;
    printf("  %.1fs\n", elapsed);
    g_step_pending = 0;
}

static void cli_progress(const quine_progress_t *info, void *ctx) {
    (void)ctx;

    /* finish the previous line with its elapsed time */
    progress_finish_line();

    /* start new line: print step info without newline */
    if (info->file && info->total > 0)
        printf("  [%s] (%u/%u) %s", info->stage, info->current, info->total, info->file);
    else if (info->file)
        printf("  [%s] %s", info->stage, info->file);
    else
        printf("  [%s]", info->stage);
    fflush(stdout);

    /* record start time for this step */
    clock_gettime(CLOCK_MONOTONIC, &g_step_start);
    g_step_pending = 1;
}

/* ── Timing helpers ──────────────────────────────────────────────────────── */

typedef struct {
    struct timespec wall_start;
    struct rusage   ru_start;
} Timer;

static void timer_start(Timer *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->wall_start);
    getrusage(RUSAGE_SELF, &t->ru_start);
}

/* Returns wall seconds elapsed since timer_start(). */
static double timer_wall(const Timer *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - t->wall_start.tv_sec) +
           (now.tv_nsec - t->wall_start.tv_nsec) * 1e-9;
}

/*
 * Fills *user_sec and *sys_sec with CPU time consumed since timer_start().
 * cpu_cores is the effective parallelism:  total_cpu_time / wall_time.
 * For a single-threaded program this will be ~1.0 (slightly under due to
 * kernel overhead); a parallel encoder would show > 1.0.
 */
static void timer_cpu(const Timer *t, double wall_sec,
                       double *user_sec, double *sys_sec, double *cpu_cores) {
    struct rusage ru_now;
    getrusage(RUSAGE_SELF, &ru_now);

#define TV_DELTA(field) \
    ((ru_now.field.tv_sec  - t->ru_start.field.tv_sec) + \
     (ru_now.field.tv_usec - t->ru_start.field.tv_usec) * 1e-6)

    *user_sec  = TV_DELTA(ru_utime);
    *sys_sec   = TV_DELTA(ru_stime);
    double total_cpu = *user_sec + *sys_sec;
    *cpu_cores = (wall_sec > 1e-6) ? total_cpu / wall_sec : 0.0;
}

/*
 * Peak RSS (high water mark) from /proc/self/status (Linux).
 * VmHWM is the actual peak physical memory used — unlike VmPeak which
 * includes the full virtual address space (inflated by mmap'd files).
 * Falls back to getrusage() ru_maxrss if the proc file isn't available.
 */
static long peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            long kb;
            if (sscanf(line, "VmHWM: %ld kB", &kb) == 1) {
                fclose(f);
                return kb;
            }
        }
        fclose(f);
    }
    /* fallback */
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;   /* already KB on Linux */
}

/* ── Directory size helper ───────────────────────────────────────────────── */

static int64_t dir_bytes(const char *root) {
    /* Iterative walk, same pattern as quine.c */
    #define STACK_MAX 4096
    char **stack = malloc(STACK_MAX * sizeof(char*));
    if (!stack) return -1;
    int top = 0;
    stack[top++] = strdup(root);
    int64_t total = 0;

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
            if (lstat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode) && top < STACK_MAX-1) stack[top++] = full;
                else { if (S_ISREG(st.st_mode)) total += st.st_size; free(full); }
            } else { free(full); }
        }
        closedir(d);
        free(cur);
    }
    free(stack);
    return total;
}

static int64_t file_bytes(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

/* ── Formatting helpers ──────────────────────────────────────────────────── */

static void fmt_bytes(char *buf, size_t bufsz, int64_t b) {
    if      (b >= 1024*1024*1024) snprintf(buf, bufsz, "%.2f GB", b / (1024.0*1024*1024));
    else if (b >= 1024*1024)      snprintf(buf, bufsz, "%.2f MB", b / (1024.0*1024));
    else if (b >= 1024)           snprintf(buf, bufsz, "%.1f KB", b / 1024.0);
    else                          snprintf(buf, bufsz, "%lld B",  (long long)b);
}

static void print_stats(const char *op, double wall, double user, double sys,
                        double cores, long rss_kb) {
    printf("\n");
    printf("  %-20s %s\n", "operation:", op);
    printf("  %-20s %.3f s\n",    "wall time:",  wall);
    printf("  %-20s %.3f s (user)  %.3f s (sys)\n", "cpu time:", user, sys);
    printf("  %-20s %.2f\n",  "cores used:",   cores);
    printf("  %-20s %ld KB\n", "peak RSS:",    rss_kb);
}

/* ── Verification helpers ────────────────────────────────────────────────── */

/* Compare two files byte-for-byte in streaming 256 KB chunks.
 * Returns 1 if identical, 0 otherwise.  Constant memory. */
static int files_equal(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) || stat(b, &sb)) return 0;
    if (sa.st_size != sb.st_size) return 0;

    int fa = open(a, O_RDONLY);
    int fb = open(b, O_RDONLY);
    if (fa < 0 || fb < 0) { if (fa >= 0) close(fa); if (fb >= 0) close(fb); return 0; }

    #define CMP_BUF (256 * 1024)
    uint8_t bufa[CMP_BUF], bufb[CMP_BUF];
    int eq = 1;
    while (eq) {
        ssize_t na = read(fa, bufa, CMP_BUF);
        ssize_t nb = read(fb, bufb, CMP_BUF);
        if (na != nb) { eq = 0; break; }
        if (na == 0) break;  /* both EOF */
        if (memcmp(bufa, bufb, (size_t)na)) eq = 0;
    }
    close(fa); close(fb);
    return eq;
    #undef CMP_BUF
}

/* Recursively compare two directories; return 1 if identical */
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
        if (lstat(pa, &st)) { ok = 0; break; }
        if (S_ISDIR(st.st_mode)) ok = dirs_equal(pa, pb);
        else                     ok = files_equal(pa, pb);
    }
    closedir(da);
    return ok;
}

/*
 * Run decompression with progress, timing, and stats.
 * label: stats label (e.g. "decompress" or "verify decompress")
 * Returns 0 on success, 1 on error (prints error to stderr).
 */
static int run_decompress(const char *dir_a, const char *patch_path,
                           const char *out_dir, const char *label) {
    int64_t sz_patch = file_bytes(patch_path);

    Timer t;
    timer_start(&t);
    progress_timer_reset();

    printf("decompressing...\n");
    int r = quine_decompress(dir_a, patch_path, out_dir);
    progress_finish_line();

    double wall = timer_wall(&t);
    double user, sys, cores;
    timer_cpu(&t, wall, &user, &sys, &cores);
    long rss = peak_rss_kb();

    if (r) {
        fprintf(stderr, "decompress error: %s\n", quine_errmsg());
        return 1;
    }

    int64_t sz_out = dir_bytes(out_dir);

    char bp[32], bo[32];
    fmt_bytes(bp, sizeof(bp), sz_patch);
    fmt_bytes(bo, sizeof(bo), sz_out);

    printf("\n  %-20s %s\n", "patch size:",  bp);
    printf("  %-20s %s\n",   "output size:", bo);
    print_stats(label, wall, user, sys, cores, rss);
    return 0;
}

static void rmtree(const char *path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd); (void)rc;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) goto usage;

    quine_set_progress(cli_progress, NULL);

    /* ── compress ── */
    if (!strcmp(argv[1], "compress") && argc == 5) {
        const char *dir_a      = argv[2];
        const char *dir_b      = argv[3];
        const char *patch_path = argv[4];

        int64_t sz_a = dir_bytes(dir_a);
        int64_t sz_b = dir_bytes(dir_b);

        Timer t;
        timer_start(&t);
        progress_timer_reset();

        printf("compressing...\n");
        int r = quine_compress(dir_a, dir_b, patch_path);
        progress_finish_line();

        double wall = timer_wall(&t);
        double user, sys, cores;
        timer_cpu(&t, wall, &user, &sys, &cores);
        long rss = peak_rss_kb();

        if (r) {
            fprintf(stderr, "compress error: %s\n", quine_errmsg());
            return 1;
        }

        int64_t sz_patch = file_bytes(patch_path);

        char ba[32], bb[32], bp[32];
        fmt_bytes(ba, sizeof(ba), sz_a);
        fmt_bytes(bb, sizeof(bb), sz_b);
        fmt_bytes(bp, sizeof(bp), sz_patch);

        printf("\npatch written: %s\n", patch_path);
        printf("\n");
        printf("  %-20s %s\n",    "dir A size:",   ba);
        printf("  %-20s %s\n",    "dir B size:",   bb);
        printf("  %-20s %s\n",    "patch size:",   bp);
        if (sz_b > 0 && sz_patch > 0) {
            printf("  %-20s %.2fx  (%.0f%% savings)\n",
                   "ratio:",
                   (double)sz_b / sz_patch,
                   100.0 * (1.0 - (double)sz_patch / sz_b));
        }
        print_stats("compress", wall, user, sys, cores, rss);

        /* ── verify: decompress then compare ── */
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/quine_verify_%d", (int)getpid());

        /* step 1: decompress */
        printf("\nverify: ");
        if (run_decompress(dir_a, patch_path, tmpdir, "verify decompress")) {
            rmtree(tmpdir);
            return 1;
        }

        /* step 2: byte-compare */
        printf("\nverify: comparing...\n");
        {
            Timer tc;
            timer_start(&tc);

            int eq = dirs_equal(dir_b, tmpdir);

            double cwall = timer_wall(&tc);
            double cuser, csys, ccores;
            timer_cpu(&tc, cwall, &cuser, &csys, &ccores);
            long crss = peak_rss_kb();
            print_stats("verify compare", cwall, cuser, csys, ccores, crss);

            if (eq) {
                printf("  OK: round-trip verified\n");
            } else {
                fprintf(stderr, "  FAIL: decompressed output differs from dir B\n");
                rmtree(tmpdir);
                return 1;
            }
        }
        rmtree(tmpdir);
        return 0;
    }

    /* ── decompress ── */
    if (!strcmp(argv[1], "decompress") && argc == 5) {
        const char *dir_a      = argv[2];
        const char *patch_path = argv[3];
        const char *out_dir    = argv[4];

        return run_decompress(dir_a, patch_path, out_dir, "decompress");
    }

usage:
    fprintf(stderr,
        "Usage:\n"
        "  quine compress   <dir_a> <dir_b> <output.patch>\n"
        "  quine decompress <dir_a> <input.patch> <out_dir>\n");
    return 1;
}
