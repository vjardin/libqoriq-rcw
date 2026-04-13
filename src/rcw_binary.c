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

/* Reverse the bit order within a byte (same as in rcw_bits.c). */
static uint8_t
reverse_byte(uint8_t b) {
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));

  return b;
}

/*
 * Reverse all 32 bits of a word.
 * Matches rcw.py: int(bin(crc)[2:].zfill(32)[::-1], 2)
 */
static uint32_t
reverse_word_bits(uint32_t w) {
  uint32_t r = 0;

  for (int i = 0; i < 32; i++) {
    r <<= 1;
    r |= (w & 1);
    w >>= 1;
  }
  return r;
}

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

static uint32_t
unpack_le32(const uint8_t *buf) {
  return (uint32_t)buf[0]
      | ((uint32_t)buf[1] << 8)
      | ((uint32_t)buf[2] << 16)
      | ((uint32_t)buf[3] << 24)
      ;
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
      uint64_t pbilen = ctx->pbi.len / 4;
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
    total += 8; /* preamble + load RCW command */
  total += RCW_SIZE_BYTES;
  if (ctx->pbl)
    total += 4; /* checksum */
  total += ctx->pbi.len;
  if (ctx->pbl)
    total += 8; /* terminator */

  uint8_t *binary = malloc(total);
  if (!binary)
    return RCW_ERR_NOMEM;

  size_t pos = 0;

  /* Preamble */
  if (ctx->pbl) {
    pack(binary + pos, PBL_PREAMBLE);
    pos += 4;

    if (ctx->vars.loadwochecksum)
      pack(binary + pos, PBL_CMD_LOAD_RCW_NOCS);
    else
      pack(binary + pos, PBL_CMD_LOAD_RCW);
    pos += 4;
  }

  /* RCW data */
  memcpy(binary + pos, rcw_bytes, RCW_SIZE_BYTES);
  pos += RCW_SIZE_BYTES;

  /* Checksum (sum of all 32-bit words so far) */
  if (ctx->pbl) {
    if (ctx->vars.loadwochecksum) {
      pack(binary + pos, 0x00000000);
    } else {
      uint32_t checksum = 0;
      for (size_t i = 0; i < pos; i += 4) {
        checksum += unpack_le32(binary + i);
      }
      pack(binary + pos, checksum);
    }
    pos += 4;
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
      pos += 4;
      pack(binary + pos, 0x00000000);
      pos += 4;
    } else {
      /* CRC and Stop */
      uint32_t cmd = PBL_CMD_CRC_STOP;
      uint8_t cmd_bytes[4];
      pack(cmd_bytes, cmd);

    /*
     * CRC covers PBI data + stop command word.
     * Matches rcw.py line 716: crcbinary = pbi
     */
    size_t crc_len = ctx->pbi.len + 4;
    uint8_t *crc_input = malloc(crc_len);
    if (!crc_input) {
      free(binary);
      return RCW_ERR_NOMEM;
    }
    if (ctx->pbi.len > 0)
      memcpy(crc_input, ctx->pbi.data, ctx->pbi.len);
    memcpy(crc_input + ctx->pbi.len, cmd_bytes, 4);

    /*
     * Apply classicbitnumbers bit-reversal to CRC input.
     * Matches rcw.py lines 730-736.
     */
    if (ctx->vars.classicbitnumbers) {
      for (size_t i = 0; i < crc_len; i++)
        crc_input[i] = reverse_byte(crc_input[i]);
    }

    uint32_t crc = rcw_crc32(crc_input, crc_len);
    free(crc_input);

    /*
     * Apply classicbitnumbers bit-reversal to CRC result.
     * Matches rcw.py lines 741-742.
     */
    if (ctx->vars.classicbitnumbers)
      crc = reverse_word_bits(crc);

    crc ^= RCW_CRC32_INIT;

    pack(binary + pos, cmd);
    pos += 4;
    pack(binary + pos, crc);
    pos += 4;
    }
  }

  *out_data = binary;
  *out_len = pos;

  return RCW_OK;
}
