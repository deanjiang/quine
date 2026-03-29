# Quine Patch Format — Version 3

This document specifies the binary format of patch files produced by quine v3.
The version number is stored in the patch header; any change to the format
requires a version bump.

## Overview

A quine patch encodes the differences between two directory trees (A and B)
using content-defined chunking (CDC).  The patch contains a header with file
manifests, followed by an opcode stream that reconstructs B given A.

## Flat address space

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

## Byte order

All integers are **little-endian**.

## Header

| Field | Type | Description |
|-------|------|-------------|
| magic | `byte[4]` | `\xDD` `L` `T` `\0` (hex: `DD 4C 54 00`) |
| version | `u8` | Format version (currently `3`) |
| chunk_min | `u32` | Minimum chunk size in bytes (default 4096) |
| chunk_avg | `u32` | Average chunk size in bytes (default 16384) |
| chunk_max | `u32` | Maximum chunk size in bytes (default 65536) |
| a_count | `u16` | Number of files in directory A |
| A manifest | (variable) | `a_count` entries, each: `u16 path_len \| path[path_len] \| u64 file_size` |
| b_count | `u16` | Number of files in directory B |
| B manifest | (variable) | `b_count` entries, each: `u16 path_len \| path[path_len] \| u64 file_size` |

Manifests are sorted in **lexicographic order** by relative path.  Both
manifests appear before any opcode, so the decompressor knows the full
flat-space layout before processing begins.

## Opcode stream

The opcode stream follows the header immediately and consists of a sequence
of opcodes terminated by `END`.

| Opcode | Value | Payload | Description |
|--------|-------|---------|-------------|
| REF | `0x01` | `u64 global_offset \| u32 len` | Copy `len` bytes from `global_offset` in the flat space |
| LIT | `0x02` | `u32 len \| byte[len]` | Write `len` literal bytes verbatim |
| NEWFILE | `0x03` | `u16 path_len \| byte[path_len]` | Begin writing a new output file |
| END | `0x04` | (none) | End of opcode stream |

### Constraints

- `REF` global_offset + len must refer to data that has already been written
  (the **strictly-before invariant**).  This includes all A files and any B
  file bytes written before the current position.
- `LIT` len is capped at 32 MB (`33554432` bytes) per opcode.  Larger literal
  runs are split into multiple `LIT` opcodes.
- `NEWFILE` opcodes appear in the same lexicographic order as the B manifest.
- The opcode stream must end with exactly one `END` opcode.

## Global offset resolution

The decompressor builds a **slot table** from the A and B manifests:

| Slot range | Source |
|------------|--------|
| `[0, sum_A)` | A files in lex order |
| `[sum_A, sum_A + sum_B)` | B files in lex order (as written) |

Given a `global_offset`, binary search the slot table to find the file and
local offset within that file.

## Self-referential REFs

A `REF` may point into the file currently being written.  This is safe because
the referenced bytes lie strictly before the current write position and are
already committed to disk.  The decompressor opens a separate read-only fd on
the output file immediately upon `NEWFILE`, allowing `pread()` without
affecting the write position.

## Version history

| Version | Changes |
|---------|---------|
| 2 | Initial format. `path_len` was `u8` (max 255 bytes). |
| 3 | `path_len` changed to `u16` (max 65535 bytes). |
