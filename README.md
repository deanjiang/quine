# quine

Named after the concept of self-reproducing programs, **quine** is a
content-defined delta compression tool for directories.  It encodes directory
**B** relative to directory **A** by finding matching content chunks across
both trees and emitting a compact patch file.  Decompression is fast, uses a
fixed ~6.5 MB of memory regardless of input size, and produces a byte-identical
copy of B.

Quine is available as a **C library** (`libquine`) and a **CLI tool**.

---

## Performance

Benchmarked on ~1 GB model weight directories (8 files, WSL2 on NVMe):

| | quine + zip | zstd --patch-from |
|---|---|---|
| **Compress time** | 10.1s | 1.7s |
| **Decompress time** | 6.5s | 1.1s |
| **Output size** | 175 MB (5.40x) | 173 MB (5.47x) |
| **Decompress memory** | **6.4 MB** | 1.52 GB |
| **Max input size** | **unlimited** | 2 GB |

**Comparable compression.** Combined with zip, quine achieves compression
ratios within 1% of zstd's byte-level delta mode (5.40x vs 5.47x).  Quine
handles the structural dedup via CDC; zip compresses the remaining literal
bytes.

**Minimal decompression memory.** The decompressor uses a fixed ~6.5 MB
regardless of input size — a 4 MB read buffer, a 64 KB copy buffer, and a
small slot table.  No data proportional to A, B, or the patch is ever
allocated.  This is **238x less memory** than zstd's delta decompressor on the
same workload.

**No size limits.** zstd's `--patch-from` loads the entire reference into
memory, limiting it to ~2 GB.  Quine has no such constraint — it streams A
files via `pread()` and reads the patch through a buffered reader.  Tested on
directories up to 5 GB with 6.5 MB decompression RSS.

**Parallel compression.** Both A and B files are indexed in parallel across all
CPU cores using segment-based work distribution (large files are split across
threads).  On a 16-core machine, compression achieves 1.8x core utilization
on I/O-bound workloads.

---

## Algorithm

Quine uses content-defined chunking (CDC) with a Rabin rolling hash to split
files into variable-length chunks, SHA-256 to identify them, and a parallel
segment-based indexing strategy to saturate all CPU cores.

See [docs/quine-algorithm.md](docs/quine-algorithm.md) for the full algorithm
description, including parallel indexing, query-time offset filtering,
memory guarantees, and I/O design notes (mmap vs buffered read tradeoffs).

### Wire format

See [docs/patch-format-v3.md](docs/patch-format-v3.md) for the binary patch format
specification.  The version number is stored in the patch header; any format
change requires a version bump.

---

## Prerequisites (Ubuntu 22.04 / 24.04)

```bash
sudo apt update
sudo apt install -y build-essential libssl-dev
```

`build-essential` provides `gcc` and `make`.
`libssl-dev` provides OpenSSL's SHA-256 implementation.

---


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

---

## CLI usage

### Compress

```bash
build/quine compress <dir_a> <dir_b> <output.qn>
```

### Decompress

```bash
build/quine decompress [--verify-max-mem=SIZE] <dir_a> <input.qn> <out_dir>
```

`out_dir` is created if it does not exist.  The optional `--verify-max-mem`
flag causes the command to fail if peak RSS exceeds the given limit (e.g.
`10M`, `512K`, `1G`).  This is useful in CI to enforce the decompressor's
memory guarantees.

### Scripts

**Compress and verify** — compress, zip, unzip, decompress, compare:

```bash
./scripts/compress-and-verify.sh <dir_a> <dir_b> [patch_file] [max_mem]
```

- `patch_file` — output name (`.qn` extension added if missing); both
  `patch_file.qn` and `patch_file.qn.zip` are kept on success, removed on
  failure.  If omitted, uses temp files for test-only verification.
- `max_mem` defaults to `10M` — the peak RSS limit for decompression
- Steps: compress → zip → unzip → decompress (from zip) → `diff -rq`
- Verification starts from the zipped file to prove the full round-trip
- Exits non-zero if any step fails

**Benchmark** — compare quine+zip against zstd delta:

```bash
./scripts/benchmark.sh <dir_a> <dir_b>
```

Runs quine compress, zip, unzip + decompress, and zstd delta
compress/decompress, then prints a summary table.  Requires `zip`,
`unzip`, `zstd`, and GNU `time` (`/usr/bin/time -v`).

---

## Library API

See [docs/libquine.md](docs/libquine.md) for the full API documentation, progress
callback usage, code examples, and linking instructions.

---

## Limitations

- **Max 65 535 files each in A and B** (`u16` count fields).  Increase to
  `u32` in the wire format for larger trees.
- **Max 65 535-byte relative path** per file (`u16` path-length field).
  Paths longer than 4095 bytes are also rejected by the decompressor's
  internal `abs[4096]` buffer; increase that buffer if needed.
- **Single-threaded encode phase**.  A and B indexing are now parallelized
  across all CPU cores, but the final encode phase (which emits REF/LIT
  opcodes) remains single-threaded.
- **No literal compression**.  Bytes in B with no match in A or prior B files
  are stored verbatim.  Piping the patch through `zstd` reduces these further.
- **No integrity check on the patch**.  Add a trailing HMAC or SHA-256 of the
  opcode stream if tamper-resistance is required.
- **Linux/POSIX only**.  Uses `mmap`, `pread`, `getrusage`,
  `/proc/self/status`.  Porting to macOS requires noting that `ru_maxrss` is
  in bytes on macOS (not KB), and removing the `/proc/self/status` read.
