/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Reverse decompilation: .bin -> .rcw source text.
 * Mirrors rcw.py create_source() (lines 763-1046), pbiformat=2 only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "rcw_internal.h"

/* Dynamic string buffer for building source output. */
typedef struct {
  char  *data;
  size_t len;
  size_t cap;
} strbuf_t;

static rcw_error_t
strbuf_init(strbuf_t *sb) {
  sb->cap = 4096;
  sb->len = 0;
  sb->data = malloc(sb->cap);
  if (!sb->data)
    return RCW_ERR_NOMEM;
  sb->data[0] = '\0';

  return RCW_OK;
}

__attribute__((format(printf, 2, 3)))
static rcw_error_t
strbuf_printf(strbuf_t *sb, const char *fmt, ...) {
  va_list ap;

  for (;;) {
    size_t avail = sb->cap - sb->len;
    va_start(ap, fmt);
    int n = vsnprintf(sb->data + sb->len, avail, fmt, ap);
    va_end(ap);

    if (n < 0)
      return RCW_ERR_IO;

    if ((size_t)n < avail) {
      sb->len += (size_t)n;
      return RCW_OK;
    }

    size_t newcap = sb->cap * 2;
    while (newcap < sb->len + (size_t)n + 1)
      newcap *= 2;
    char *p = realloc(sb->data, newcap);
    if (!p)
      return RCW_ERR_NOMEM;
    sb->data = p;
    sb->cap = newcap;
  }
}

static rcw_error_t
disassemble_pbi(strbuf_t *sb, const uint8_t *pbi, size_t pbi_len,
                int littleendian) {
  size_t i = 0;

  while (i + 4 <= pbi_len) {
    rcw_pbi_cmd_t c;
    rcw_error_t err = rcw_pbi_decode(pbi + i, pbi_len - i, littleendian, &c);

    if (err == RCW_ERR_TRUNCATED && c.op == RCW_PBI_STOP) {
      strbuf_printf(sb, "/* Stop command */\n");
      i += 4;
      continue;
    }
    if (err != RCW_OK) {
      strbuf_printf(sb, "/* Unknown word 0x%08x */\n", c.word);
      i += 4;
      continue;
    }

    switch (c.op) {
    case RCW_PBI_CCSR_WRITE:
      strbuf_printf(sb, "%s 0x%08x,0x%08x\n",
                    c.width == 1 ? "write.b1" : "write", c.param, c.args[0]);
      break;

    case RCW_PBI_ALTCFG_WRITE: {
      uint32_t b = (c.word >> PBI_AWRITE_B_SHIFT) & PBI_AWRITE_B_BITS;
      if (b == PBI_AWRITE_B3)
        strbuf_printf(sb, "awrite 0x%08x", c.param);
      else
        strbuf_printf(sb, "awrite.b%u 0x%08x", b, c.param);
      for (size_t k = 0; k < c.nargs; k++)
        strbuf_printf(sb, ",0x%08x", c.args[k]);
      strbuf_printf(sb, "\n");
      break;
    }

    case RCW_PBI_BLOCKCOPY:
      strbuf_printf(sb, "blockcopy 0x%02x,0x%08x,0x%08x,0x%08x\n",
                    c.param & PBI_BLOCKCOPY_SRC_MAX, c.args[0], c.args[1], c.args[2]);
      break;

    case RCW_PBI_CCSR_COPY: {
      static const char *wsuf[4] = { "?", "b1", "b2", "b4" };
      strbuf_printf(sb, "ccsrcopy.%s 0x%08x,0x%08x,0x%08x\n",
                    wsuf[c.param & PBI_CCSR_COPY_TYPE_MASK], c.args[0], c.args[1], c.args[2]);
      break;
    }

    case RCW_PBI_CCSR_RMW: {
      static const char *msuf[4] = { "?", "b1", "b2", "b4" };
      static const char *opnm[4] = { "ccsrset", "ccsrclear",
                                     "ccsrreplace", "ccsrreserved" };
      uint32_t mt  = c.param & PBI_RMW_MASK_TYPE_MASK;
      uint32_t ops = (c.param >> PBI_RMW_OPS_SHIFT) & PBI_RMW_OPS_MASK;
      if (ops == PBI_RMW_OPS_REPLACE)
        strbuf_printf(sb, "%s.%s 0x%08x,0x%08x,0x%08x\n",
                      opnm[ops], msuf[mt], c.args[0], c.args[1], c.args[2]);
      else
        strbuf_printf(sb, "%s.%s 0x%08x,0x%08x\n",
                      opnm[ops], msuf[mt], c.args[0], c.args[1]);
      break;
    }

    case RCW_PBI_LOAD_RCW:
    case RCW_PBI_LOAD_RCW_NOCS:
      strbuf_printf(sb, "/* Disassemble not implemented for word 0x%08x */\n", c.word);
      break;

    case RCW_PBI_LOADACWINDOW:
      strbuf_printf(sb, "loadacwindow 0x%08x\n", c.param & PBI_ACWINDOW_MAX);
      break;

    case RCW_PBI_LOADCONDITION:
      strbuf_printf(sb, "loadcondition 0x%08x,0x%08x\n", c.args[0], c.args[1]);
      break;

    case RCW_PBI_LOAD_SEC_HDR:
      strbuf_printf(sb, "loadsecurityheader ");
      for (size_t k = 0; k < c.nargs; k++)
        strbuf_printf(sb, "%s0x%08x", (k == 0) ? "" : ",", c.args[k]);
      strbuf_printf(sb, "\n");
      break;

    case RCW_PBI_LOAD_CSF_PTR:
      strbuf_printf(sb, "loadboot1csfptr 0x%08x\n", c.args[0]);
      break;

    case RCW_PBI_POLL_SHORT:
    case RCW_PBI_POLL_LONG:
      strbuf_printf(sb, "%s 0x%08x,0x%08x,0x%08x\n", rcw_pbi_op_name(c.op),
                    c.args[0], c.args[1], c.args[2]);
      break;

    case RCW_PBI_WAIT:
      strbuf_printf(sb, "wait 0x%08x\n", c.param);
      break;

    case RCW_PBI_JUMP:
      strbuf_printf(sb, "jump 0x%08x\n", c.args[0]);
      break;

    case RCW_PBI_JUMP_COND:
      strbuf_printf(sb, "jumpif 0x%08x,0x%08x\n", c.args[0], c.args[1]);
      break;

    case RCW_PBI_CRC_STOP:
      strbuf_printf(sb, "/* CRC and Stop command (CRC 0x%08x)*/\n", c.args[0]);
      break;

    case RCW_PBI_STOP:
      strbuf_printf(sb, "/* Stop command */\n");
      break;

    case RCW_PBI_UNKNOWN:
      break;
    }

    i += c.size;
  }

  return RCW_OK;
}

