/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#pragma once

#include <qoriq-rcw/rcw.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Limits - sized for the largest known .rcwi (ls1028a: 144 entries).
 */
#define RCW_MAX_SYMBOLS     512
#define RCW_MAX_OVERRIDES    64
#define RCW_MAX_INCLUDE_PATHS 32
#define RCW_FIELD_NAME_MAX   64
#define RCW_ERROR_DETAIL_MAX 512

/*
 * Fixed RCW size for pbiformat=2 platforms.
 * The RCW is 1024 bits (32 x 32-bit RCWSR registers, 128 bytes).
 * See LX2160ARM Chapter 4.9.4 stage 13: "stores the 128 bytes of
 * data to the RCWSR registers within the Device Configuration block."
 */
#define RCW_SIZE_BITS  1024
#define RCW_SIZE_BYTES (RCW_SIZE_BITS / 8)

/*
 * CRC-32 parameters for PBI command validation.
 * See LX2160ARM Section 8.2.6 "CRC Checking of PBI Commands":
 *   Polynomial: 1+X^1+X^2+X^4+X^5+X^7+X^8+X^10+X^11+X^12+X^16+
 *               X^22+X^23+X^26+X^32
 *   Initial condition: 0xFFFF_FFFF
 *   Final XOR: 0x0000_0000
 * This is NOT compatible with the standard zlib/binascii crc32.
 */
#define RCW_CRC32_POLY          0x04c11db7
#define RCW_CRC32_INIT          0xffffffff
#define RCW_CRC32_TABLE_SIZE    256
#define RCW_CRC32_BITS_PER_BYTE 8

/*
 * PBL preamble - identifies a valid PBI image.
 * See LX2160ARM Section 8.3.1 "Preamble":
 *   "The preamble is a 4-byte pattern arbitrarily defined as
 *   0xAA55_AA55."  Must be the first record in the PBI image.
 *   The Boot 0 Reset Handler generates an error if the pattern
 *   is not found (error code 0x50 ERROR_PREAMBLE).
 */
#define PBL_PREAMBLE             0xAA55AA55

/*
 * PBI general command format (LX2160ARM Section 8.2.1, Table 27):
 *   Word 0: [31:24]=HDR  [23:16]=CMD  [15:0]=varies
 *
 * All general commands have HDR=0x80 (ACS=1, bit 31 set).
 * The CMD byte identifies the specific operation.
 */

/*
 * Load RCW commands (Section 8.3.5, Table 38):
 *   CMD=0x10: Load RCW with 32-bit checksum validation
 *   CMD=0x11: Load RCW without checksum (padding with zeroes)
 * Checksum = unsigned 32-bit sum of preamble + Load RCW command +
 * 32 RCW words (Section 8.3.5 pseudocode).
 */
#define PBL_CMD_LOAD_RCW         0x80100000
#define PBL_CMD_LOAD_RCW_NOCS    0x80110000

/*
 * Stop command (Section 8.3.16, Table 49):
 *   CMD=0xFF: ends PBI sequence immediately, no CRC check.
 *   Followed by 4 bytes of zero padding.
 */
#define PBL_CMD_STOP             0x80FF0000

/*
 * CRC and Stop command (Section 8.3.15, Table 48):
 *   CMD=0x8F: ends PBI sequence with CRC-32 validation.
 *   "CRC covers all commands from the first command after the RCW
 *   up to and including this CRC and Stop command."
 */
#define PBL_CMD_CRC_STOP         0x808F0000

/*
 * Wait command (Section 8.3.12, Table 45):
 *   CMD=0x82, low 16 bits = iteration count.
 *   "Forces the PBI sequence to wait for an approximate amount of
 *   time counted by iterations through a tight loop."
 */
#define PBL_CMD_WAIT             0x80820000

/*
 * Poll commands (Section 8.3.11, Table 44):
 *   CMD=0x80: Poll Short - timeout after 100,000 accesses (~25 ms)
 *   CMD=0x81: Poll Long  - timeout after 10,000,000 accesses (~2.5 s)
 *   "Repeated reads of a specified address until a specified condition
 *   is found.  A mask is applied to both the read result and condition."
 */
#define PBL_CMD_POLL_SHORT       0x80800000
#define PBL_CMD_POLL_LONG        0x80810000

/*
 * Load Alternate Configuration Window (Section 8.3.6, Table 39):
 *   CMD=0x12, low 14 bits = ALTCFG_WNDW.
 *   Sets up a 64 MB ATU translation window at SP address 0x8000_0000
 *   so that AltConfig Write commands can reach the full 40-bit system
 *   address space.
 */
