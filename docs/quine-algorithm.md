# Quine Algorithm

This document describes the CDC-based delta compression algorithm used by
quine, including the parallel indexing strategy and I/O design choices.

## Unified flat address space

All files from A and all files from B are treated as a single logically
contiguous binary stream, sorted lexicographically by relative path:

```
[ A/file1 | A/file2 | ... | B/file1 | B/file2 | ... ]
  0                         sum_A
```

A `REF` opcode is a `(global_offset, length)` pair addressing this flat space.
There is no `file_idx` field — the offset alone identifies the source.  The
decompressor resolves any offset to the correct file and local position via a
small slot table built from the file-size manifest in the patch header.

## Content-defined chunking (CDC)

Each file is split into variable-length chunks (4 KB – 64 KB, averaging ~16 KB)
using a Rabin rolling hash over a 48-byte sliding window.  A chunk boundary is
declared whenever the low 14 bits of the hash are zero.

Because boundaries are determined by the local content of the window — not by
absolute byte position — they are **shift-resistant**: inserting bytes early in
a file does not invalidate chunk boundaries later in the same file or in
subsequent files.  Each chunk is identified by its SHA-256 digest.

## Compression

### Parallel indexing

Files are split into **segments** of approximately `total_bytes / nproc` each
(minimum 1 MB).  Large files get multiple segments; small files are one
segment.  This ensures all CPU cores stay busy even with few large files.

A file indexing uses `read()` with per-segment temporary buffers (no mmap).
B file indexing uses `mmap` with `MADV_SEQUENTIAL` + `MADV_WILLNEED` prefetch.
A and B index concurrently — A threads use heap buffers (freed after
processing), B threads use evictable page cache (kernel-managed).

Each worker thread:
1. Runs CDC + SHA-256 on its segments
2. Inserts `sha256 → global_offset` into a thread-private hash index
3. For B segments, also stores pre-computed chunk boundaries (`PreChunkList`)

After all threads complete, per-thread indexes are merged: A indexes first
(smallest global offsets), then B indexes in thread order.  `idx_insert` keeps
the first entry per SHA, guaranteeing the earliest occurrence is stored.

### Query-time offset filtering

Rather than indexing chunks incrementally during encoding (which forces
single-threaded B processing), all chunks are indexed up front in parallel.
The strictly-before invariant is enforced at **query time**: when encoding a
chunk at global position P, `idx_lookup_before(sha, P)` only returns a match
if the matched entry's data ends before P.

This allows the expensive work (CDC + SHA-256) to be fully parallelized for
both A and B files, while the sequential encode phase becomes a lightweight
loop of hash lookups and opcode writes.

### Sequential encode

For each B file in lexicographic order:
1. Emit a `NEWFILE` opcode
2. Iterate pre-computed chunks, do filtered lookup:
   - **Match found** → flush pending literals, emit `REF(global_offset, len)`
   - **No match** → accumulate into a literal run
3. Flush remaining literals at end of file

Literal runs are tracked as zero-copy pointers into the mmap'd B file data.
On flush, the data goes directly from the mmap to the buffered patch writer —
one memcpy instead of three.

### Intra-file and inter-B deduplication

Because both A and B chunks are indexed before encoding begins, the index
contains all chunk occurrences.  The offset filter ensures correctness:

- **Intra-file**: if a repeated region appears later in the same B file, the
  second occurrence finds the first in the index (offset filter passes since
  the first chunk is earlier).
- **Inter-file**: chunks from earlier B files have smaller global offsets and
  are always available to later B files via the offset filter.

## Decompression

1. Read the patch file via a 4 MB buffered reader (no mmap — see I/O notes).
2. Parse the header.  Open every A file read-only; populate the slot table.
   Pre-register all B file slots with `fd = -1` (not yet written).
3. Read opcodes sequentially:

   | Opcode | Action |
   |--------|--------|
   | `NEWFILE` | Open the next output file. Also open a separate read-only fd on the same inode for self-referential REFs. |
   | `REF` | Binary-search the slot table → `pread()` into copy buffer → `write()` to output. |
   | `LIT` | Stream directly from the patch reader buffer to the output fd. |
   | `END` | Done. |

**Self-referential reads** (a REF that points into the file currently being
written) are safe because the referenced bytes lie strictly before the current
write position.  The read-fd and write-fd are separate file descriptions on
the same inode.

## Memory guarantees

| Phase | Heap allocation |
|-------|----------------|
| Compression | O(total_chunks) for the hash index (~64 MB per million chunks) |
| Decompression | O(\|A files\| + \|B files\|) for the slot table + one `malloc(chunk_max)` = 64 KB copy buffer + 4 MB patch reader buffer |

The decompressor's peak RSS is ~6.5 MB regardless of input size.

## I/O design notes

### mmap vs buffered read

