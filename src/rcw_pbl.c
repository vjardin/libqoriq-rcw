/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBL image envelope, PBI check and image verdict
 */

#include <string.h>

#include "rcw_internal.h"

#define PBL_RCW_PBI_LENGTH_BEGIN 287
#define PBL_RCW_PBI_LENGTH_END   276
#define PBL_RCW_SB_EN            266

static void
rcw_fields(const uint8_t *rcw, rcw_pbl_t *pbl) {
  pbl->rcw = rcw;
  pbl->pbi_length = (uint32_t)rcw_bits_extract(rcw, PBL_RCW_PBI_LENGTH_BEGIN,
                                               PBL_RCW_PBI_LENGTH_END, true);
  pbl->sb_en = rcw_bits_extract(rcw, PBL_RCW_SB_EN, PBL_RCW_SB_EN, true) != 0;
}

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
  pbl->checksum = cmd.args[PBL_RCW_WORDS];
  pbl->checksum_computed = PBL_PREAMBLE + cmd.word;
  for (size_t i = 0; i < PBL_RCW_WORDS; i++)
    pbl->checksum_computed += cmd.args[i];

  rcw_fields(buf + RCW_PBL_RCW_OFFSET, pbl);
  pbl->pbi = buf + RCW_PBL_PBI_OFFSET;
  pbl->pbi_avail = len - RCW_PBL_PBI_OFFSET;

  return RCW_OK;
}

void
rcw_pbl_from_rcw(const uint8_t rcw[RCW_SIZE_BYTES], rcw_pbl_t *pbl) {
  memset(pbl, 0, sizeof(*pbl));
  rcw_fields(rcw, pbl);
}

uint32_t
rcw_pbi_crc(const uint8_t *pbi, size_t len) {
  uint32_t crc = rcw_crc32_update(RCW_CRC32_INIT, pbi, len, true);

  return rcw_reverse_word(crc) ^ RCW_CRC32_INIT;
}

#define PBI_JUMP_MIN_OFFSET 8

static bool
terminator(rcw_pbi_op_t op) {
  return op == RCW_PBI_STOP || op == RCW_PBI_CRC_STOP;
}

static rcw_error_t
pbl_walk(const rcw_pbl_t *pbl, int littleendian, rcw_pbi_visit_t visit, void *arg,
         rcw_pbi_check_t *chk, size_t *term_end) {
  size_t len = PBI_WORD_BYTES * (size_t)pbl->pbi_length;
  size_t avail = (len < pbl->pbi_avail) ? len : pbl->pbi_avail;
  size_t off = 0;
  bool condition = false, stopped = false, conditional = false;
  rcw_error_t err = RCW_OK;
  const rcw_pbi_cmd_t *last;
  rcw_pbi_cmd_t final;

  memset(chk, 0, sizeof(*chk));
  *term_end = 0;
  chk->len = len;
  chk->stop_op = RCW_PBI_UNKNOWN;
  chk->cmd.op = RCW_PBI_UNKNOWN;
  if (len == 0)
    return RCW_OK;

  while (off < avail) {
    err = rcw_pbi_decode(pbl->pbi + off, avail - off, littleendian, &chk->cmd);
    chk->end = off;
    if (err == RCW_ERR_BAD_BINARY)
      return err;
    if (err != RCW_OK)
      break;
    chk->ncmd++;
    if (visit)
      visit(&chk->cmd, off, arg);

    if (terminator(chk->cmd.op)) {
      off += chk->cmd.size;
      chk->end = off;
      *term_end = off;
      stopped = true;
      break;
    }
    if (chk->cmd.op == RCW_PBI_LOADCONDITION)
      condition = true;
    if (chk->cmd.op == RCW_PBI_JUMP || chk->cmd.op == RCW_PBI_JUMP_COND) {
      uint32_t offset = chk->cmd.args[0];

      if (offset % PBI_WORD_BYTES != 0 || offset <= PBI_JUMP_MIN_OFFSET ||
          offset >= len - off)
        return RCW_ERR_PBI_JUMP;
      if (chk->cmd.op == RCW_PBI_JUMP_COND) {
        if (!condition)
          return RCW_ERR_PBI_NO_CONDITION;
        conditional = true;
        break;
      }
      off += offset;
      continue;
    }
    off += chk->cmd.size;
    chk->end = off;
  }
  if (avail < len)
    return RCW_ERR_TRUNCATED;
  if (!stopped && !conditional)
    return err == RCW_ERR_TRUNCATED ? RCW_ERR_PBI_OVERRUN : RCW_ERR_PBI_LENGTH;

  last = &chk->cmd;
  if (chk->end < len || conditional) {
    if (rcw_pbi_decode(pbl->pbi + len - PBL_TERMINATOR_BYTES, PBL_TERMINATOR_BYTES,
                       littleendian, &final) != RCW_OK || !terminator(final.op))
      return RCW_ERR_PBI_LENGTH;
    last = &final;
  }
  chk->stop_op = last->op;
  if (last->op == RCW_PBI_CRC_STOP) {
    chk->crc = last->args[0];
    chk->crc_computed = rcw_pbi_crc(pbl->pbi, len - PBI_WORD_BYTES);
  }

  return conditional ? RCW_ERR_PBI_CONDITIONAL : RCW_OK;
}