#define PBL_CMD_LOADACWINDOW     0x80120000

/*
 * Block Copy command (Section 8.3.3, Table 36):
 *   HDR=0x80, CMD=0x00, low 8 bits = SRC interface encoding.
 *   "Used to move larger blocks of data efficiently from non-volatile
 *   memory to on-chip RAM (OCRAM)."
 *   Format: {HDR,CMD,SRC}, SRC_ADDR, DEST_ADDR, BLOCK_SIZE
 */
#define PBL_CMD_BLOCKCOPY        0x80000000

/*
 * General command bit - ACS=1 (bit 31 of the header word).
 * Used by AltConfig Write commands (Section 8.3.2.2, Table 34):
 *   "ACS=1 and B[3-0] != 0"
 * Also the base for all general commands (HDR=0x80).
 */
#define PBL_CMD_GENERAL          0x80000000

/*
 * Flush command - legacy pbiformat=0/1 only.
 * Encoded as a CCSR write to the PBL address space that triggers
 * a read-back of the last write address to force completion.
 * Not used in pbiformat=2 but kept for compatibility.
 */
#define PBL_CMD_FLUSH            0x09000000

/*
 * CCSR Write command header encoding (Section 8.3.2.1, Table 32-33):
 *   Byte 0: [7]=ACS(0), [6]=RSVD(0), [5:4]=B[1:0], [3:0]=ADDR[27:24]
 *   B encodes the byte count: N = 2^(B-1).
 *     B=1 (0b01): 1-byte write  (I2C, UART registers)
 *     B=3 (0b11): 4-byte write  (standard CCSR register write)
 *     B=0,2: reserved/illegal
 *   The B field occupies bits [29:28] of the full 32-bit command word.
 */
#define PBI_WRITE_4BYTE          3  /* B=0b11: 4-byte CCSR write   */
#define PBI_WRITE_1BYTE          1  /* B=0b01: 1-byte CCSR write   */
#define PBI_WRITE_SHIFT          28 /* B field position in word     */

/*
 * AltConfig Write B field (Section 8.3.2.2, Table 34-35):
 *   Byte 0: [7]=ACS(1), [6]=RSVD(0), [5:2]=B[3:0], [1:0]=OFFSET[25:24]
 *   Byte count N = 2^(B-1).  B in range 1-10 (1 to 512 bytes).
 *     B=3: 4 bytes  (1 data word)
 *     B=4: 8 bytes  (2 data words)
 *     B=5: 16 bytes (4 data words)
 *   The B field occupies bits [27:26] of the full 32-bit command word
 *   (after masking ACS at bit 31).
 */
#define PBI_AWRITE_B3            3
#define PBI_AWRITE_B4            4
#define PBI_AWRITE_B5            5
#define PBI_AWRITE_B_SHIFT       26

/*
 * PBI terminator size: Stop or CRC-and-Stop commands are both 2 words
 * (8 bytes): the command word + the CRC value or zero padding.
 * See Tables 48-49 in Section 8.3.15-8.3.16.
 */
#define PBL_TERMINATOR_WORDS     2

/*
 * PBI command word field masks for decoding (decompile).
 * Word 0 layout (general commands): [31:24]=HDR [23:16]=CMD [15:0]=param
 */
#define PBI_HDR_MASK             0xFF000000
#define PBI_HDR_SHIFT            24
#define PBI_CMD_MASK             0x00FF0000
#define PBI_CMD_SHIFT            16

/* CCSR Write: 28-bit address in bits [27:0] (Section 8.3.2.1). */
#define PBI_CCSR_ADDR_MASK       0x0FFFFFFF

/* AltConfig Write: 26-bit offset in bits [25:0] (Section 8.3.2.2). */
#define PBI_AWRITE_ADDR_MASK     0x03FFFFFF

/* AltConfig Write: B field in header byte, bits [5:2] (Section 8.3.2.2). */
#define PBI_AWRITE_B_MASK        0x3C
#define PBI_AWRITE_B_RSHIFT      2

/* CCSR Write: B field in header byte, bits [5:4] (Section 8.3.2.1). */
#define PBI_CCSR_TYPE_MASK       0x30
#define PBI_CCSR_TYPE_SHIFT      4

/*
 * PBI general command numbers - CMD byte (bits [23:16] of word 0).
 * See LX2160ARM Section 8.2.1, Table 27 "PBI Command Summary".
 */
