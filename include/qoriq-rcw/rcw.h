/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * QorIQ Reset Configuration Word (RCW) compiler library.
 * Compiles .rcw source files into PBL/RCW binary images and
 * decompiles binaries back to source (pbiformat=2 platforms).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QORIQ_RCW_VERSION_MAJOR 1
#define QORIQ_RCW_VERSION_MINOR 2
#define QORIQ_RCW_VERSION_PATCH 0

/* Error codes */
typedef enum {
  RCW_OK = 0,
  RCW_ERR_NOMEM,
  RCW_ERR_IO,
  RCW_ERR_PREPROCESS,
  RCW_ERR_PARSE,
  RCW_ERR_OVERLAP,
  RCW_ERR_UNKNOWN_FIELD,
  RCW_ERR_VALUE_OVERFLOW,
  RCW_ERR_PBI_SYNTAX,
  RCW_ERR_MISSING_VAR,
  RCW_ERR_BAD_BINARY,
  RCW_ERR_UNSUPPORTED,
  RCW_ERR_TRUNCATED,
  RCW_ERR_PBI_LENGTH,
} rcw_error_t;

/* Opaque context - all state lives here, no globals. */
typedef struct rcw_ctx rcw_ctx_t;

/*
 * Lifecycle
 */
rcw_ctx_t *rcw_ctx_new(void);
void rcw_ctx_free(rcw_ctx_t *ctx);

/*
 * Configuration (call before compilation)
 */
rcw_error_t rcw_ctx_add_include_path(rcw_ctx_t *ctx, const char *path);

rcw_error_t rcw_ctx_add_define(rcw_ctx_t *ctx, const char *name, const char *value);
rcw_error_t rcw_ctx_set_pbl(rcw_ctx_t *ctx, int enable);
rcw_error_t rcw_ctx_set_warnings(rcw_ctx_t *ctx, int enable);

/* Override a bitfield value (equivalent to rcw.py -D FIELD=VALUE).
 * Can be called multiple times. Overrides apply after source parsing. */
rcw_error_t rcw_ctx_set_bitfield(rcw_ctx_t *ctx, const char *name, uint64_t value);

/*
 * Forward compilation: .rcw -> .bin
 *
 * rcw_compile_file() runs the embedded C preprocessor (mcpp) on
 * input_path, then parses and compiles the result.
 *
 * rcw_compile_buffer() takes already-preprocessed text (for embedding
 * or testing without a preprocessor).
 *
 * Both allocate *out_data; caller must free it with rcw_free().
 */
rcw_error_t rcw_compile_file(rcw_ctx_t *ctx, const char *input_path, uint8_t **out_data, size_t *out_len);
rcw_error_t rcw_compile_buffer(rcw_ctx_t *ctx, const char *preprocessed, size_t len, uint8_t **out_data, size_t *out_len);

/*
 * Reverse decompilation: .bin -> .rcw text
 *
 * rcw_decompile_file() preprocesses rcwi_path with the embedded
 * preprocessor (mcpp) to obtain symbol definitions, then reads
 * bin_path and produces RCW source text. The output is prefixed with
 *     #include <basename(rcwi_path)>
 *
 * rcw_decompile_buffer() takes already-preprocessed .rcwi text and an
 * in-memory binary buffer. No preprocessor is invoked and no file
 * I/O is performed - useful for embedding the library in environments
 * where the .rcwi text is generated on the fly, or for unit testing.
 * If rcwi_name is non-NULL, the output is prefixed with
 * "#include <rcwi_name>\n\n"; pass NULL to omit the header.
 *
 * Both allocate *out_source; caller must free it with rcw_free().
 */
rcw_error_t rcw_decompile_file(rcw_ctx_t *ctx, const char *bin_path, const char *rcwi_path, char **out_source, size_t *out_len);
rcw_error_t rcw_decompile_buffer(rcw_ctx_t *ctx, const char *rcwi_preprocessed, size_t rcwi_len, const uint8_t *binary, size_t binary_len, const char *rcwi_name, char **out_source, size_t *out_len);

