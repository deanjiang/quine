# quine

Named after the concept of self-reproducing programs, **quine** is a
content-defined delta compression tool for directories.  It encodes directory
**B** relative to directory **A** by finding matching content chunks across
both trees and emitting a compact patch file.  Decompression is fast, uses a
fixed ~6.5 MB of memory regardless of input size, and produces a byte-identical
copy of B.

Quine is available as a **C library** (`libquine`) and a **CLI tool**.

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

**Compress and verify** — compress, decompress, compare, keep patch on success:

```bash
./scripts/compress-and-verify.sh <dir_a> <dir_b> [patch_file] [max_mem]
```

- `patch_file` — output patch file path (kept on success, removed on failure);
  if omitted, uses a temp file that is cleaned up after verification
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

Runs quine compress/decompress and zstd delta compress/decompress
(`zstd --patch-from`), then prints a summary table.  Requires `zstd`
and GNU `time` (`/usr/bin/time -v`).

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