#define PBI_GENCMD_BLOCKCOPY     0x00 /* Section 8.3.3:  Block Copy              */
#define PBI_GENCMD_LOAD_RCW_CS   0x10 /* Section 8.3.5:  Load RCW w/ checksum    */
#define PBI_GENCMD_LOAD_RCW_NOCS 0x11 /* Section 8.3.5:  Load RCW w/o checksum   */
#define PBI_GENCMD_LOADACWINDOW  0x12 /* Section 8.3.6:  Load Alt Cfg Window     */
#define PBI_GENCMD_LOADCONDITION 0x14 /* Section 8.3.7:  Load Condition          */
#define PBI_GENCMD_LOAD_SEC_HDR  0x20 /* Section 8.3.8:  Load Security Header    */
#define PBI_GENCMD_LOAD_CSF_PTR  0x22 /* Section 8.3.9:  Load Boot 1 CSF Hdr Ptr */
#define PBI_GENCMD_POLL_SHORT    0x80 /* Section 8.3.11: Poll Short (100k iters) */
#define PBI_GENCMD_POLL_LONG     0x81 /* Section 8.3.11: Poll Long (10M iters)   */
#define PBI_GENCMD_WAIT          0x82 /* Section 8.3.12: Wait N iterations       */
#define PBI_GENCMD_JUMP          0x84 /* Section 8.3.13: Jump forward            */
#define PBI_GENCMD_JUMP_COND     0x85 /* Section 8.3.14: Conditional Jump        */
#define PBI_GENCMD_CRC_STOP      0x8F /* Section 8.3.15: CRC and Stop            */
#define PBI_GENCMD_STOP          0xFF /* Section 8.3.16: Stop (no CRC)           */

/*
 * CCSR Write B-field values for decoding (Section 8.3.2.1):
 *   B=0b01 -> 1-byte write, B=0b11 -> 4-byte write.
 */
#define PBI_CCSR_WRITE_B1        0x1
#define PBI_CCSR_WRITE_B4        0x3

/*
 * Decompile output: fields wider than this many bits are printed
 * in hexadecimal; narrower fields use decimal.  Matches rcw.py
 * create_source() behavior: "if s > 8: hex else decimal".
 */
#define RCW_DECOMPILE_HEX_THRESHOLD 8

/*
 * Default %pbladdr value.  In rcw.py this is initialized to 0x138000
 * in the global vars dict.  Used by the legacy flush command encoding
 * (pbiformat=0/1 only), where the PBL address space base is masked
 * into the command word.
 */
#define RCW_DEFAULT_PBLADDR      0x138000
#define RCW_PBLADDR_MASK         0x00ffff00

/* PBI buffer initial allocation size (bytes). */
#define RCW_PBI_BUF_INIT_CAP     4096

/* Maximum PBI parameters per command (awrite.b5 has the most: 5). */
#define PBI_MAX_PARAMS           5

/*
 * .uboot subsection encoder constants.
 *
 * The .uboot subsection contains an xxd hex dump of u-boot.bin (8
 * 16-bit words per line, 16 bytes per line). Mirrors rcw.py
 * build_pbi_uboot() (lines 219-272).
 *
 * Lines are grouped:
 *   - Most lines: every 4 lines (64 bytes) -> one block written at
 *     RCW_UBOOT_QUAD_BASE + (line_addr - 0x30).
 *   - Last 4 lines: emitted as 2 pairs (32 bytes each) at
 *     RCW_UBOOT_PAIR_BASE + (line_addr - 0x10).
 *
 * Output is always big-endian regardless of %littleendian.
 *
 * The base addresses below come from rcw.py's struct.pack('>L', ...)
 * with values 0x0C1F80000 and 0x081F80000.  These are 33-bit values
 * truncated by struct.pack to 32-bit (high bit is set, but value
 * fits in uint32).  We use the truncated forms directly.
 */
#define RCW_UBOOT_QUAD_BASE      0x81F80000u /* 64-byte block base */
#define RCW_UBOOT_PAIR_BASE      0xC1F80000u /* 32-byte block base */
#define RCW_UBOOT_LINE_BYTES     16          /* bytes per xxd line  */
#define RCW_UBOOT_WORDS_PER_LINE 8           /* 16-bit words/line   */
#define RCW_UBOOT_QUAD_LINES     4           /* lines per quad block*/
#define RCW_UBOOT_PAIR_LINES     2           /* lines per pair block*/
#define RCW_UBOOT_TAIL_LINES     4           /* last N lines use pairs */
#define RCW_UBOOT_LINE_MAX       256         /* max chars per line  */

