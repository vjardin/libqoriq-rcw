---
title: LIBQORIQ-RCW
section: 3
header: Library Functions
footer: libqoriq-rcw 0.1.0
date: 2026-04-12
---

<!-- SPDX-License-Identifier: BSD-3-Clause -->
<!-- Copyright 2026 Free Mobile -- Vincent Jardin -->

# NAME

libqoriq-rcw - QorIQ Reset Configuration Word (RCW) compiler library

# SYNOPSIS

```c
#include <qoriq-rcw/rcw.h>

rcw_ctx_t  *rcw_ctx_new(void);
void        rcw_ctx_free(rcw_ctx_t *ctx);

rcw_error_t rcw_ctx_add_include_path(rcw_ctx_t *ctx, const char *path);
rcw_error_t rcw_ctx_set_pbl(rcw_ctx_t *ctx, int enable);
rcw_error_t rcw_ctx_set_warnings(rcw_ctx_t *ctx, int enable);
rcw_error_t rcw_ctx_set_bitfield(rcw_ctx_t *ctx, const char *name, uint64_t value);

rcw_error_t rcw_compile_file(rcw_ctx_t *ctx, const char *input_path, uint8_t **out_data, size_t *out_len);
rcw_error_t rcw_compile_buffer(rcw_ctx_t *ctx, const char *preprocessed, size_t len, uint8_t **out_data, size_t *out_len);

rcw_error_t rcw_decompile_file(rcw_ctx_t *ctx, const char *bin_path, const char *rcwi_path, char **out_source, size_t *out_len);
rcw_error_t rcw_decompile_buffer(rcw_ctx_t *ctx,
                                 const char *rcwi_preprocessed, size_t rcwi_len,
                                 const uint8_t *binary, size_t binary_len,
                                 const char *rcwi_name,
                                 char **out_source, size_t *out_len);

const char *rcw_strerror(rcw_error_t err);
const char *rcw_ctx_last_error_detail(const rcw_ctx_t *ctx);

void        rcw_free(void *ptr);
```

Link with **-lqoriq-rcw**.

# DESCRIPTION

**libqoriq-rcw** is a C library for compiling NXP QorIQ Reset Configuration
Word source files into PBL/RCW binary images, and for decompiling binary images
back to source text. It supports PBI format 2 platforms: LS1028, LS1088,
LS2088, and LX2160 SoC families.

All library state is held in an opaque context object (**rcw_ctx_t**). There
are no global variables, so multiple contexts can be used concurrently in
separate threads.

## Context Lifecycle

**rcw_ctx_new()**
:   Allocate and initialize a new context. PBL generation is enabled by
    default. Returns **NULL** on allocation failure.

**rcw_ctx_free**(*ctx*)
:   Free a context and all associated resources. *ctx* may be **NULL**.

## Configuration

These functions configure the context before compilation or decompilation.

**rcw_ctx_add_include_path**(*ctx*, *path*)
:   Add *path* to the preprocessor include search list. The current directory
    (**.**) is always included automatically. May be called multiple times (up
    to 32 paths).

**rcw_ctx_set_pbl**(*ctx*, *enable*)
:   Enable (*enable* != 0) or disable (*enable* == 0) generation of the PBL
    preamble, Load RCW command, checksum, and terminator. Default: enabled.
    When disabled, only the raw 128-byte RCW data is produced.

**rcw_ctx_set_warnings**(*ctx*, *enable*)
:   Enable or disable warning messages for duplicate bitfield assignments.
    Default: disabled.

**rcw_ctx_set_bitfield**(*ctx*, *name*, *value*)
:   Register a bitfield override. When compilation runs, the bitfield *name*
    will be set to *value* after source parsing, overriding any assignment in
    the source file. This is equivalent to **qoriq-rcw -D** on the command
    line. May be called multiple times (up to 64 overrides).

## Forward Compilation

