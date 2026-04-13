# libqoriq-rcw

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
- Buffer-based compilation API for embedding without a `gcc` dependency

## Dependencies

- C17 compiler (gcc or clang)
- Meson
- cmocka (for tests only)
- gcc in `PATH` at runtime - used as the C preprocessor for `.rcw` files
  by `rcw_compile_file()` and `rcw_decompile_file()`. The buffer-based
  variants (`rcw_compile_buffer()`, `rcw_decompile_buffer()`) take
  already-preprocessed text and have no runtime dependency on gcc.
  - TODO: drop the dependency on gcc

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