/*
 * Bitfield symbol definition (one entry per NAME[begin:end] declaration).
 */
typedef struct {
  char name[RCW_FIELD_NAME_MAX];
  int begin; /* first bit position as declared */
  int end; /* last bit position as declared */
  bool has_value; /* true if an assignment exists */
  uint64_t value; /* assigned value */
} rcw_symbol_t;

/*
 * Ordered symbol table - array preserving insertion order.
 * Linear scan for lookup (few +100s entries, fine without a hash).
 */
typedef struct {
  rcw_symbol_t entries[RCW_MAX_SYMBOLS];
  size_t       count;
} rcw_symtab_t;

/*
 * Parsed %variable values.
 * Typed fields instead of a string map - pbiformat=2 variable set is known.
 */
typedef struct {
  int size; /* %size (shall be 1024) */
  int pbiformat; /* %pbiformat (shall be 2 for our supported cases) */
  bool classicbitnumbers;  /* %classicbitnumbers=1 */
  bool littleendian; /* %littleendian=1 */
  bool nocrc; /* %nocrc=1 */
  bool loadwochecksum; /* %loadwochecksum=1 */
  uint32_t pbladdr; /* %pbladdr (default 0x138000) */
} rcw_vars_t;

/*
 * Dynamic byte buffer for PBI binary output.
 */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} rcw_pbi_buf_t;

/*
 * Full context.
 */
struct rcw_ctx {
  /* Configuration */
  char *include_paths[RCW_MAX_INCLUDE_PATHS];
  size_t include_path_count;
  bool pbl; /* generate PBL preamble/terminator (default true) */
  bool warnings;

  /* Command-line bitfield overrides (-D) */
  struct {
    char name[RCW_FIELD_NAME_MAX];
    uint64_t value;
  } overrides[RCW_MAX_OVERRIDES];
  size_t override_count;

  /* State populated during parse */
  rcw_symtab_t symbols;
  rcw_vars_t vars;
  rcw_pbi_buf_t pbi;

  /* Error detail buffer */
  char error_detail[RCW_ERROR_DETAIL_MAX];
};

/*
 * Internal helpers
 */

/* rcw_crc32.c */
uint32_t rcw_crc32(const uint8_t *data, size_t len);

/* rcw_bits.c */
rcw_error_t rcw_bits_pack(const rcw_symtab_t *syms, const rcw_vars_t *vars, uint8_t out[RCW_SIZE_BYTES]);
uint64_t rcw_bits_extract(const uint8_t data[RCW_SIZE_BYTES], int begin, int end, bool classicbitnumbers);

/* rcw_parse.c */
rcw_error_t rcw_parse(rcw_ctx_t *ctx, const char *source, size_t len);

/* rcw_pbi.c */
rcw_error_t rcw_pbi_encode_line(rcw_ctx_t *ctx, const char *line);
rcw_error_t rcw_eval_expr(const char *expr, uint32_t *result);

/* rcw_uboot.c */
rcw_error_t rcw_uboot_encode(rcw_ctx_t *ctx, const char *body, size_t len);

/* rcw_binary.c */
rcw_error_t rcw_binary_generate(rcw_ctx_t *ctx, uint8_t **out_data, size_t *out_len);

/* rcw_decompile.c */
rcw_error_t rcw_decompile(rcw_ctx_t *ctx, const uint8_t *binary, size_t len, char **out_source, size_t *out_len);

/* rcw_preprocess.c */
rcw_error_t rcw_preprocess(const rcw_ctx_t *ctx, const char *input_path, char **out, size_t *out_len);

/* PBI buffer helpers */
static inline rcw_error_t
rcw_pbi_append(rcw_pbi_buf_t *buf, const void *data, size_t len) {
  if (buf->len + len > buf->cap) {
    size_t newcap = (buf->cap == 0) ? RCW_PBI_BUF_INIT_CAP : buf->cap * 2;

    while (newcap < buf->len + len)
      newcap *= 2;
    uint8_t *p = realloc(buf->data, newcap);

    if (!p)
      return RCW_ERR_NOMEM;

    buf->data = p;
    buf->cap = newcap;
  }

  memcpy(buf->data + buf->len, data, len);
  buf->len += len;

  return RCW_OK;
}

/* Set error detail (printf-style). */
__attribute__((format(printf, 2, 3)))
static inline void
rcw_set_error(rcw_ctx_t *ctx, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->error_detail, RCW_ERROR_DETAIL_MAX, fmt, ap);
  va_end(ap);
}
