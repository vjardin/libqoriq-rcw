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
 *
 * All bounds checks compare against the real pbi_len (not a rounded-
 * up version). Trailing bytes that don't form a full 4-byte word are
 * silently ignored - rather than read out-of-bounds.
 */
static rcw_error_t
disassemble_pbi(strbuf_t *sb, const uint8_t *pbi,
                size_t pbi_len, unpack_fn unpack) {
  size_t i = 0;

  while (i + 4 <= pbi_len) {
    uint32_t word = unpack(pbi + i);
    i += 4;

    uint8_t hdr = (uint8_t)((word & PBI_HDR_MASK) >> PBI_HDR_SHIFT);

    if (hdr == PBI_HDR_GENERAL) {
      /* General command */
      uint8_t cmd = (uint8_t)((word & PBI_CMD_MASK) >> PBI_CMD_SHIFT);

      if (cmd == PBI_GENCMD_BLOCKCOPY && i + 12 <= pbi_len) {
        /* blockcopy */
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "blockcopy 0x%02x,0x%08x,0x%08x,0x%08x\n", word & PBI_BLOCKCOPY_SRC_MAX, a1, a2, a3);

      } else if (cmd == PBI_GENCMD_LOADACWINDOW) {
        strbuf_printf(sb, "loadacwindow 0x%08x\n", word & PBI_ACWINDOW_MAX);

      } else if (cmd == PBI_GENCMD_LOADCONDITION && i + 8 <= pbi_len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "loadcondition 0x%08x,0x%08x\n", a1, a2);

      } else if (cmd == PBI_GENCMD_POLL_SHORT && i + 12 <= pbi_len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "poll.short 0x%08x,0x%08x,0x%08x\n", a1, a2, a3);

      } else if (cmd == PBI_GENCMD_POLL_LONG && i + 12 <= pbi_len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        uint32_t a2 = unpack(pbi + i); i += 4;
        uint32_t a3 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "poll.long 0x%08x,0x%08x,0x%08x\n", a1, a2, a3);

      } else if (cmd == PBI_GENCMD_WAIT) {
        strbuf_printf(sb, "wait 0x%08x\n", word & PBI_PARAM_MASK);

      } else if (cmd == PBI_GENCMD_CRC_STOP && i + 4 <= pbi_len) {
        uint32_t crc = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "/* CRC and Stop command (CRC 0x%08x)*/\n", crc);

      } else if (cmd == PBI_GENCMD_STOP) {
        if (i + 4 <= pbi_len)
          i += 4;
        strbuf_printf(sb, "/* Stop command */\n");

      } else if (cmd == PBI_GENCMD_CCSR_COPY && i + 12 <= pbi_len) {
        /* RM Table 37: 16 B, word type in the low 2 bits of the header. */
        static const char *wsuf[4] = { "?", "b1", "b2", "b4" };
        uint32_t wt   = word & PBI_CCSR_COPY_TYPE_MASK;
        uint32_t src  = unpack(pbi + i); i += 4;
        uint32_t dst  = unpack(pbi + i); i += 4;
        uint32_t nw   = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "ccsrcopy.%s 0x%08x,0x%08x,0x%08x\n",
                      wsuf[wt], src, dst, nw);

      } else if (cmd == PBI_GENCMD_CCSR_RMW && i + 12 <= pbi_len) {
        /*
         * RM Table 43: 16 B, header low byte = ops[3:2] | masktype[1:0].
         * Emitted as 0x42 per Table 27; Table 43's own "CMD = 0x02" is a
         * documentation error (0x02 is Table 37's opcode).
         */
        static const char *msuf[4] = { "?", "b1", "b2", "b4" };
        static const char *opnm[4] = { "ccsrset", "ccsrclear",
                                       "ccsrreplace", "ccsrreserved" };
        uint32_t mt   = word & PBI_RMW_MASK_TYPE_MASK;
        uint32_t ops  = (word >> PBI_RMW_OPS_SHIFT) & PBI_RMW_OPS_MASK;
        uint32_t addr = unpack(pbi + i); i += 4;
        uint32_t mask = unpack(pbi + i); i += 4;
        uint32_t data = unpack(pbi + i); i += 4;
        if (ops == PBI_RMW_OPS_REPLACE)
          strbuf_printf(sb, "%s.%s 0x%08x,0x%08x,0x%08x\n",
                        opnm[ops], msuf[mt], addr, mask, data);
        else
          strbuf_printf(sb, "%s.%s 0x%08x,0x%08x\n",
                        opnm[ops], msuf[mt], addr, mask);

      } else if (cmd == PBI_GENCMD_JUMP && i + 4 <= pbi_len) {
        /* RM Table 46: 8 bytes, one OFFSET word. Forward only. */
        uint32_t off = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "jump 0x%08x\n", off);

      } else if (cmd == PBI_GENCMD_JUMP_COND && i + 8 <= pbi_len) {
        /* RM Table 47: 12 bytes, OFFSET then CONDITION. Forward only. */
        uint32_t off  = unpack(pbi + i); i += 4;
        uint32_t cond = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "jumpif 0x%08x,0x%08x\n", off, cond);

      } else if (cmd == PBI_GENCMD_LOAD_SEC_HDR &&
                 i + 4 * PBI_CSF_HEADER_WORDS <= pbi_len) {
        /*
         * RM Table 41: 84 B, 20 CSF words. Always emitted in the inline
         * form -- the source may have used a file, but the binary does not
         * record which, and inline is what round-trips.
         */
        strbuf_printf(sb, "loadsecurityheader ");
        for (size_t k = 0; k < PBI_CSF_HEADER_WORDS; k++) {
          uint32_t w = unpack(pbi + i); i += 4;
          strbuf_printf(sb, "%s0x%08x",
                        (k == 0) ? "" : ",", w);
        }
        strbuf_printf(sb, "\n");

      } else if (cmd == PBI_GENCMD_LOAD_CSF_PTR && i + 4 <= pbi_len) {
        /* RM Table 42: 8 B, one 4-byte-aligned pointer. */
        uint32_t ptr = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "loadboot1csfptr 0x%08x\n", ptr);

      } else if (cmd == PBI_GENCMD_LOAD_RCW_CS ||
                 cmd == PBI_GENCMD_LOAD_RCW_NOCS) {
        strbuf_printf(sb, "/* Disassemble not implemented for word 0x%08x */\n", word);

      } else {
        strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

      }

    } else if ((hdr & (PBI_HDR_ACS | PBI_HDR_RSVD)) == 0) {
      /* CCSR write */
      uint8_t cmd = (hdr & PBI_CCSR_TYPE_MASK) >> PBI_CCSR_TYPE_SHIFT;
      if (cmd == PBI_CCSR_WRITE_B1 && i + 4 <= pbi_len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "write.b1 0x%08x,0x%08x\n", word & PBI_CCSR_ADDR_MASK, a1);

      } else if (cmd == PBI_CCSR_WRITE_B4 && i + 4 <= pbi_len) {
        uint32_t a1 = unpack(pbi + i); i += 4;
        strbuf_printf(sb, "write 0x%08x,0x%08x\n", word & PBI_CCSR_ADDR_MASK, a1);

      } else {
        strbuf_printf(sb, "/* Unknown word 0x%08x */\n", word);

      }

    } else if ((hdr & (PBI_HDR_ACS | PBI_HDR_RSVD)) == PBI_HDR_ACS) {
      /* Altconfig write */
      uint8_t bcnt = (hdr & PBI_AWRITE_B_MASK) >> PBI_AWRITE_B_RSHIFT;
      if (bcnt) {
        /*
         * The payload width lives in the mnemonic suffix, not in the
         * argument count: the parser dispatches on "awrite" (B=3, one
         * data word), "awrite.b4" (two) and "awrite.b5" (four). Emit
         * the matching spelling -- writing a bare "awrite" for a wide
         * command produces source that still *lists* every data word
         * but recompiles to B=3, silently dropping all but the first
         * and shrinking the PBI (measured: 992 bytes lost over one
         * nbxv3 image, which then boots differently).
         *
         * The hardware allows B=1..10 (Section 8.3.2.2); we can only
         * round-trip what rcw_pbi.c can parse back. Anything else is
         * flagged rather than mis-emitted.
         */
        const char *mnemonic = NULL;
        switch (bcnt) {
        case PBI_AWRITE_B3: mnemonic = "awrite";    break;
        case PBI_AWRITE_B4: mnemonic = "awrite.b4"; break;
        case PBI_AWRITE_B5: mnemonic = "awrite.b5"; break;
        default: break;
        }

        if (!mnemonic) {
          strbuf_printf(sb,
              "/* Unsupported awrite payload width B=%u (%u bytes) at "
              "PBI offset %zu -- not representable in .rcw source */\n",
              bcnt, 1u << (bcnt - 1), (size_t)(i - 4));
          /* Skip the payload so the stream stays in sync. */
          for (uint32_t j = 0;
               j < (1u << (bcnt - 1)) && i + 4 <= pbi_len;
               j += 4)
            i += 4;
        } else {
          strbuf_printf(sb, "%s 0x%08x", mnemonic,
                        word & PBI_AWRITE_ADDR_MASK);
          for (uint32_t j = 0;
               j < (1u << (bcnt - 1)) && i + 4 <= pbi_len;
               j += 4) {
            uint32_t a1 = unpack(pbi + i);
            i += 4;
            strbuf_printf(sb, ",0x%08x", a1);
          }
          strbuf_printf(sb, "\n");
        }

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