/*
 * Standalone preprocessing.
 *
 * Run the embedded mcpp preprocessor on input_path, honouring include
 * paths previously added via rcw_ctx_add_include_path(). The resulting
 * NUL-terminated text is suitable for direct use with
 * rcw_compile_buffer() or rcw_decompile_buffer().
 *
 * On success, *out points to a newly-allocated buffer (length *out_len,
 * excluding the terminating NUL). The caller must free it with
 * rcw_free().
 *
 * On preprocessor failure, returns RCW_ERR_PREPROCESS and mcpp
 * diagnostics are written to standard error.
 */
rcw_error_t rcw_preprocess_file(rcw_ctx_t *ctx, const char *input_path, char **out, size_t *out_len);

/* PBI command decoding */
typedef enum {
  RCW_PBI_UNKNOWN       = -1,
  RCW_PBI_BLOCKCOPY     = 0x00,  /* Block Copy */
  RCW_PBI_CCSR_COPY     = 0x02,  /* CCSR Write from Address */
  RCW_PBI_LOAD_RCW      = 0x10,  /* Load RCW with Checksum */
  RCW_PBI_LOAD_RCW_NOCS = 0x11,  /* Load RCW w/o Checksum */
  RCW_PBI_LOADACWINDOW  = 0x12,  /* Load Alt Cfg Window */
  RCW_PBI_LOADCONDITION = 0x14,  /* Load Condition */
  RCW_PBI_LOAD_SEC_HDR  = 0x20,  /* Load Security Header */
  RCW_PBI_LOAD_CSF_PTR  = 0x22,  /* Load Boot 1 CSF Header Ptr */
  RCW_PBI_CCSR_RMW      = 0x42,  /* CCSR Read, Modify, Write */
  RCW_PBI_POLL_SHORT    = 0x80,  /* Poll Short */
  RCW_PBI_POLL_LONG     = 0x81,  /* Poll Long */
  RCW_PBI_WAIT          = 0x82,  /* Wait */
  RCW_PBI_JUMP          = 0x84,  /* Jump */
  RCW_PBI_JUMP_COND     = 0x85,  /* Jump Conditional */
  RCW_PBI_CRC_STOP      = 0x8F,  /* CRC and Stop */
  RCW_PBI_STOP          = 0xFF,  /* Stop */
  RCW_PBI_CCSR_WRITE    = 0x100, /* CCSR Write */
  RCW_PBI_ALTCFG_WRITE  = 0x101, /* AltConfig Write */
} rcw_pbi_op_t;

/* The most operand words a command carries */
#define RCW_PBI_MAX_ARGS (512 / 4)

typedef struct {
  rcw_pbi_op_t op;
  uint32_t word;                   /* the command word */
  size_t size;                     /* whole command, in bytes */
  size_t nargs;                    /* operand words after the command word */
  uint32_t args[RCW_PBI_MAX_ARGS];
  /*
   * The operand carried by the command word itself:
   *   CCSR Write       the 28-bit CCSR address
   *   AltConfig Write  the 26-bit ALTCFG_OFFSET
   *   general commands the low 16 bits: Block Copy SRC, ALTCFG_WNDW,
   *                    the Wait count, the word type of CCSR Write from
   *                    Address, the ops and mask type of CCSR Read,
   *                    Modify, Write; zero for the others
   */
  uint32_t param;
  uint32_t width;                  /* bytes written by a CCSR or AltConfig Write, else 0 */
} rcw_pbi_cmd_t;

/*
 * Decode the command starting at buf[0]. Words are little-endian when
 * littleendian is non-zero (%littleendian=1, every pbiformat=2 platform).
 *
 *   RCW_OK             *cmd is filled in, the command is cmd->size bytes
 *   RCW_ERR_BAD_BINARY the word is not a PBI command: a header the RM
 *                      reserves, an unknown CMD byte, a CCSR Write with a
 *                      byte count of 0 or 2, an AltConfig Write wider than
 *                      512 bytes; cmd->word holds the word
 *   RCW_ERR_TRUNCATED  fewer than cmd->size bytes are available (cmd->op,
 *                      word, size and nargs are set, the args are not), or
 *                      len is below 4
 */
rcw_error_t rcw_pbi_decode(const uint8_t *buf, size_t len, int littleendian, rcw_pbi_cmd_t *cmd);

/* The .rcw mnemonic of op, "unknown" for RCW_PBI_UNKNOWN. */
const char *rcw_pbi_op_name(rcw_pbi_op_t op);

