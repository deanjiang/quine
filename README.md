# quine

Content-defined delta compression between two directories.  Encodes directory
**B** relative to directory **A** by finding matching content chunks across
both directories and emitting a compact patch file.  Decompression is fast and
uses a fixed, minimal amount of memory regardless of input size.

---

## Algorithm

### Unified flat address space

All files from A and all files from B are treated as a single logically
contiguous binary stream, sorted lexicographically by relative path:

```
[ A/file1 | A/file2 | ... | B/file1 | B/file2 | ... ]
  0                         size_of_A
```

A `REF` opcode is a `(global_offset, length)` pair addressing this flat space.
There is no `file_idx` field — the offset alone identifies the source.  The
decompressor resolves any offset to the correct file and local position via a
small slot table built from the file-size manifest stored in the patch header.

### Content-defined chunking (CDC)

Each file is split into variable-length chunks (4–64 KB) using a Rabin rolling
hash over a 48-byte sliding window.  A chunk boundary is declared whenever the
low 14 bits of the hash are zero, giving an average chunk size of ~16 KB.

Because boundaries are determined by the local content of the window — not by
absolute byte position — they are **shift-resistant**: inserting bytes early in
a file does not invalidate chunk boundaries later in the same file or in
subsequent files.  Each chunk is identified by its SHA-256 digest.

### Compression (expensive, offline)

1. Collect the A file list in lexicographic order.  `stat()` each file to get
   its original size.  `mmap` each A file, run CDC, SHA-256 each chunk, and
   insert `sha256 → global_offset` into an open-addressed hash index.

2. Collect the B file list in lexicographic order.  `stat()` each file.
   Write the patch header: magic, chunk parameters, the A file manifest
   (path + original size per file), and the B file manifest (path + original
   size per file).  Both manifests are written before any opcode, so the
   decompressor knows the full flat-space layout before it processes a single
   byte.

3. For each B file in lexicographic order:
   - Emit a `NEWFILE` opcode.
   - `mmap` the file, run CDC, SHA-256 each chunk.
   - **Chunk found in index** → flush any pending literal bytes, emit
     `REF(global_offset, len)`.
   - **Chunk not found** → accumulate into a literal buffer; flush as a `LIT`
     opcode at the next REF or end of file.
   - **Immediately insert this chunk into the index** at `global_base +
     chunk_start`.  Because chunks are processed left to right, each insertion
     sits at an offset strictly less than all future chunks — so later chunks
     in the *same* file can reference earlier chunks in that file, and all
     chunks in subsequent B files can reference any prior chunk.  This is the
     strictly-before invariant, and it is enforced naturally by processing
     order with no extra bookkeeping.
   - Flush any remaining literal bytes at end of file.

This single-pass approach (encode then immediately index each chunk) also
eliminates a redundant re-scan of the file and halves the number of SHA-256
calls compared to a two-pass design.

### Intra-file and inter-B deduplication

Because each chunk is indexed immediately after it is encoded, the index is
always up to date as of the previous chunk.  This means:

- **Intra-file**: if a repeated region appears later in the same B file, the
  second occurrence finds the first in the index and emits a `REF` rather than
  a `LIT`.
- **Inter-file**: completed chunks from earlier B files are already in the
  index when later B files are encoded, so duplicate content across B files is
  deduplicated automatically.

Example: if `B/dup/copy1.bin` and `B/dup/copy2.bin` are identical and neither
exists in A, `copy1.bin`'s chunks are emitted as literals and simultaneously
inserted into the index.  When `copy2.bin` is encoded, every chunk hits the
index and is emitted as a `REF` into the already-written `copy1.bin` region.

### Decompression (fast, minimal memory)

1. `mmap` the patch file read-only.
2. Parse the header.  Open every A file read-only; populate the slot table.
   Pre-register all B file slots with `fd = -1` (not yet written).
