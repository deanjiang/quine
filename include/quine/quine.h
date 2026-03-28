#pragma once
/*
 * quine.h — CDC-based directory delta compression
 *
 * ── Algorithm summary ────────────────────────────────────────────────────────
 *
 * FLAT ADDRESS SPACE
 *   All files from A, then all files from B, are treated as one logically
 *   contiguous binary stream, ordered lexicographically by relative path:
 *
 *     [ A/file1 | A/file2 | ... | B/file1 | B/file2 | ... ]
 *       0                        size_of_A
 *
 *   A REF opcode is simply (offset, len) into this flat space — no file_idx.
 *   The decompressor resolves any offset to the correct fd + local offset via
 *   a small slot table built from the file-size manifest in the patch header.
 *
 * CONTENT-DEFINED CHUNKING (CDC)
 *   Files are split into variable-length chunks (4–64 KB) using a Rabin
 *   rolling hash over a 48-byte window.  Boundaries are content-defined, so
 *   they are shift-resistant: inserting bytes in one file does not invalidate
 *   chunks in subsequent regions.  Each chunk is identified by its SHA-256.
 *
 * COMPRESSION (expensive, offline)
 *   1. Collect A file list in lex order; stat each file; build flat-A offset
 *      table.  mmap each A file; CDC-chunk; SHA-256 each chunk → insert into
 *      hash index as  sha256 → global_offset.
 *   2. Collect B file list in lex order; stat each file (original sizes known).
 *      Write patch header: magic + chunk params + A manifest + B manifest.
 *   3. For each B file (in lexicographic order):
 *        mmap the file; CDC-chunk it; for each chunk:
 *          • SHA-256 lookup in index → found    : flush pending literals,
 *                                                 emit REF(global_offset, len)
 *                                    → not found: accumulate LIT bytes
 *          • Immediately insert this chunk into the index at its global offset.
 *            Because chunks are processed left to right, each insertion is at
 *            an offset strictly less than all future chunks — so subsequent
 *            chunks in the same file, and all chunks in later B files, can
 *            reference it.  This is the strictly-before invariant.
 *        Flush any remaining LIT bytes at end of file.
 *
 * DECOMPRESSION (fast, minimal memory)
 *   1. mmap the patch file read-only.
 *   2. Read header → open every A file read-only; build slot table.
 *   3. Read opcodes sequentially:
 *        NEWFILE → open next output file (O_CREAT|O_WRONLY|O_TRUNC).
 *                  When the previous file is complete, open a read-fd on it
 *                  and register it in the slot table so later REFs can reach
 *                  it.  The current output file is also registered immediately
 *                  (same inode, separate fd) to allow self-referential REFs.
 *        REF     → resolve global offset via slot table → pread into the one
 *                  fixed copy buffer → write() to output.
 *        LIT     → write() directly from the mmap'd patch (zero extra alloc).
 *        END     → done.
 *
 * MEMORY GUARANTEES
 *   Compression  : O(total_chunks) for the hash index
 *   Decompression: O(|A_files| + |B_files|) for the slot table (tiny)
 *                  + ONE malloc(chunk_max) copy buffer — nothing else.
 *
 * I/O PATTERN (decompression)
 *   Output files: O_CREAT|O_WRONLY|O_TRUNC, sequential write(), no seeking.
 *   Source reads : pread() at arbitrary offsets into A files and already-
 *                  completed (or in-progress) B files.  Self-referential
 *                  pread() is safe: the referenced bytes are strictly before
 *                  the current write position and already in the page cache.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <stdint.h>
#include <stddef.h>

/* ── Tuning ──────────────────────────────────────────────────────────────── */

#define QN_CHUNK_MIN    (4  * 1024)   /* 4 KB  — minimum chunk size           */
#define QN_CHUNK_AVG    (16 * 1024)   /* 16 KB — average (boundary mask = 14b)*/
#define QN_CHUNK_MAX    (64 * 1024)   /* 64 KB — forced cut; decompressor buf */
#define QN_RABIN_WINDOW 48            /* rolling hash window in bytes          */

/* ── Wire format ─────────────────────────────────────────────────────────── */

#define QN_MAGIC    "\xDDLT"
#define QN_VERSION  3           /* v3: path_len is u16 (was u8 in v2)         */

/*
 * Header (all integers little-endian):
 *   magic[4]    \xDDLT
 *   version     u8
 *   chunk_min   u32
 *   chunk_avg   u32
 *   chunk_max   u32
 *   a_count     u16
 *   [u16 path_len | path... | u64 file_size] × a_count   (lex order)
 *   b_count     u16
 *   [u16 path_len | path... | u64 file_size] × b_count   (lex order)
 *
 * Opcode stream (follows header):
 *   0x01  REF      u64:global_offset | u32:len
 *   0x02  LIT      u32:len | <bytes>
 *   0x03  NEWFILE  u16:path_len | <path>
 *   0x04  END
 *
 * global_offset addresses the unified flat space:
 *   [0,            sum_A)          → A files in lex order
 *   [sum_A,        sum_A + sum_B)  → B files in lex order (as written)
 * The referenced region must end strictly before the current write position.
 */

#define QN_OP_REF     0x01
#define QN_OP_LIT     0x02
#define QN_OP_NEWFILE 0x03
#define QN_OP_END     0x04

/* ── Progress callback ───────────────────────────────────────────────────── */

/*
 * Optional progress reporting.  Set a callback before calling compress or
 * decompress; the library invokes it at each significant step.  Pass NULL
 * to disable (the default).  The callback is stored per-thread.
 *
 * stage:   "scan_a", "scan_b", "mmap_b", "index", "merge_index",
 *          "write_header", "encode_b", "read_header", "restore"
 * file:    relative path of the current file, or NULL for non-file stages
 * current: 1-based index of the current item within the stage
 * total:   total items in this stage (0 if unknown)
 */
typedef struct {
    const char *stage;
    const char *file;
    uint32_t    current;
    uint32_t    total;
} quine_progress_t;

typedef void (*quine_progress_fn)(const quine_progress_t *info, void *ctx);

void quine_set_progress(quine_progress_fn fn, void *ctx);

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * Compress directory B relative to directory A.
 * Returns 0 on success, -1 on error (quine_errmsg() for details).
 */
int quine_compress(const char *dir_a,
                      const char *dir_b,
                      const char *patch_path);

/*
 * Decompress patch_path using dir_a as reference; write restored B to out_dir.
 * Returns 0 on success, -1 on error.
 */
int quine_decompress(const char *dir_a,
                        const char *patch_path,
                        const char *out_dir);

/* Thread-local human-readable error string for the last failed call. */
const char *quine_errmsg(void);