On WSL2 (9P filesystem over NTFS), `mmap` for sequential reading causes
per-page faults through the 9P protocol — each 4 KB page is a separate
round trip.  Buffered `read()` with large chunks (4 MB) batches these into
fewer, larger transfers.

Measured results on 4 GB inputs:

| Approach | Decompress time | Peak RSS |
|----------|----------------|----------|
| mmap patch | 38.5s | 1.8 GB |
| buffered read() | 31.3s | 6.5 MB |

For writing, the same principle applies:
- **mmap for output**: 3.5x slower due to per-page fault overhead on sparse file allocation
- **sendfile()**: slower than userspace buffered I/O on WSL2's 9P layer
- **fwrite with 256 KB buffer**: fastest — batches writes efficiently

The current implementation uses:
- **A indexing**: `read()` with per-segment buffers (freed after use)
- **B indexing**: `mmap` with `MADV_SEQUENTIAL` (page cache is kernel-managed,
  evictable; data stays mapped for the zero-copy encode phase)
- **Patch output**: buffered `fwrite` with 256 KB buffer
- **Patch input (decompress)**: buffered `read()` with 4 MB buffer

On native Linux with ext4/NVMe, mmap may perform equally or better.  The
buffered approach was chosen for portability and predictable RSS.

## Chunk size tuning

The chunk size parameters significantly affect compression ratio.  We
simulated different average chunk sizes on a ~5 GB quantized model weight
dataset (dir A = 4.36 GB, dir B = 5.02 GB):

| Avg chunk | Chunks (A+B) | REF matched | LIT unmatched | Savings |
|-----------|-------------|-------------|---------------|---------|
| 1 KB | 9.7M | 940 MB | 4.10 GB | 17.9% |
| **2 KB** | **4.8M** | **939 MB** | **4.10 GB** | **18.1%** |
| 4 KB | 2.4M | 645 MB | 4.39 GB | 12.5% |
| 8 KB | 1.2M | 645 MB | 4.39 GB | 12.5% |
| 16 KB | 617K | 645 MB | 4.39 GB | 12.5% |

Key findings:

- **2 KB avg is the sweet spot** for this workload — 46% more REF matches than
  16 KB (939 MB vs 645 MB) because partial changes within a 16 KB chunk cause
  the entire chunk to become LIT, while at 2 KB only the ~2 KB around the
  change is missed.
- **Going below 2 KB doesn't help**: at 1 KB, the extra REF opcode overhead
  (13 bytes × 5.6M chunks) eats into the gains from finer matching.
- **4 KB and above plateau**: the REF matched bytes are identical at 4/8/16 KB,
  meaning the matched regions are naturally aligned at ≥4 KB boundaries in this
  data.

However, smaller chunks cause **severe decompression regression**: 2 KB avg
produces ~8x more REF opcodes than 16 KB, each requiring a `pread()` syscall.
Measured on WSL2:

| Avg chunk | Decompress time (5 GB) | Decompress time (1 GB) |
|-----------|----------------------|----------------------|
| 2 KB | 135s | 43s |
| 16 KB | 35s | 8s |

The 4–5x decompression slowdown outweighs the compression ratio improvement,
especially since post-compression zip achieves comparable ratios anyway.

The current defaults are `QN_CHUNK_MIN=4096`, `QN_CHUNK_AVG=16384`,
`QN_CHUNK_MAX=65536`.

## LIT data analysis

We analyzed whether a FIL (fill/repeat pattern) opcode would reduce patch size
by replacing repeated byte patterns in LIT data.  Analysis of 3.91 GB of LIT
data from the model weight workload:

| Pattern type | Bytes | % of LIT |
|-------------|-------|----------|
| All-zero runs (≥ 4 KB) | 4.1 KB | 0.0% |
| Single-byte repeat (≥ 4 KB) | 0 B | 0.0% |
| Short pattern repeat (2-8 B) | 111 KB | 0.003% |
| Unique/incompressible | 3.91 GB | 99.997% |

**Conclusion**: FIL would not help for quantized model weights — the data is
essentially random binary with no exploitable repeated patterns.  For workloads
with significant zero-filled or pattern-filled regions (e.g., sparse matrices,
padded binaries), FIL could be beneficial.

## Comparison with zstd

zstd's `--patch-from` delta mode achieves better compression than quine alone
because it operates at byte-level granularity (vs chunk-level) and applies
entropy coding (Huffman/FSE) to all data including unmatched bytes.  However,
zstd's delta mode has a **2 GB dictionary limit** — it cannot handle reference
directories larger than 2 GB.

For best results, **pipe quine's output through zip/zstd**: quine handles the
CDC-based delta dedup (no size limit), then zip/zstd compresses the remaining
literal bytes.  The `compress-and-verify.sh` script does this automatically
using zip.