**rcw_compile_file**(*ctx*, *input_path*, *out_data*, *out_len*)
:   Compile an RCW source file to a binary image. The file at *input_path* is
    first run through the C preprocessor (**mcpp**), then parsed and
    compiled. On success, *\*out_data* is set to a newly allocated buffer
    containing the binary, and *\*out_len* is set to its size in bytes. The
    caller must free the buffer with **rcw_free()**.

    The context is reset before each compilation; a single context can be
    reused for multiple compilations.

**rcw_compile_buffer**(*ctx*, *preprocessed*, *len*, *out_data*, *out_len*)
:   Like **rcw_compile_file()** but takes already-preprocessed source text
    instead of a file path. No C preprocessor is invoked. This is useful
    for embedding the library where the caller controls its own
    preprocessing pipeline, or for unit testing.

## Reverse Decompilation

**rcw_decompile_file**(*ctx*, *bin_path*, *rcwi_path*, *out_source*, *out_len*)
:   Decompile a binary image back to RCW source text. The bitfield definition
    file *rcwi_path* is preprocessed (via **mcpp**) and parsed to obtain
    symbol names and positions. Then the binary at *bin_path* is decoded:
    field values are extracted and PBI commands are disassembled. The output
    is prefixed with **#include <**basename(*rcwi_path*)**>**, so it can be
    recompiled directly. On success, *\*out_source* is set to a newly
    allocated NUL-terminated string, and *\*out_len* is set to its length
    (excluding the NUL). The caller must free the buffer with **rcw_free()**.

**rcw_decompile_buffer**(*ctx*, *rcwi_preprocessed*, *rcwi_len*, *binary*, *binary_len*, *rcwi_name*, *out_source*, *out_len*)
:   Like **rcw_decompile_file()** but takes already-preprocessed *.rcwi*
    text and an in-memory binary buffer. No preprocessor is invoked and
    no file I/O is performed - useful for embedding the library where
    the caller controls preprocessing, or for unit testing. If
    *rcwi_name* is non-NULL, the output is prefixed with
    **#include <***rcwi_name***>\\n\\n**; pass **NULL** to omit the header.

## Error Handling

All functions that can fail return an **rcw_error_t** value. **RCW_OK** (0)
indicates success; all other values indicate an error.

**rcw_strerror**(*err*)
:   Return a short static string describing the error code.

**rcw_ctx_last_error_detail**(*ctx*)
:   Return a detailed error message from the last failed operation on *ctx*.
    The returned string is valid until the next library call on the same
    context. May be empty if no detail is available.

## Memory Management

**rcw_free**(*ptr*)
:   Free a buffer allocated by the library (*\*out_data* from compilation or
    *\*out_source* from decompilation). *ptr* may be **NULL**.

# SOURCE FORMAT

The text consumed by **rcw_compile_file**() (after preprocessing) and by
**rcw_compile_buffer**() supports two kinds of subsection blocks:

**.pbi** / **.end**
:   Pre-Boot Initialization commands. Each line inside the block is a
    single PBI command (e.g. **write**, **awrite.b4**, **wait**,
    **poll**, **blockcopy**, **loadacwindow**). See **qoriq-rcw**(1)
    for the full command list.

**.uboot** / **.end**
:   Embeds a u-boot binary image into the PBI stream for SPI/SD/NAND
    boot chains. The body is an **xxd**(1) hex dump of *u-boot.bin*
    (8 16-bit words per line, 16 bytes per line); each line has the
    form *ADDRESS*: *WORD WORD WORD WORD WORD WORD WORD WORD*. The
    encoder packs the lines into PBI write blocks at fixed on-chip
    addresses (0x81F80000 base for 64-byte quad blocks; 0xC1F80000 base
    for the trailing 32-byte pair blocks). Output is always
    big-endian regardless of **%littleendian**. No upstream
    pbiformat=2 board uses this subsection; it is provided for parity
    with **rcw.py**.

The body is generated from a u-boot binary by:

```
xxd u-boot.bin | cut -d ' ' -f1-10 > u-boot.xxd
```

then pasted between **.uboot** and **.end** in the source.

# ERROR CODES

**RCW_OK**
:   Success (value 0).

