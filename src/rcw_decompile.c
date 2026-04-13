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

static uint32_t
read_le32(const uint8_t *p) {
  return (uint32_t)p[0]
      | ((uint32_t)p[1] << 8)
      | ((uint32_t)p[2] << 16)
      | ((uint32_t)p[3] << 24);
}

static uint32_t
read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24)
       | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8)
       | (uint32_t)p[3];
}

typedef uint32_t (*unpack_fn)(const uint8_t *p);

/*
 * Disassemble PBI commands in a binary PBI section.
 * Matches rcw.py lines 909-1043 for pbiformat=2.
 */
static rcw_error_t
disassemble_pbi(strbuf_t *sb, const uint8_t *pbi,
                size_t pbi_len, unpack_fn unpack) {
  size_t i = 0;
  /* Pad to 4-byte boundary */
  size_t len = (pbi_len + 3) & ~(size_t)3;

  while (i < len) {
    if (i + 4 > pbi_len + 3)
      break;
    uint32_t word = unpack(pbi + i);
    i += 4;

    uint8_t hdr = (uint8_t)((word & PBI_HDR_MASK) >> PBI_HDR_SHIFT);

    if (hdr == 0x80) {
      /* General command */
      uint8_t cmd = (uint8_t)((word & PBI_CMD_MASK) >> PBI_CMD_SHIFT);

      if (cmd == PBI_GENCMD_BLOCKCOPY && i + 12 <= len) {
        /* blockcopy */
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "blockcopy 0x%02x,0x%08x,0x%08x,0x%08x\n", word & 0xFF, a1, a2, a3);

      } else if (cmd == PBI_GENCMD_LOADACWINDOW) {
        strbuf_printf(sb, "loadacwindow 0x%08x\n", word & 0x3FFF);

      } else if (cmd == PBI_GENCMD_LOADCONDITION && i + 8 <= len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "loadcondition 0x%08x,0x%08x\n", a1, a2);

      } else if (cmd == PBI_GENCMD_POLL_SHORT && i + 12 <= len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "poll.short 0x%08x,0x%08x,0x%08x\n", a1, a2, a3);

      } else if (cmd == PBI_GENCMD_POLL_LONG && i + 12 <= len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "poll.long 0x%08x,0x%08x,0x%08x\n", a1, a2, a3);

      } else if (cmd == PBI_GENCMD_WAIT) {
        strbuf_printf(sb, "wait 0x%08x\n", word & 0xFFFF);

      } else if (cmd == PBI_GENCMD_CRC_STOP && i + 4 <= len) {
        uint32_t crc = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "/* CRC and Stop command (CRC 0x%08x)*/\n", crc);

      } else if (cmd == PBI_GENCMD_STOP) {
        i += 4;
        strbuf_printf(sb, "/* Stop command */\n");

      } else if (cmd == PBI_GENCMD_LOAD_RCW_CS ||
                 cmd == PBI_GENCMD_LOAD_RCW_NOCS ||
                 cmd == PBI_GENCMD_LOAD_SEC_HDR ||
                 cmd == PBI_GENCMD_LOAD_CSF_PTR ||
                 cmd == PBI_GENCMD_JUMP ||
                 cmd == PBI_GENCMD_JUMP_COND) {
        strbuf_printf(sb, "/* Disassemble not implemented for word 0x%08x */\n", word);

      } else {
        strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

      }

    } else if ((hdr & 0xC0) == 0x00) {
      /* CCSR write */
      uint8_t cmd = (hdr & PBI_CCSR_TYPE_MASK) >> PBI_CCSR_TYPE_SHIFT;
      if (cmd == PBI_CCSR_WRITE_B1 && i + 4 <= len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "write.b1 0x%08x,0x%08x\n", word & PBI_CCSR_ADDR_MASK, a1);

      } else if (cmd == PBI_CCSR_WRITE_B4 && i + 4 <= len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "write 0x%08x,0x%08x\n", word & PBI_CCSR_ADDR_MASK, a1);

      } else {
        strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

      }

    } else if ((hdr & 0xC0) == 0x80) {
      /* Altconfig write */
      uint8_t bcnt = (hdr & PBI_AWRITE_B_MASK) >> PBI_AWRITE_B_RSHIFT;
      if (bcnt) {
        strbuf_printf(sb, "awrite 0x%08x", word & PBI_AWRITE_ADDR_MASK);
        for (uint32_t j = 0;
             j < (1u << (bcnt - 1)) && i + 4 <= len;
             j += 4) {
          uint32_t a1 = unpack(pbi + i);
          i += 4;
          strbuf_printf(sb, ",0x%08x", a1);
        }
        strbuf_printf(sb, "\n");

      } else {
        strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

      }
    } else {
      strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

    }
  }

  return RCW_OK;
}

rcw_error_t
rcw_decompile(rcw_ctx_t *ctx, const uint8_t *binary, size_t len, char **out_source, size_t *out_len) {
  unpack_fn unpack = ctx->vars.littleendian ? read_le32 : read_be32;

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

  if (len > RCW_SIZE_BYTES && memcmp(binary, preamble_bytes, 4) == 0) {
    /* Has preamble: skip 8 bytes (preamble + load RCW cmd) */
    rcw_data = binary + 8;
    /* Skip checksum (4 bytes after RCW) */
    size_t pbi_start = 8 + RCW_SIZE_BYTES + 4;
    if (pbi_start < len) {
      pbi_data = binary + pbi_start;
      pbi_len = len - pbi_start;
    }

  } else {
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
    err = disassemble_pbi(&sb, pbi_data, pbi_len, unpack);
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
