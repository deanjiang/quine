# libquine — C Library API

`libquine` provides CDC-based directory delta compression as a C library.
Add `-Ipath/to/quine/include` to your compiler flags and link against
`build/libquine.a` (static) or `build/libquine.so` (shared).

## API

```c
#include "quine/quine.h"

/* Compress dir_b relative to dir_a; write patch to patch_path.
 * Returns 0 on success, -1 on error. */
int quine_compress(const char *dir_a,
                      const char *dir_b,
                      const char *patch_path);

/* Decompress patch_path using dir_a as reference; restore into out_dir.
 * Returns 0 on success, -1 on error. */
int quine_decompress(const char *dir_a,
                        const char *patch_path,
                        const char *out_dir);

/* Thread-local human-readable error string for the last failed call. */
const char *quine_errmsg(void);
```

## Progress callback

Set a callback before calling `quine_compress` or `quine_decompress` to
receive per-file progress updates.  Library users that don't call
`quine_set_progress` get no output — the default is `NULL`.

```c
/* Optional progress callback — set before calling compress/decompress.
 * Pass NULL to disable (the default).  Stored per-thread. */
typedef struct {
    const char *stage;    /* "scan_a","scan_b","mmap_b","index",
                             "merge_index","write_header","encode_b",
                             "read_header","restore" */
    const char *file;     /* relative path, or NULL for non-file stages */
    uint32_t    current;  /* 1-based index within the stage */
    uint32_t    total;    /* total items in this stage (0 if unknown) */
} quine_progress_t;

typedef void (*quine_progress_fn)(const quine_progress_t *info, void *ctx);
void quine_set_progress(quine_progress_fn fn, void *ctx);
```

Example:

```c
static void my_progress(const quine_progress_t *info, void *ctx) {
    (void)ctx;
    if (info->file)
        printf("[%s] %u/%u %s\n", info->stage, info->current, info->total, info->file);
}

quine_set_progress(my_progress, NULL);
quine_compress("/data/v1.0", "/data/v1.1", "/tmp/update.patch");
```

Stages reported during compression: `scan_a`, `scan_b`, `mmap_b`, `index`,
`merge_index`, `write_header`, `encode_b`.  During decompression:
`read_header`, `restore`.

## Minimal usage

```c
#include "quine/quine.h"
#include <stdio.h>

int main(void) {
    if (quine_compress("/data/v1.0", "/data/v1.1", "/tmp/update.patch")) {
        fprintf(stderr, "%s\n", quine_errmsg());
        return 1;
    }
    if (quine_decompress("/data/v1.0", "/tmp/update.patch", "/data/v1.1_out")) {
        fprintf(stderr, "%s\n", quine_errmsg());
        return 1;
    }
    return 0;
}
```

## Linking

Static library:

```bash
gcc -O2 -D_GNU_SOURCE -Ipath/to/quine/include -o myapp myapp.c -Lpath/to/quine/build -lquine -lssl -lcrypto -lpthread
```

Shared library:

```bash
make build/libquine.so
gcc -O2 -D_GNU_SOURCE -Ipath/to/quine/include -o myapp myapp.c -Lpath/to/quine/build -lquine -lssl -lcrypto -lpthread
LD_LIBRARY_PATH=path/to/quine/build ./myapp
```

## Tuning

Chunk parameters in `include/quine/quine.h`:

| Constant | Default | Effect of increasing |
|---|---|---|
| `QN_CHUNK_MIN` | 4 KB | Fewer tiny chunks; lower index overhead |
| `QN_CHUNK_AVG` | 16 KB | Larger average chunk; fewer REF opcodes but larger miss penalty |
| `QN_CHUNK_MAX` | 64 KB | Larger decompressor copy buffer; fewer forced cuts on repetitive data |
| `QN_RABIN_WINDOW` | 48 B | Wider context for boundary detection; diminishing returns above ~64 |

Rebuild after changing any constant.  Patch files produced with different chunk
parameters are not compatible — the header records the values used at
compression time and the decompressor checks them.
