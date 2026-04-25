/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * .uboot subsection encoder.
 *
 * Mirrors rcw.py build_pbi_uboot() (lines 219-272). Generates PBI
 * commands from an xxd hex dump of u-boot.bin for SPI/SD/NAND boot
 * chains (refer: Chapter 10, u-boot of QorIQ_SDK_Infocenter.pdf).
 *
 * No upstream pbiformat=2 board (LS1028, LS1088, LS2088, LX2160) uses
 * .uboot subsections, so this code path is provided for completeness
 * and parity with rcw.py - it is not exercised by the cross-validation
 * test suite.
 *
 * Input format (one xxd line, after `xxd u-boot.bin | cut -d ' ' -f1-10`):
 *
 *     ADDRESS: WORD WORD WORD WORD WORD WORD WORD WORD
 *
 * where ADDRESS is hex with a trailing ':' and each WORD is a 4-character
 * hex value representing a 16-bit big-endian word (16 bytes per line).
 *
 * Grouping rules:
 *   - The last 4 lines are emitted as 2 pairs (32 bytes each) prefixed
 *     by RCW_UBOOT_PAIR_BASE + (line_addr - 0x10).
 *   - All earlier lines are emitted in groups of 4 (64 bytes each)
 *     prefixed by RCW_UBOOT_QUAD_BASE + (line_addr - 0x30).
 *
 * Output is always big-endian regardless of %littleendian.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rcw_internal.h"

/* Pack a 32-bit big-endian word and append to PBI buffer. */
static rcw_error_t
ub_pack_be32(rcw_ctx_t *ctx, uint32_t v) {
  uint8_t buf[4];

  buf[0] = (uint8_t)((v >> 24) & 0xFF);
  buf[1] = (uint8_t)((v >> 16) & 0xFF);
  buf[2] = (uint8_t)((v >>  8) & 0xFF);
  buf[3] = (uint8_t)( v        & 0xFF);

  return rcw_pbi_append(&ctx->pbi, buf, 4);
}

/* Pack a 16-bit big-endian word and append to PBI buffer. */
static rcw_error_t
ub_pack_be16(rcw_ctx_t *ctx, uint16_t v) {
  uint8_t buf[2];

  buf[0] = (uint8_t)((v >> 8) & 0xFF);
  buf[1] = (uint8_t)( v       & 0xFF);

  return rcw_pbi_append(&ctx->pbi, buf, 2);
}

/*
 * Parse one xxd-style line: hex address followed by ':' followed by
 * RCW_UBOOT_WORDS_PER_LINE 16-bit hex words separated by whitespace.
 */
static rcw_error_t
parse_xxd_line(const char *line, uint32_t *addr, uint16_t words[RCW_UBOOT_WORDS_PER_LINE]) {
  char *endp;

  unsigned long a = strtoul(line, &endp, 16);
  if (endp == line || *endp != ':')
    return RCW_ERR_PBI_SYNTAX;

  *addr = (uint32_t)a;
  endp++; /* skip ':' */

  for (int i = 0; i < RCW_UBOOT_WORDS_PER_LINE; i++) {
    while (*endp == ' ' || *endp == '\t')
      endp++;

    char *next;
    unsigned long w = strtoul(endp, &next, 16);
    if (next == endp)
      return RCW_ERR_PBI_SYNTAX;

    words[i] = (uint16_t)w;
    endp = next;
  }

  return RCW_OK;
}

/*
 * Count non-blank lines in a body.
 * Needed for a first pass: the grouping rule depends on whether the
 * current line is in the last RCW_UBOOT_TAIL_LINES of the input.
 */
static size_t
count_lines(const char *body, size_t len) {
  size_t n = 0;
  const char *p = body;
  const char *end = body + len;

  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);

    bool blank = true;
    for (size_t i = 0; i < llen; i++) {
      unsigned char c = (unsigned char)p[i];
      /* Treat NUL as blank too - some callers pass C-string buffers
       * including the trailing NUL by accident, and a NUL byte in the
       * middle of a buffer can never be valid xxd input anyway. */
      if (c != '\0' && !isspace(c)) {
        blank = false;
        break;
      }
    }
    if (!blank)
      n++;

    p = eol ? eol + 1 : end;
  }

  return n;
}

