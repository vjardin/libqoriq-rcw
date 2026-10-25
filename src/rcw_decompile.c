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

static size_t
pbi_step(const uint8_t *pbi, size_t pbi_len, size_t i, int littleendian,
         rcw_pbi_cmd_t *c, rcw_error_t *err) {
  *err = rcw_pbi_decode(pbi + i, pbi_len - i, littleendian, c);
  if (*err != RCW_OK)
    return PBI_WORD_BYTES;
  return c->size;
}

static long
jump_target(size_t i, uint32_t offset, size_t pbi_len) {
  uint64_t t = (uint64_t)i + offset;

  if (t >= pbi_len || t % PBI_WORD_BYTES != 0)
    return -1;
  return (long)(t / PBI_WORD_BYTES);
}

static int *
collect_labels(const uint8_t *pbi, size_t pbi_len, int littleendian) {
  size_t nwords = pbi_len / PBI_WORD_BYTES;
  if (nwords == 0)
    return NULL;

  int *label_of = malloc(nwords * sizeof(*label_of));
  uint8_t *starts = calloc(nwords, 1);
  if (!label_of || !starts) {
    free(label_of);
    free(starts);
    return NULL;
  }
  for (size_t w = 0; w < nwords; w++)
    label_of[w] = -1;

  for (size_t i = 0; i + PBI_WORD_BYTES <= pbi_len;) {
    rcw_pbi_cmd_t c;
    rcw_error_t err;

    starts[i / PBI_WORD_BYTES] = 1;
    size_t step = pbi_step(pbi, pbi_len, i, littleendian, &c, &err);
    if (err == RCW_OK && (c.op == RCW_PBI_JUMP || c.op == RCW_PBI_JUMP_COND)) {
      long t = jump_target(i, c.args[0], pbi_len);
      if (t >= 0)
        label_of[t] = 0; /* a target; numbered below */
    }
    i += step;
  }

  int n = 0;
  for (size_t w = 0; w < nwords; w++)
    label_of[w] = (label_of[w] == 0 && starts[w]) ? n++ : -1;

  free(starts);
  return label_of;
}

static int
jump_label(const int *label_of, size_t pbi_len, size_t i, uint32_t offset) {
  long t = jump_target(i, offset, pbi_len);

  return (label_of && t >= 0) ? label_of[t] : -1;
}

static rcw_error_t
disassemble_pbi(strbuf_t *sb, const uint8_t *pbi, size_t pbi_len,
                int littleendian) {
  int *label_of = collect_labels(pbi, pbi_len, littleendian);
  size_t i = 0;

  while (i + PBI_WORD_BYTES <= pbi_len) {
    rcw_pbi_cmd_t c;
    rcw_error_t err;
    size_t step = pbi_step(pbi, pbi_len, i, littleendian, &c, &err);

    if (label_of && label_of[i / PBI_WORD_BYTES] >= 0)
      strbuf_printf(sb, "L%d:\n", label_of[i / PBI_WORD_BYTES]);

    if (err == RCW_ERR_TRUNCATED && c.op == RCW_PBI_STOP) {
      strbuf_printf(sb, "/* Stop command */\n");
      i += step;
      continue;
    }
    if (err != RCW_OK) {
      strbuf_printf(sb, "/* Unknown word 0x%08x */\n", c.word);
      i += step;
      continue;
    }

    int label = (c.op == RCW_PBI_JUMP || c.op == RCW_PBI_JUMP_COND)
                ? jump_label(label_of, pbi_len, i, c.args[0]) : -1;
    if (label >= 0) {
      if (c.op == RCW_PBI_JUMP)
        strbuf_printf(sb, "jump L%d\n", label);
      else
        strbuf_printf(sb, "jumpc L%d,0x%08x\n", label, c.args[1]);
    } else {
      char line[RCW_PBI_SOURCE_MAX];

      rcw_pbi_to_source(&c, line, sizeof(line));
      strbuf_printf(sb, "%s\n", line);
    }

    i += step;
  }

  free(label_of);
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
  uint8_t covered[RCW_SIZE_BITS / 8] = {0};
  for (size_t i = 0; i < ctx->symbols.count; i++) {
    const rcw_symbol_t *sym = &ctx->symbols.entries[i];

    uint64_t v = rcw_bits_extract(rcw_data, sym->begin, sym->end, ctx->vars.classicbitnumbers);

    int b = (sym->begin < sym->end) ? sym->begin : sym->end;
    int e = (sym->begin < sym->end) ? sym->end : sym->begin;
    int s = 1 + e - b;

    for (int k = b; k <= e; k++)
      covered[k / 8] |= (uint8_t)(1u << (k % 8));

    if (v != 0) {
      if (s > RCW_DECOMPILE_HEX_THRESHOLD)
        strbuf_printf(&sb, "%s=0x%lx\n", sym->name, (unsigned long)v);
      else
        strbuf_printf(&sb, "%s=%lu\n", sym->name, (unsigned long)v);
    }

  }

  bool unknown = false;
  for (int k = 0; k < RCW_SIZE_BITS; k++) {
    if (covered[k / 8] & (1u << (k % 8)))
      continue;
    if (rcw_bits_extract(rcw_data, k, k, ctx->vars.classicbitnumbers) == 0)
      continue;
    strbuf_printf(&sb, "%s %d", unknown ? "" : "/* Unknown bits in positions:", k);
    unknown = true;
  }
  if (unknown)
    strbuf_printf(&sb, " */\n");

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