rcw_error_t
rcw_pbl_check(const rcw_pbl_t *pbl, int littleendian, rcw_pbi_visit_t visit, void *arg,
              rcw_pbi_check_t *chk) {
  size_t term_end;

  return pbl_walk(pbl, littleendian, visit, arg, chk, &term_end);
}

rcw_error_t
rcw_pbl_verify(const uint8_t *buf, size_t len, int littleendian, rcw_pbi_visit_t visit, void *arg,
               rcw_pbl_report_t *rep) {
  size_t term_end = 0;
  size_t pbi_end;
  bool checked;

  memset(rep, 0, sizeof(*rep));
  rep->image_end = len;
  if (len >= 2 * PBI_WORD_BYTES) {
    rep->word0 = rcw_unpack32(buf, littleendian);
    rep->word1 = rcw_unpack32(buf + PBI_WORD_BYTES, littleendian);
    rep->preamble_ok = rep->word0 == PBL_PREAMBLE;
    rep->load_rcw_ok = rep->word1 == PBL_CMD_LOAD_RCW || rep->word1 == PBL_CMD_LOAD_RCW_NOCS;
  }

  rep->envelope = rcw_pbl_parse(buf, len, littleendian, &rep->pbl);
  rep->pbi = rep->envelope;
  if (rep->envelope != RCW_OK)
    return rep->envelope;

  rep->has_checksum = rep->pbl.load_rcw_cmd == PBL_CMD_LOAD_RCW;
  rep->checksum_ok = !rep->has_checksum || rep->pbl.checksum == rep->pbl.checksum_computed;

  rep->pbi = pbl_walk(&rep->pbl, littleendian, visit, arg, &rep->chk, &term_end);
  rep->terminated = term_end > 0;
  checked = rep->pbi == RCW_OK || rep->pbi == RCW_ERR_PBI_CONDITIONAL;
  rep->crc_ok = checked &&
                (rep->chk.stop_op != RCW_PBI_CRC_STOP || rep->chk.crc == rep->chk.crc_computed);
  rep->ok = rep->checksum_ok && rep->pbi == RCW_OK && rep->crc_ok;

  if (checked)
    pbi_end = rep->chk.len;
  else if (rep->terminated)
    pbi_end = term_end;
  else
    pbi_end = rep->pbl.pbi_avail;
  if (RCW_PBL_PBI_OFFSET + pbi_end < len)
    rep->image_end = RCW_PBL_PBI_OFFSET + pbi_end;

  if (!rep->checksum_ok)
    return RCW_ERR_CHECKSUM;
  if (!checked)
    return rep->pbi;
  if (!rep->crc_ok)
    return RCW_ERR_CRC;
  if (rep->pbi != RCW_OK)
    return rep->pbi;

  return RCW_OK;
}