rcw_error_t
rcw_uboot_encode(rcw_ctx_t *ctx, const char *body, size_t len) {
  size_t nlines = count_lines(body, len);
  if (nlines == 0)
    return RCW_OK;

  /*
   * Accumulator for pending words. Sized for the larger of the two
   * block types (quad block = 32 16-bit words).
   */
  uint16_t buf[RCW_UBOOT_QUAD_LINES * RCW_UBOOT_WORDS_PER_LINE];
  size_t buf_cnt = 0;
  size_t cnt = 0; /* 1-indexed line counter, matches rcw.py */

  const char *p = body;
  const char *end = body + len;

  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    size_t llen = eol ? (size_t)(eol - p) : (size_t)(end - p);

    /* Skip blank lines */
    bool blank = true;
    for (size_t i = 0; i < llen; i++) {
      unsigned char c = (unsigned char)p[i];
      /* Treat NUL as blank too - some callers pass C-string buffers
       * including the trailing NUL by accident, and a NUL byte in the
       * middle of a buffer can never be valid xxd input anyway. */
      if (c != '\0' && !isspace(c)) {
        blank = false;
        break;
      }
    }
    if (blank) {
      p = eol ? eol + 1 : end;
      continue;
    }

    /* Copy the line into a NUL-terminated buffer for parsing */
    if (llen >= RCW_UBOOT_LINE_MAX) {
      rcw_set_error(ctx, ".uboot line too long (%zu bytes)", llen);
      return RCW_ERR_PBI_SYNTAX;
    }
    char line[RCW_UBOOT_LINE_MAX];
    memcpy(line, p, llen);
    line[llen] = '\0';

    uint32_t addr;
    uint16_t words[RCW_UBOOT_WORDS_PER_LINE];
    rcw_error_t err = parse_xxd_line(line, &addr, words);
    if (err != RCW_OK) {
      rcw_set_error(ctx, "Bad .uboot xxd line: %s", line);
      return err;
    }

    cnt++;

    /* Append this line's words to the accumulator */
    for (int i = 0; i < RCW_UBOOT_WORDS_PER_LINE; i++)
      buf[buf_cnt++] = words[i];

    /*
     * Decide whether this line completes a block.
     * Matches rcw.py:
     *   if (cnt % 2 == 0) and (cnt > len(lines) - 4): # last 4 lines
     *       emit 32-byte pair
     *   elif (cnt % 4 == 0):
     *       emit 64-byte quad
     */
    bool last_tail = (nlines >= RCW_UBOOT_TAIL_LINES) &&
                     (cnt > nlines - RCW_UBOOT_TAIL_LINES);

    if (last_tail && (cnt % RCW_UBOOT_PAIR_LINES == 0)) {
      if (addr < 0x10u) {
        rcw_set_error(ctx,
            ".uboot pair-line address 0x%x below 0x10 "
            "(would underflow header base)", addr);
        return RCW_ERR_PBI_SYNTAX;
      }
      uint32_t hdr = RCW_UBOOT_PAIR_BASE + (addr - 0x10);
      err = ub_pack_be32(ctx, hdr);
      if (err != RCW_OK)
        return err;

      size_t nwords = RCW_UBOOT_PAIR_LINES * RCW_UBOOT_WORDS_PER_LINE;
      for (size_t i = 0; i < nwords; i++) {
        err = ub_pack_be16(ctx, buf[i]);
        if (err != RCW_OK)
          return err;
      }
      buf_cnt = 0;

    } else if (cnt % RCW_UBOOT_QUAD_LINES == 0) {
      if (addr < 0x30u) {
        rcw_set_error(ctx,
            ".uboot quad-line address 0x%x below 0x30 "
            "(would underflow header base)", addr);
        return RCW_ERR_PBI_SYNTAX;
      }
      uint32_t hdr = RCW_UBOOT_QUAD_BASE + (addr - 0x30);
      err = ub_pack_be32(ctx, hdr);
      if (err != RCW_OK)
        return err;

      size_t nwords = RCW_UBOOT_QUAD_LINES * RCW_UBOOT_WORDS_PER_LINE;
      for (size_t i = 0; i < nwords; i++) {
        err = ub_pack_be16(ctx, buf[i]);
        if (err != RCW_OK)
          return err;
      }
      buf_cnt = 0;
    }

    p = eol ? eol + 1 : end;
  }

  /*
   * If anything remains in the accumulator, the input did not end on
   * a block boundary. rcw.py silently drops the trailing partial
   * group; do the same but emit a debug-style error detail.
   */
  if (buf_cnt > 0) {
    rcw_set_error(ctx, ".uboot input has %zu line(s) past the last full block", buf_cnt / RCW_UBOOT_WORDS_PER_LINE);
    /* Not an error - rcw.py tolerates this silently */
  }

  return RCW_OK;
}