/*
 * PBL image envelope
 *
 * A PBL image is the preamble, the Load RCW command with its 32 RCW words
 * and checksum, then the PBI commands:
 *
 *   byte   0  preamble          0xAA55AA55                 4 bytes
 *   byte   4  Load RCW command  0x80100000 or 0x80110000   4 bytes
 *   byte   8  RCW               32 words                 128 bytes
 *   byte 136  checksum, or zero padding                    4 bytes
 *   byte 140  the first PBI command
 *
 * RCW[PBI_LENGTH] counts the PBI words, terminator included: the RM calls
 * the field a size in bytes, but NXP's rcw.py and TF-A's create_pbl write
 * words, and those images boot !
 */
#define RCW_PBL_RCW_OFFSET      (4 + 4)                                   /* preamble, Load RCW command word */
#define RCW_PBL_RCW_BYTES       (1024 / 8)                                /* the 1024-bit RCW */
#define RCW_PBL_CHECKSUM_OFFSET (RCW_PBL_RCW_OFFSET + RCW_PBL_RCW_BYTES)  /* 136 */
#define RCW_PBL_PBI_OFFSET      (RCW_PBL_CHECKSUM_OFFSET + 4)             /* 140 */

typedef struct {
  uint32_t load_rcw_cmd;       /* word 1: 0x80100000, or 0x80110000 without checksum */
  const uint8_t *rcw;          /* the 128 RCW bytes, inside buf */
  uint32_t checksum;           /* as stored after the RCW words */
  uint32_t checksum_computed;  /* preamble + Load RCW command + the 32 RCW words */
  uint32_t pbi_length;         /* RCW[PBI_LENGTH], in 32-bit words */
  int sb_en;                   /* RCW[SB_EN] */
  const uint8_t *pbi;          /* the first PBI command, inside buf */
  size_t pbi_avail;            /* bytes from pbi to the end of buf */
} rcw_pbl_t;

/*
 * Read the envelope of the PBL image in buf.
 *
 *   RCW_ERR_TRUNCATED  len is below 140
 *   RCW_ERR_BAD_BINARY no preamble, or word 1 is not a Load RCW command
 */
rcw_error_t rcw_pbl_parse(const uint8_t *buf, size_t len, int littleendian, rcw_pbl_t *pbl);

uint32_t rcw_pbi_crc(const uint8_t *pbi, size_t len);

/*
 * PBI check
 */

typedef void (*rcw_pbi_visit_t)(const rcw_pbi_cmd_t *cmd, size_t off, void *arg);

typedef struct {
  size_t len;
  size_t end;
  size_t ncmd;
  rcw_pbi_op_t stop_op;
  uint32_t crc;
  uint32_t crc_computed;
  rcw_pbi_cmd_t cmd;
} rcw_pbi_check_t;

/*
 * Check the PBI of a parsed PBL image
 *
 *   RCW_OK              the command at PBI_LENGTH is a Stop or a CRC and Stop
 *                       and every word executed before it is a command. The
 *                       CRC is reported, not judged: compare crc and
 *                       crc_computed. PBI_LENGTH 0 is a PBL without PBI.
 *   RCW_ERR_PBI_LENGTH  the command at PBI_LENGTH is not a terminator: a
 *                       command crosses it, the walk reaches it without a
 *                       terminator, or a Stop ends execution earlier and
 *                       something else sits at the end
 *   RCW_ERR_BAD_BINARY  the word at end is not a PBI command, cmd.word holds it
 *   RCW_ERR_TRUNCATED   pbl->pbi_avail is below 4 * PBI_LENGTH, so the command
 *                       at PBI_LENGTH cannot be read; the walk ran to end
 */
rcw_error_t rcw_pbl_check(const rcw_pbl_t *pbl, int littleendian, rcw_pbi_visit_t visit, void *arg, rcw_pbi_check_t *chk);

/*
 * Error information
 */
const char *rcw_strerror(rcw_error_t err);
const char *rcw_ctx_last_error_detail(const rcw_ctx_t *ctx);

/*
 * Memory management - free any buffer allocated by the library.
 */
void rcw_free(void *ptr);

#ifdef __cplusplus
}
#endif
