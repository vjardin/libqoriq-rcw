/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI command decoder
 */

#include <string.h>

#include "rcw_internal.h"

/*
 * Operand words of a general command, from its CMD byte
 * Load RCW is listed for completeness: it never appears in a
 * PBI sequence, it precedes it.
 */
static bool
general_layout(uint8_t cmd, rcw_pbi_op_t *op, size_t *nargs) {
  switch (cmd) {
  case PBI_GENCMD_BLOCKCOPY:     *op = RCW_PBI_BLOCKCOPY;     *nargs = 3;  return true;
  case PBI_GENCMD_CCSR_COPY:     *op = RCW_PBI_CCSR_COPY;     *nargs = 3;  return true;
  case PBI_GENCMD_LOAD_RCW_CS:   *op = RCW_PBI_LOAD_RCW;      *nargs = PBL_LOAD_RCW_WORDS; return true;
  case PBI_GENCMD_LOAD_RCW_NOCS: *op = RCW_PBI_LOAD_RCW_NOCS; *nargs = PBL_LOAD_RCW_WORDS; return true;
  case PBI_GENCMD_LOADACWINDOW:  *op = RCW_PBI_LOADACWINDOW;  *nargs = 0;  return true;
  case PBI_GENCMD_LOADCONDITION: *op = RCW_PBI_LOADCONDITION; *nargs = 2;  return true;
  case PBI_GENCMD_LOAD_SEC_HDR:  *op = RCW_PBI_LOAD_SEC_HDR;  *nargs = PBI_CSF_HEADER_WORDS; return true;
  case PBI_GENCMD_LOAD_CSF_PTR:  *op = RCW_PBI_LOAD_CSF_PTR;  *nargs = 1;  return true;
  case PBI_GENCMD_CCSR_RMW:      *op = RCW_PBI_CCSR_RMW;      *nargs = 3;  return true;
  case PBI_GENCMD_POLL_SHORT:    *op = RCW_PBI_POLL_SHORT;    *nargs = 3;  return true;
  case PBI_GENCMD_POLL_LONG:     *op = RCW_PBI_POLL_LONG;     *nargs = 3;  return true;
  case PBI_GENCMD_WAIT:          *op = RCW_PBI_WAIT;          *nargs = 0;  return true;
  case PBI_GENCMD_JUMP:          *op = RCW_PBI_JUMP;          *nargs = 1;  return true;
  case PBI_GENCMD_JUMP_COND:     *op = RCW_PBI_JUMP_COND;     *nargs = 2;  return true;
  case PBI_GENCMD_CRC_STOP:      *op = RCW_PBI_CRC_STOP;      *nargs = 1;  return true;
  case PBI_GENCMD_STOP:          *op = RCW_PBI_STOP;          *nargs = 1;  return true;
  }
  return false;
}

rcw_error_t
rcw_pbi_decode(const uint8_t *buf, size_t len, int littleendian, rcw_pbi_cmd_t *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  cmd->op = RCW_PBI_UNKNOWN;
  if (len < PBI_WORD_BYTES)
    return RCW_ERR_TRUNCATED;

  uint32_t w = rcw_unpack32(buf, littleendian);
  uint8_t hdr = (uint8_t)(w >> PBI_HDR_SHIFT);
  cmd->word = w;

  if ((hdr & (PBI_HDR_ACS | PBI_HDR_RSVD)) == 0) {
    uint32_t b = (hdr & PBI_CCSR_TYPE_MASK) >> PBI_CCSR_TYPE_SHIFT;

    if (b != PBI_CCSR_WRITE_B1 && b != PBI_CCSR_WRITE_B4)
      return RCW_ERR_BAD_BINARY;
    cmd->op = RCW_PBI_CCSR_WRITE;
    cmd->nargs = 1;
    cmd->param = w & PBI_CCSR_ADDR_MASK;
    cmd->width = 1u << (b - 1); /* N = 2^(B-1): 1 or 4 bytes */

  } else if ((hdr & (PBI_HDR_ACS | PBI_HDR_RSVD)) == PBI_HDR_ACS) {
    uint32_t b = (hdr & PBI_AWRITE_B_MASK) >> PBI_AWRITE_B_RSHIFT;

    if (b != 0) {
      if (b > PBI_AWRITE_B_MAX)
        return RCW_ERR_BAD_BINARY;
      cmd->op = RCW_PBI_ALTCFG_WRITE;
      cmd->width = 1u << (b - 1);
      cmd->nargs = (cmd->width + PBI_WORD_BYTES - 1) / PBI_WORD_BYTES;
      cmd->param = w & PBI_AWRITE_ADDR_MASK;

    } else if (hdr == PBI_HDR_GENERAL) {
      if (!general_layout((uint8_t)((w & PBI_CMD_MASK) >> PBI_CMD_SHIFT),
                          &cmd->op, &cmd->nargs))
        return RCW_ERR_BAD_BINARY;
      cmd->param = w & PBI_PARAM_MASK;

    } else {
      return RCW_ERR_BAD_BINARY;
    }

  } else {
    return RCW_ERR_BAD_BINARY;
  }

  cmd->size = PBI_WORD_BYTES * (1 + cmd->nargs); /* the command word and its operands */
  if (cmd->size > len)
    return RCW_ERR_TRUNCATED;
  for (size_t i = 0; i < cmd->nargs; i++)
    cmd->args[i] = rcw_unpack32(buf + PBI_WORD_BYTES * (1 + i), littleendian);

  return RCW_OK;
}

const char *
rcw_pbi_op_name(rcw_pbi_op_t op) {
  switch (op) {
  case RCW_PBI_UNKNOWN:       break;
  case RCW_PBI_BLOCKCOPY:     return "blockcopy";
  case RCW_PBI_CCSR_COPY:     return "ccsrcopy";
  case RCW_PBI_LOAD_RCW:      return "loadrcw";
  case RCW_PBI_LOAD_RCW_NOCS: return "loadrcw.nochecksum";
  case RCW_PBI_LOADACWINDOW:  return "loadacwindow";
  case RCW_PBI_LOADCONDITION: return "loadcondition";
  case RCW_PBI_LOAD_SEC_HDR:  return "loadsecurityheader";
  case RCW_PBI_LOAD_CSF_PTR:  return "loadboot1csfptr";
  case RCW_PBI_CCSR_RMW:      return "ccsrrmw";
  case RCW_PBI_POLL_SHORT:    return "poll.short";
  case RCW_PBI_POLL_LONG:     return "poll.long";
  case RCW_PBI_WAIT:          return "wait";
  case RCW_PBI_JUMP:          return "jump";
  case RCW_PBI_JUMP_COND:     return "jumpif";
  case RCW_PBI_CRC_STOP:      return "crcstop";
  case RCW_PBI_STOP:          return "stop";
  case RCW_PBI_CCSR_WRITE:    return "write";
  case RCW_PBI_ALTCFG_WRITE:  return "awrite";
  }
  return "unknown";
}