**RCW_ERR_NOMEM**
:   Memory allocation failed.

**RCW_ERR_IO**
:   File I/O error (open, read, or write failure).

**RCW_ERR_PREPROCESS**
:   The C preprocessor (**mcpp**) returned a non-zero exit status.
    Preprocessing errors are printed to standard error.

**RCW_ERR_PARSE**
:   Syntax error in the RCW source file.

**RCW_ERR_OVERLAP**
:   A bitfield definition overlaps with a previously defined field.

**RCW_ERR_UNKNOWN_FIELD**
:   An assignment or **-D** override references a bitfield name that was not
    defined.

**RCW_ERR_VALUE_OVERFLOW**
:   The assigned value is too large for the bitfield width.

**RCW_ERR_PBI_SYNTAX**
:   A PBI command line could not be parsed or has the wrong number of
    parameters.

**RCW_ERR_MISSING_VAR**
:   A required **%variable** (such as **%size** or **%pbiformat**) is missing
    or invalid.

**RCW_ERR_BAD_BINARY**
:   The binary file does not have a valid PBL preamble or is too short.

**RCW_ERR_UNSUPPORTED**
:   The source specifies a feature not supported by this library (e.g.
    **%pbiformat!=2**).

# EXAMPLES

## Compile an RCW file

```c
#include <qoriq-rcw/rcw.h>
#include <stdio.h>

int main(void)
{
    rcw_ctx_t *ctx = rcw_ctx_new();
    if (!ctx)
        return 1;

    uint8_t *binary = NULL;
    size_t len = 0;
    rcw_error_t err;

    err = rcw_compile_file(ctx, "config/rcw_2000.rcw",
                           &binary, &len);
    if (err != RCW_OK) {
        fprintf(stderr, "Error: %s: %s\n",
                rcw_strerror(err),
                rcw_ctx_last_error_detail(ctx));
        rcw_ctx_free(ctx);
        return 1;
    }

    FILE *f = fopen("output.bin", "wb");
    fwrite(binary, 1, len, f);
    fclose(f);

    rcw_free(binary);
    rcw_ctx_free(ctx);
    return 0;
}
```

## Compile from a buffer (no preprocessor)

```c
const char source[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "SYS_PLL_RAT=14\n";

uint8_t *bin = NULL;
size_t len = 0;

rcw_error_t err = rcw_compile_buffer(ctx, source, strlen(source), &bin, &len);
```

## Override a bitfield

```c
rcw_ctx_set_bitfield(ctx, "SYS_PLL_RAT", 10);
rcw_compile_file(ctx, "rcw_2000.rcw", &binary, &len);
```

## Decompile a binary

```c
char *source = NULL;
size_t source_len = 0;

err = rcw_decompile_file(ctx, "rcw.bin", "ls1088rdb.rcwi", &source, &source_len);
if (err == RCW_OK) {
    printf("%s", source);
    rcw_free(source);
}
```

# NOTES

- Only PBI format 2 platforms are supported (**%pbiformat=2**). Older PowerPC
  platforms using **%pbiformat=0** or **%pbiformat=1** are not supported and
  will return **RCW_ERR_UNSUPPORTED**.

- The library uses a custom CRC-32 implementation with polynomial
  **0x04C11DB7** and initial value **0xFFFFFFFF**. This is not compatible with
  the standard zlib **crc32()**.

- **rcw_compile_file()** and **rcw_decompile_file()** use an embedded
  copy of **mcpp**(1) for C preprocessing - no external toolchain is
  required at runtime. The buffer-based variants
  (**rcw_compile_buffer()**, **rcw_decompile_buffer()**) bypass
  preprocessing entirely.

- PBI command parameters support arithmetic expressions (**+ - \* & | << >>**)
  because the C preprocessor expands macros to expressions like
  **(0x1ea0000 + (0x100 \* (0)) + 0x0800)**.

# SEE ALSO

**qoriq-rcw**(1), **mcpp**(1), **pkg-config**(1), **xxd**(1)

# AUTHORS

Vincent Jardin, Free Mobile.
