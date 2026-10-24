/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBL image envelope
 */

#include <string.h>

#include "rcw_internal.h"

#define PBL_RCW_PBI_LENGTH_BEGIN 287
#define PBL_RCW_PBI_LENGTH_END   276
#define PBL_RCW_SB_EN            266

rcw_error_t
rcw_pbl_parse(const uint8_t *buf, size_t len, int littleendian, rcw_pbl_t *pbl) {
  rcw_pbi_cmd_t cmd;

  memset(pbl, 0, sizeof(*pbl));
  if (len < RCW_PBL_PBI_OFFSET)
    return RCW_ERR_TRUNCATED;
  if (rcw_unpack32(buf, littleendian) != PBL_PREAMBLE)
    return RCW_ERR_BAD_BINARY;

  if (rcw_pbi_decode(buf + PBI_WORD_BYTES, len - PBI_WORD_BYTES, littleendian, &cmd) != RCW_OK ||
      (cmd.op != RCW_PBI_LOAD_RCW && cmd.op != RCW_PBI_LOAD_RCW_NOCS))
    return RCW_ERR_BAD_BINARY;

  pbl->load_rcw_cmd = cmd.word;
  pbl->rcw = buf + RCW_PBL_RCW_OFFSET;
  pbl->checksum = cmd.args[PBL_RCW_WORDS];
  pbl->checksum_computed = PBL_PREAMBLE + cmd.word;
  for (size_t i = 0; i < PBL_RCW_WORDS; i++)
    pbl->checksum_computed += cmd.args[i];

  pbl->pbi_length = (uint32_t)rcw_bits_extract(pbl->rcw, PBL_RCW_PBI_LENGTH_BEGIN,
                                               PBL_RCW_PBI_LENGTH_END, true);
  pbl->sb_en = rcw_bits_extract(pbl->rcw, PBL_RCW_SB_EN, PBL_RCW_SB_EN, true) != 0;
  pbl->pbi = buf + RCW_PBL_PBI_OFFSET;
  pbl->pbi_avail = len - RCW_PBL_PBI_OFFSET;

  return RCW_OK;
}

uint32_t
rcw_pbi_crc(const uint8_t *pbi, size_t len) {
  uint32_t crc = rcw_crc32_update(RCW_CRC32_INIT, pbi, len, true);

  return rcw_reverse_word(crc) ^ RCW_CRC32_INIT;
}

rcw_error_t
rcw_pbl_check(const rcw_pbl_t *pbl, int littleendian, rcw_pbi_visit_t visit, void *arg,
              rcw_pbi_check_t *chk) {
  size_t len = PBI_WORD_BYTES * (size_t)pbl->pbi_length;
  size_t avail = (len < pbl->pbi_avail) ? len : pbl->pbi_avail;
  size_t off = 0;
  const rcw_pbi_cmd_t *last = NULL;
  rcw_pbi_cmd_t final;

  memset(chk, 0, sizeof(*chk));
  chk->len = len;
  chk->stop_op = RCW_PBI_UNKNOWN;
  chk->cmd.op = RCW_PBI_UNKNOWN;
  if (len == 0)
    return RCW_OK;

  while (off < avail) {
    rcw_error_t err = rcw_pbi_decode(pbl->pbi + off, avail - off, littleendian, &chk->cmd);

    chk->end = off;
    if (err == RCW_ERR_BAD_BINARY)
      return err;
    if (err != RCW_OK)
      break;
    chk->ncmd++;
    if (visit)
      visit(&chk->cmd, off, arg);
    off += chk->cmd.size;
    chk->end = off;
    if (chk->cmd.op == RCW_PBI_STOP || chk->cmd.op == RCW_PBI_CRC_STOP) {
      last = &chk->cmd;
      break;
    }
  }
  if (avail < len)
    return RCW_ERR_TRUNCATED;
  if (!last)
    return RCW_ERR_PBI_LENGTH;

  if (chk->end < len) {
    if (rcw_pbi_decode(pbl->pbi + len - PBL_TERMINATOR_BYTES, PBL_TERMINATOR_BYTES,
                       littleendian, &final) != RCW_OK ||
        (final.op != RCW_PBI_STOP && final.op != RCW_PBI_CRC_STOP))
      return RCW_ERR_PBI_LENGTH;
    last = &final;
  }
  chk->stop_op = last->op;
  if (last->op == RCW_PBI_CRC_STOP) {
    chk->crc = last->args[0];
    chk->crc_computed = rcw_pbi_crc(pbl->pbi, len - PBI_WORD_BYTES);
  }

  return RCW_OK;
}