3. Read opcodes sequentially:

   | Opcode | Action |
   |--------|--------|
   | `NEWFILE` | Open the next output file `O_CREAT\|O_WRONLY\|O_TRUNC`. Also open a separate read-only fd on the same inode immediately and register it in the slot table, enabling self-referential REFs within this file. |
   | `REF` | Binary-search the slot table to resolve the global offset to `(fd, local_offset)`. `pread()` into the single fixed copy buffer; `write()` to the output file. |
   | `LIT` | `write()` directly from the `mmap`'d patch bytes — zero extra allocation. |
   | `END` | Done. |

**Self-referential reads** (a REF that points into the file currently being
written) are safe because the referenced bytes lie strictly before the current
write position and are already in the OS page cache.  The read-fd and write-fd
are separate file descriptions on the same inode; `pread()` on the read-fd
sees all bytes previously committed by `write()` on the write-fd.

### Memory guarantees

| Phase | Heap allocation |
|-------|----------------|
| Compression | O(total_chunks) for the hash index (~64 MB per million chunks at default settings) |
| Decompression | O(\|A files\| + \|B files\|) for the slot table (16 bytes/file) + **one `malloc(chunk_max)` = 64 KB** copy buffer |

The decompressor never allocates memory proportional to the size of A, B, or
the patch.  The `LIT` path writes directly from the `mmap`'d patch with no
intermediate buffer.

### Wire format

All integers are little-endian.

```
Header:
  magic[4]         \xDDLT
  version          u8       (3)
  chunk_min        u32      default 4096
  chunk_avg        u32      default 16384
  chunk_max        u32      default 65536
  a_count          u16
  [ u16 path_len | path... | u64 file_size ] × a_count   (lex order)
  b_count          u16
  [ u16 path_len | path... | u64 file_size ] × b_count   (lex order)

Opcode stream:
  0x01  REF      u64:global_offset | u32:len
  0x02  LIT      u32:len | <bytes>
  0x03  NEWFILE  u16:path_len | <path>
  0x04  END
```

---

## Prerequisites (Ubuntu 22.04 / 24.04)

```bash
sudo apt update
sudo apt install -y build-essential libssl-dev
```

`build-essential` provides `gcc` and `make`.
`libssl-dev` provides OpenSSL's SHA-256 implementation.

---

## Project layout

```
include/quine/quine.h       public header
src/quine.c                 library implementation
tests/test.c                test suite
main.c                      CLI driver
scripts/
  verify-roundtrip.sh       compress + decompress + compare
  benchmark.sh              benchmark quine vs zstd
Makefile
build/                      all generated artifacts (gitignored)
```

## Build

```bash
git clone <repo>
cd quine
make            # builds everything into build/
```

All artifacts are placed in `build/`:

```bash
make                          # build/libquine.a, build/quine, build/test_quine
make build/libquine.a         # static library only
make build/libquine.so        # shared library
make build/quine              # CLI binary (links libquine.a)
make build/test_quine         # test binary
make test                     # build and run tests
```

Debug build:

```bash
make CFLAGS="-g -O0 -Wall -Wextra -std=c11 -D_GNU_SOURCE"
```

To use a different SHA-256 provider (e.g. BoringSSL or a vendored
implementation), replace `-lssl -lcrypto` in `LDFLAGS` and swap
`#include <openssl/sha.h>` + `SHA256()` in `src/quine.c` for your provider's
equivalent.

---

## CLI usage

### Compress

```bash
build/quine compress <dir_a> <dir_b> <output.patch>
```

### Decompress

```bash
build/quine decompress [--verify-max-mem=SIZE] <dir_a> <input.patch> <out_dir>
```

`out_dir` is created if it does not exist.  The optional `--verify-max-mem`
flag causes the command to fail if peak RSS exceeds the given limit (e.g.
`10M`, `512K`, `1G`).  This is useful in CI to enforce the decompressor's
memory guarantees.

### Compare

```bash
build/quine compare <dir_a> <dir_b>
```

Byte-compares two directories recursively.  Exits 0 if identical, 1 if they
differ.  Uses streaming 256 KB chunks — constant memory regardless of file
sizes.

