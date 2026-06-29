# libqoriq-rcw

[![CI](https://github.com/vjardin/libqoriq-rcw/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/vjardin/libqoriq-rcw/actions/workflows/ci.yml)

C library for compiling and decompiling NXP QorIQ Reset Configuration Word (RCW) files.

## What is the RCW?

On NXP QorIQ and Layerscape SoCs, the Reset Configuration Word (RCW) is a 1024-bit block that
controls the earliest stage of hardware initialization. At power-on reset, the SoC's on-chip
Service Processor (TBC a dedicated Cortex-A5 core with its own 64 KB boot ROM) is released
from reset and begins executing its Boot 0 Reset Handler from ROM. The Service Processor
samples the `CFG_RCW_SRC` configuration pins to determine the boot interface (FlexSPI NOR, SD
card, eMMC, or I2C), then loads 128 bytes of RCW data from that interface into the RCWSR
registers of the Device Configuration block. The PLLs lock according to the clock ratios
encoded in the RCW, and optional Pre-Boot Initialization (PBI) commands are executed to
write CCSR registers or copy data into on-chip SRAM (OCRAM). Only then are the application
cores released from reset and allowed to boot.

The RCW therefore defines PLL ratios, SerDes protocol selection, boot source and location,
pin muxing, and errata workarounds. The binary image produced by this library is what gets
flashed onto the board's boot medium and consumed by the Service Processor's boot ROM at
every power cycle.

Supported platforms: LS1028, LS1088, LS2088, and LX2160 families (PBI format 2).

## Features

- Forward compilation: `.rcw` source to `.bin` binary image
- Reverse decompilation: `.bin` binary back to `.rcw` source text
- Stable public C API with opaque context (no global state)
- Built as both shared (`.so`) and static (`.a`) libraries
- pkg-config support for downstream integration
- Bitfield override API (equivalent to command-line `-D` overrides)
- Buffer-based compilation API (`rcw_compile_buffer()` /
  `rcw_decompile_buffer()`) for embedding already-preprocessed source
- No runtime dependency on an external C compiler: preprocessing is
  handled in-process by [mcpp](http://mcpp.sourceforge.net/)

## Dependencies

- C17 compiler (gcc or clang)
- Meson, Ninja
- [mcpp](http://mcpp.sourceforge.net/) (package: `libmcpp-dev`) - used
  in-process as the C preprocessor for `.rcw` / `.rcwi` files
- cmocka (package: `libcmocka-dev`) - for the unit tests only
- pandoc - for generating the man page

## Build

From the repository root (`rcw/`):

```bash
meson setup build
meson compile -C build
```

## Test

```bash
meson test -C build
```

This runs the cmocka unit test suites:

| Suite | What it tests |
|-------|---------------|
| `test_crc32` | Custom CRC-32 implementation |
| `test_bits` | Bitfield packing and extraction |
| `test_parse` | Source file parser |
| `test_pbi` | PBI command encoder and expression evaluator |
| `test_binary` | End-to-end binary generation |
| `test_decompile` | Decompilation and round-trip verification |

## Install

```bash
meson install -C build
```

This installs:
- `libqoriq-rcw.so` / `libqoriq-rcw.a` to `$libdir`
- `qoriq-rcw/rcw.h` to `$includedir/qoriq-rcw/`
- `libqoriq-rcw.pc` pkg-config file

## Releasing

The version number shall be bumped for every release:

| File                      | What to change                                |
|---------------------------|-----------------------------------------------|
| `meson.build`             | the `version :` field of the `project()` call |
| `include/qoriq-rcw/rcw.h` | `QORIQ_RCW_VERSION_MAJOR` / `_MINOR` / `_PATCH` |
| `debian/changelog`        | a new top entry `libqoriq-rcw (X.Y.Z-1) ...`  |

Add the changelog entry with the text timestamp:

```bash
date -R # e.g. Mon, 29 Jun 2026 15:14:51 +0200
```

Commit the three files, create an annotated tag, and push both:

```bash
git add meson.build include/qoriq-rcw/rcw.h debian/changelog
git commit -m "Release X.Y.Z"
git tag -a vX.Y.Z -m "libqoriq-rcw X.Y.Z"
git push origin main
git push origin vX.Y.Z
```

Then, GitHub shall auto-generate the source archives
(`.tar.gz` / `.zip`) for the tag; if needed use `gh release create`:

```bash
gh release create vX.Y.Z --title "vX.Y.Z" --notes "..."
```

The tarball is then available at:

```
https://github.com/vjardin/libqoriq-rcw/archive/refs/tags/vX.Y.Z.tar.gz
```

For example: the 1.0.1 release was commit
[`aa075dd`](https://github.com/vjardin/libqoriq-rcw/commit/aa075dd8fdb1b255fad40fb0f5692d4e2fee3cab)
("Release 1.0.1"), tagged `v1.0.1`
([release page](https://github.com/vjardin/libqoriq-rcw/releases/tag/v1.0.1)).

## Usage

```c
#include <qoriq-rcw/rcw.h>

rcw_ctx_t *ctx = rcw_ctx_new();

/* Compile from file (runs gcc -E internally) */
uint8_t *binary = NULL;
size_t len = 0;
rcw_error_t err = rcw_compile_file(ctx, "rcw_2000.rcw", &binary, &len);

/* Or compile from a pre-processed buffer */
err = rcw_compile_buffer(ctx, source_text, source_len, &binary, &len);

/* Override bitfields before compilation */
rcw_ctx_set_bitfield(ctx, "SYS_PLL_RAT", 10);

/* Decompile a binary */
char *source = NULL;
size_t source_len = 0;
err = rcw_decompile_file(ctx, "rcw.bin", "lx2160a.rcwi", &source, &source_len);

/* Clean up */
rcw_free(binary);
rcw_free(source);
rcw_ctx_free(ctx);
```

Link with `-lqoriq-rcw`, or use pkg-config:

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs libqoriq-rcw)
```

See `libqoriq-rcw(3)` for the full API reference.