rcw_error_t
rcw_decompile(rcw_ctx_t *ctx, const uint8_t *binary, size_t len, char **out_source, size_t *out_len) {
  strbuf_t sb;
  rcw_error_t err = strbuf_init(&sb);
  if (err != RCW_OK)
    return err;

  const uint8_t *rcw_data;
  const uint8_t *pbi_data = NULL;
  size_t pbi_len = 0;

  /* Detect and skip preamble */
  uint8_t preamble_bytes[4];
  if (ctx->vars.littleendian) {
    preamble_bytes[0] = (uint8_t)(PBL_PREAMBLE & 0xFF);
    preamble_bytes[1] = (uint8_t)((PBL_PREAMBLE >> 8) & 0xFF);
    preamble_bytes[2] = (uint8_t)((PBL_PREAMBLE >> 16) & 0xFF);
    preamble_bytes[3] = (uint8_t)((PBL_PREAMBLE >> 24) & 0xFF);

  } else {
    preamble_bytes[0] = (uint8_t)((PBL_PREAMBLE >> 24) & 0xFF);
    preamble_bytes[1] = (uint8_t)((PBL_PREAMBLE >> 16) & 0xFF);
    preamble_bytes[2] = (uint8_t)((PBL_PREAMBLE >> 8) & 0xFF);
    preamble_bytes[3] = (uint8_t)(PBL_PREAMBLE & 0xFF);

  }

  /*
   * Need at least preamble(4) + load-cmd(4) + RCW(128) + checksum(4)
   * = 140 bytes before we can treat the buffer as PBL-wrapped. The
   * older check (len > RCW_SIZE_BYTES) accepted any 129+-byte input
   * starting with the preamble, which then over-read the RCW area.
   */
  size_t pbl_min = RCW_PBL_PBI_OFFSET;
  if (len >= pbl_min && memcmp(binary, preamble_bytes, sizeof(preamble_bytes)) == 0) {
    /* Has preamble: the RCW follows the preamble and the Load RCW command word */
    rcw_data = binary + RCW_PBL_RCW_OFFSET;
    /* The PBI follows the checksum word */
    size_t pbi_start = RCW_PBL_PBI_OFFSET;
    if (pbi_start < len) {
      pbi_data = binary + pbi_start;
      pbi_len = len - pbi_start;
    }

  } else {
    /*
     * No preamble: the whole buffer is the raw 128-byte RCW. Anything
     * shorter cannot be decoded - rcw_bits_extract reads exactly
     * RCW_SIZE_BYTES from rcw_data.
     */
    if (len < RCW_SIZE_BYTES) {
      free(sb.data);
      return RCW_ERR_BAD_BINARY;
    }
    rcw_data = binary;
  }


  /* Extract field values from RCW bits */
  for (size_t i = 0; i < ctx->symbols.count; i++) {
    const rcw_symbol_t *sym = &ctx->symbols.entries[i];

    uint64_t v = rcw_bits_extract(rcw_data, sym->begin, sym->end, ctx->vars.classicbitnumbers);

    if (v != 0) {
      int b = (sym->begin < sym->end) ? sym->begin : sym->end;
      int e = (sym->begin < sym->end) ? sym->end : sym->begin;
      int s = 1 + e - b;

      if (s > RCW_DECOMPILE_HEX_THRESHOLD)
        strbuf_printf(&sb, "%s=0x%lx\n", sym->name, (unsigned long)v);
      else
        strbuf_printf(&sb, "%s=%lu\n", sym->name, (unsigned long)v);
    }

  }

  /* Disassemble PBI if present */
  if (pbi_data && pbi_len > 0) {
    strbuf_printf(&sb, "\n.pbi\n");
    err = disassemble_pbi(&sb, pbi_data, pbi_len, ctx->vars.littleendian);
    if (err != RCW_OK) {
      free(sb.data);
      return err;
    }
    strbuf_printf(&sb, ".end\n");

  }

  *out_source = sb.data;
  *out_len = sb.len;

  return RCW_OK;
}
