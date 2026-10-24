/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Binary generation - forward compilation of parsed RCW to .bin.
 * Mirrors rcw.py create_binary() (lines 597-760), pbiformat=2 only.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "rcw_internal.h"

/* Pack a 32-bit word in the given endianness into buf. */
static void
pack_le32(uint8_t *buf, uint32_t val) {
  buf[0] = (uint8_t)(val & 0xFF);
  buf[1] = (uint8_t)((val >> 8) & 0xFF);
  buf[2] = (uint8_t)((val >> 16) & 0xFF);
  buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void
pack_be32(uint8_t *buf, uint32_t val) {
  buf[0] = (uint8_t)((val >> 24) & 0xFF);
  buf[1] = (uint8_t)((val >> 16) & 0xFF);
  buf[2] = (uint8_t)((val >> 8) & 0xFF);
  buf[3] = (uint8_t)(val & 0xFF);
}

typedef void (*pack_fn)(uint8_t *buf, uint32_t val);

rcw_error_t
rcw_binary_generate(rcw_ctx_t *ctx, uint8_t **out_data, size_t *out_len) {
  pack_fn pack = ctx->vars.littleendian ? pack_le32 : pack_be32;

  /*
   * Auto-calculate PBI_LENGTH if defined but not assigned.
   * Matches rcw.py lines 638-644.
   */
  for (size_t i = 0; i < ctx->symbols.count; i++) {
    rcw_symbol_t *sym = &ctx->symbols.entries[i];
    if (strcmp(sym->name, "PBI_LENGTH") == 0 && !sym->has_value) {
      uint64_t pbilen = ctx->pbi.len / PBI_WORD_BYTES;
      if (ctx->pbl)
        pbilen += PBL_TERMINATOR_WORDS; /* CRC+Stop or Stop */
      sym->has_value = true;
      sym->value = pbilen;
      break;
    }
  }

  /* Pack the 1024-bit RCW into 128 bytes */
  uint8_t rcw_bytes[RCW_SIZE_BYTES];

  rcw_error_t err = rcw_bits_pack(&ctx->symbols, &ctx->vars, rcw_bytes);

  if (err != RCW_OK)
    return err;

  /*
   * Compute the output size:
   *   PBL header: 8 bytes (preamble + load RCW cmd)
   *   RCW: 128 bytes
   *   Checksum: 4 bytes
   *   PBI data: ctx->pbi.len bytes
   *   Terminator: 8 bytes (stop/CRC + value)
   */
  size_t total = 0;
  if (ctx->pbl)
    total += RCW_PBL_RCW_OFFSET; /* preamble, Load RCW command word */
  total += RCW_SIZE_BYTES;
  if (ctx->pbl)
    total += PBI_WORD_BYTES; /* checksum word */
  total += ctx->pbi.len;
  if (ctx->pbl)
    total += PBL_TERMINATOR_BYTES; /* Stop, or CRC and Stop */

  /*
   * LX2160A SP scratch ceiling: see PBL_SP_SCRATCH_CEILING_BYTES
   * in rcw_internal.h for the rationale and trade-offs.
   */
  if (total >= PBL_SP_SCRATCH_CEILING_BYTES) {
    size_t pbi_body = ctx->pbi.len;
    size_t header_overhead = total - pbi_body - RCW_SIZE_BYTES;
    fprintf(stderr,
        "warning: total PBL size is %zu bytes -- on LX2160A this "
        "exceeds the SP scratch ceiling at %u bytes; "
        "the chip will boot SILENTLY (no UART banner, no BL2 output).\n"
        "  RCW header:   %d bytes (fixed)\n"
        "  PBI body:     %zu bytes\n"
        "  Other:        %zu bytes (preamble + checksum + terminator)\n"
        "  Trim PBI commands or drop errata to bring total below %u. "
        "See libqoriq-rcw/src/rcw_binary.c for bench evidence.\n",
        total, PBL_SP_SCRATCH_CEILING_BYTES,
        RCW_SIZE_BYTES, pbi_body, header_overhead,
        PBL_SP_SCRATCH_CEILING_BYTES);
  }

  uint8_t *binary = malloc(total);
  if (!binary)
    return RCW_ERR_NOMEM;

  size_t pos = 0;

  /* Preamble */
  if (ctx->pbl) {
    pack(binary + pos, PBL_PREAMBLE);
    pos += PBI_WORD_BYTES;

    if (ctx->vars.loadwochecksum)
      pack(binary + pos, PBL_CMD_LOAD_RCW_NOCS);
    else
      pack(binary + pos, PBL_CMD_LOAD_RCW);
    pos += PBI_WORD_BYTES;
  }

  /* RCW data */
  memcpy(binary + pos, rcw_bytes, RCW_SIZE_BYTES);
  pos += RCW_SIZE_BYTES;

  /*
   * Checksum: the 32-bit sum of every word so far, the preamble, the Load
   * RCW command and the 32 RCW words
   */
  if (ctx->pbl) {
    if (ctx->vars.loadwochecksum) {
      pack(binary + pos, 0x00000000);
    } else {
      uint32_t checksum = 0;
      for (size_t i = 0; i < pos; i += PBI_WORD_BYTES)
        checksum += rcw_unpack32(binary + i, ctx->vars.littleendian);
      pack(binary + pos, checksum);
    }
    pos += PBI_WORD_BYTES;
  }

  /* PBI data */
  if (ctx->pbi.len > 0) {
    memcpy(binary + pos, ctx->pbi.data, ctx->pbi.len);
    pos += ctx->pbi.len;
  }

  /* Terminator */
  if (ctx->pbl) {
    if (ctx->vars.nocrc) {
      /* Stop command (no CRC) */
      pack(binary + pos, PBL_CMD_STOP);
      pos += PBI_WORD_BYTES;
      pack(binary + pos, 0x00000000);
      pos += PBI_WORD_BYTES;
    } else {
      /* CRC and Stop */
      uint32_t cmd = PBL_CMD_CRC_STOP;
      uint8_t cmd_bytes[4];
      pack(cmd_bytes, cmd);

    /*
     * CRC covers PBI data + stop command word.
     */
    bool classic = ctx->vars.classicbitnumbers;
    uint32_t crc = rcw_crc32_update(RCW_CRC32_INIT, ctx->pbi.data,
                                    ctx->pbi.len, classic);
    crc = rcw_crc32_update(crc, cmd_bytes, sizeof(cmd_bytes), classic);
    if (classic)
      crc = rcw_reverse_word(crc);
    crc ^= RCW_CRC32_INIT;

    pack(binary + pos, cmd);
    pos += PBI_WORD_BYTES;
    pack(binary + pos, crc);
    pos += PBI_WORD_BYTES;
    }
  }

  *out_data = binary;
  *out_len = pos;

  return RCW_OK;
}