### Scripts

**Verify round-trip** — compress, decompress, and compare in sequence:

```bash
./scripts/verify-roundtrip.sh <dir_a> <dir_b> [max_mem]
```

- `max_mem` defaults to `10M` — the peak RSS limit for decompression
- Compresses dir_b relative to dir_a
- Decompresses the patch (with `--verify-max-mem`)
- Byte-compares the restored output against dir_b
- Cleans up temporary files on exit
- Exits non-zero if any step fails

**Benchmark** — compare quine against zstd with timing, RSS, and compression
ratio:

```bash
./scripts/benchmark.sh <dir_a> <dir_b>
```

Runs quine compress/decompress, zstd standalone (`tar | zstd`), and zstd
delta (`zstd --patch-from`), then prints a summary table.  Requires `zstd`
and GNU `time` (`/usr/bin/time -v`).

---

## Library API

Add `-Ipath/to/quine/include` to your compiler flags and link against
`build/libquine.a` (or `build/libquine.so`).

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

/* Optional progress callback — set before calling compress/decompress.
 * Pass NULL to disable (the default).  Stored per-thread. */
typedef struct {
    const char *stage;    /* "scan_a","index_a","scan_b","encode_b",
                             "write_header","read_header","restore" */
    const char *file;     /* relative path, or NULL for non-file stages */
    uint32_t    current;  /* 1-based index within the stage */
    uint32_t    total;    /* total items in this stage (0 if unknown) */
} quine_progress_t;

typedef void (*quine_progress_fn)(const quine_progress_t *info, void *ctx);
void quine_set_progress(quine_progress_fn fn, void *ctx);
```

### Progress callback

Set a callback before calling `quine_compress` or `quine_decompress` to
receive per-file progress updates.  Library users that don't call
`quine_set_progress` get no output — the default is `NULL`.

```c
static void my_progress(const quine_progress_t *info, void *ctx) {
    (void)ctx;
    if (info->file)
        printf("[%s] %u/%u %s\n", info->stage, info->current, info->total, info->file);
}

quine_set_progress(my_progress, NULL);
quine_compress("/data/v1.0", "/data/v1.1", "/tmp/update.patch");
```

Stages reported during compression: `scan_a`, `index_a`, `scan_b`,
`write_header`, `encode_b`.  During decompression: `read_header`, `restore`.

Minimal usage:

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

Linking against the static library:

```bash
gcc -O2 -D_GNU_SOURCE -Ipath/to/quine/include -o myapp myapp.c -Lpath/to/quine/build -lquine -lssl -lcrypto
```

Or against the shared library:

```bash
make build/libquine.so
gcc -O2 -D_GNU_SOURCE -Ipath/to/quine/include -o myapp myapp.c -Lpath/to/quine/build -lquine -lssl -lcrypto
LD_LIBRARY_PATH=path/to/quine/build ./myapp
```

---

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

---

## Limitations

- **Max 65 535 files each in A and B** (`u16` count fields).  Increase to
  `u32` in the wire format for larger trees.
- **Max 65 535-byte relative path** per file (`u16` path-length field).
  Paths longer than 4095 bytes are also rejected by the decompressor's
  internal `abs[4096]` buffer; increase that buffer if needed.
- **Single-threaded compression**.  The A indexing phase is embarrassingly
  parallel; sharding files across worker threads and merging per-shard hash
  tables is a straightforward improvement.
- **No literal compression**.  Bytes in B with no match in A or prior B files
  are stored verbatim.  Piping the patch through `zstd` reduces these further.
- **No integrity check on the patch**.  Add a trailing HMAC or SHA-256 of the
  opcode stream if tamper-resistance is required.
- **Linux/POSIX only**.  Uses `mmap`, `pread`, `getrusage`,
  `/proc/self/status`.  Porting to macOS requires noting that `ru_maxrss` is
  in bytes on macOS (not KB), and removing the `/proc/self/status` read.
