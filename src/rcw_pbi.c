/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI command encoder for pbiformat=2.
 * Includes a minimal expression evaluator for preprocessed macro expansions.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "rcw_internal.h"

/*
 * Minimal recursive descent expression evaluator.
 *
 * Handles: integer literals (decimal, 0x hex, 0 octal), parentheses,
 * and operators: + - * / & | ^ << >> ~
 *
 * Needed because gcc -E expands macros to expressions like:
 *   (0x1ea0000 + (0x40 * (0)) + 0x1200)
 * which rcw.py handles via eval().
 */

typedef struct {
  const char *pos;
  const char *end;
  rcw_error_t err;
} eval_state_t;

static void
skip_ws(eval_state_t *st) {
  while (st->pos < st->end && isspace((unsigned char)*st->pos))
    st->pos++;
}

static uint32_t eval_expr(eval_state_t *st);

static uint32_t
eval_primary(eval_state_t *st) {
  skip_ws(st);

  if (st->pos >= st->end) {
    st->err = RCW_ERR_PBI_SYNTAX;
    return 0;
  }

  /* Parenthesized expression */
  if (*st->pos == '(') {
    st->pos++;
    uint32_t v = eval_expr(st);
    skip_ws(st);
    if (st->pos < st->end && *st->pos == ')')
      st->pos++;
    return v;
  }

  /* Unary minus */
  if (*st->pos == '-') {
    st->pos++;
    return (uint32_t)(-(int32_t)eval_primary(st));
  }

  /* Bitwise NOT */
  if (*st->pos == '~') {
    st->pos++;
    return ~eval_primary(st);
  }

  /* Number literal */
  if (isdigit((unsigned char)*st->pos)) {
    char *endptr;
    uint32_t v = (uint32_t)strtoul(st->pos, &endptr, 0);
    st->pos = endptr;
    return v;
  }

  st->err = RCW_ERR_PBI_SYNTAX;

  return 0;
}

static uint32_t
eval_mul(eval_state_t *st) {
  uint32_t v = eval_primary(st);

  for (;;) {
    skip_ws(st);
    if (st->pos >= st->end)
      break;
    if (*st->pos == '*') {
      st->pos++;
      v *= eval_primary(st);
    } else if (*st->pos == '/') {
      st->pos++;
      uint32_t d = eval_primary(st);
      if (d != 0)
        v /= d;
    } else {
      break;
    }
  }

  return v;
}

static uint32_t
eval_add(eval_state_t *st) {
  uint32_t v = eval_mul(st);

  for (;;) {
    skip_ws(st);
    if (st->pos >= st->end)
      break;
    if (*st->pos == '+') {
      st->pos++;
      v += eval_mul(st);
    } else if (*st->pos == '-') {
      st->pos++;
      v -= eval_mul(st);
    } else {
      break;
    }
  }

  return v;
}

static uint32_t
eval_shift(eval_state_t *st) {
  uint32_t v = eval_add(st);

  for (;;) {
    skip_ws(st);
    if (st->pos + 1 < st->end && st->pos[0] == '<' &&
        st->pos[1] == '<') {
      st->pos += 2;
      v <<= eval_add(st);
    } else if (st->pos + 1 < st->end && st->pos[0] == '>' &&
               st->pos[1] == '>') {
      st->pos += 2;
      v >>= eval_add(st);
    } else {
      break;
    }
  }

  return v;
}

static uint32_t
eval_bitand(eval_state_t *st) {
  uint32_t v = eval_shift(st);

  for (;;) {
    skip_ws(st);
    if (st->pos < st->end && *st->pos == '&' &&
        (st->pos + 1 >= st->end || st->pos[1] != '&')) {
      st->pos++;
      v &= eval_shift(st);
    } else {
      break;
    }
  }

  return v;
}

static uint32_t
eval_bitxor(eval_state_t *st) {
  uint32_t v = eval_bitand(st);

  for (;;) {
    skip_ws(st);
    if (st->pos < st->end && *st->pos == '^') {
      st->pos++;
      v ^= eval_bitand(st);
    } else {
      break;
    }
  }

  return v;
}

static uint32_t
eval_expr(eval_state_t *st) {
  uint32_t v = eval_bitxor(st);

  for (;;) {
    skip_ws(st);
    if (st->pos < st->end && *st->pos == '|' &&
        (st->pos + 1 >= st->end || st->pos[1] != '|')) {
      st->pos++;
      v |= eval_bitxor(st);
    } else {
      break;
    }
  }

  return v;
}

rcw_error_t
rcw_eval_expr(const char *expr, uint32_t *result) {
  eval_state_t st = {
    .pos = expr,
    .end = expr + strlen(expr),
    .err = RCW_OK,
  };

  *result = eval_expr(&st);

  return st.err;
}

/*
 * Pack a 32-bit word in the current endianness and append to PBI buffer.
 */
static rcw_error_t
pbi_pack_word(rcw_ctx_t *ctx, uint32_t val) {
  uint8_t buf[4];

  if (ctx->vars.littleendian) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
  } else {
    buf[0] = (uint8_t)((val >> 24) & 0xFF);
    buf[1] = (uint8_t)((val >> 16) & 0xFF);
    buf[2] = (uint8_t)((val >> 8) & 0xFF);
    buf[3] = (uint8_t)(val & 0xFF);
  }

  return rcw_pbi_append(&ctx->pbi, buf, 4);
}

/*
 * Parse a PBI command line and encode it.
 * Format: op[.suffix] param1[,param2[,...]]
 */
rcw_error_t
rcw_pbi_encode_line(rcw_ctx_t *ctx, const char *line) {
  char op[32] = {0};
  char suffix[16] = {0};
  char params_str[1024];
  uint32_t params[PBI_MAX_PARAMS];
  int nparam = 0;

  /* Parse "op" or "op.suffix" */
  const char *p = line;
  while (*p && isalpha((unsigned char)*p))
    p++;

  size_t oplen = (size_t)(p - line);
  if (oplen == 0 || oplen >= sizeof(op)) {
    rcw_set_error(ctx, "Invalid PBI command: %s", line);
    return RCW_ERR_PBI_SYNTAX;
  }
  memcpy(op, line, oplen);

  /* Parse optional suffix (.b1, .b2, .b4, .b5, .short, .long) */
  if (*p == '.') {
    p++; /* skip dot */
    const char *suf_start = p;
    while (*p && (isalnum((unsigned char)*p)))
      p++;
    size_t suflen = (size_t)(p - suf_start);
    if (suflen >= sizeof(suffix)) {
      rcw_set_error(ctx, "Invalid PBI suffix: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    memcpy(suffix, suf_start, suflen);
  }

  /* Skip whitespace before parameters */
  while (*p && isspace((unsigned char)*p))
    p++;

  /* Parse comma-separated parameters */
  if (*p) {
    snprintf(params_str, sizeof(params_str), "%s", p);
    char *tok = params_str;

    while (tok && *tok && nparam < PBI_MAX_PARAMS) {
      char *comma = NULL;
      int depth = 0;

      /* Find comma not inside parentheses */
      for (char *c = tok; *c; c++) {
        if (*c == '(')
          depth++;
        else if (*c == ')')
          depth--;
        else if (*c == ',' && depth == 0) {
          comma = c;
          break;
        }
      }
      if (comma)
        *comma = '\0';

      /* Evaluate expression */
      rcw_error_t err = rcw_eval_expr(tok, &params[nparam]);
      if (err != RCW_OK) {
        rcw_set_error(ctx,
            "Cannot evaluate PBI parameter '%s' in: %s",
            tok, line);
        return err;
      }
      nparam++;

      tok = comma ? comma + 1 : NULL;
    }
  }

  /* Encode each command (pbiformat=2 only) */
  rcw_error_t err;

  if (strcmp(op, "write") == 0) {
    if (nparam < 2) {
      rcw_set_error(ctx, "\"write\" requires 2 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    uint32_t opsizebytes = PBI_WRITE_4BYTE;
    if (strcmp(suffix, "b1") == 0)
      opsizebytes = PBI_WRITE_1BYTE;

    err = pbi_pack_word(ctx, (opsizebytes << PBI_WRITE_SHIFT) | params[0]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[1]);

  } else if (strcmp(op, "awrite") == 0) {
    if (strcmp(suffix, "b5") == 0) {
      if (nparam < 5) {
        rcw_set_error(ctx,
            "\"awrite.b5\" requires 5 parameters: %s", line);
        return RCW_ERR_PBI_SYNTAX;
      }
      err = pbi_pack_word(ctx,
          PBL_CMD_GENERAL |
          ((uint32_t)PBI_AWRITE_B5 << PBI_AWRITE_B_SHIFT) |
          params[0]);
      if (err != RCW_OK)
        return err;
      for (int i = 1; i < 5; i++) {
        err = pbi_pack_word(ctx, params[i]);
        if (err != RCW_OK)
          return err;
      }
      return RCW_OK;

    } else if (strcmp(suffix, "b4") == 0) {
      if (nparam < 3) {
        rcw_set_error(ctx,
            "\"awrite.b4\" requires 3 parameters: %s", line);
        return RCW_ERR_PBI_SYNTAX;
      }
      err = pbi_pack_word(ctx,
          PBL_CMD_GENERAL |
          ((uint32_t)PBI_AWRITE_B4 << PBI_AWRITE_B_SHIFT) |
          params[0]);
      if (err != RCW_OK)
        return err;
      err = pbi_pack_word(ctx, params[1]);
      if (err != RCW_OK)
        return err;
      return pbi_pack_word(ctx, params[2]);

    } else {
      /* Default awrite (B=3) */
      if (nparam < 2) {
        rcw_set_error(ctx,
            "\"awrite\" requires 2 parameters: %s", line);
        return RCW_ERR_PBI_SYNTAX;
      }
      err = pbi_pack_word(ctx,
          PBL_CMD_GENERAL |
          ((uint32_t)PBI_AWRITE_B3 << PBI_AWRITE_B_SHIFT) |
          params[0]);
      if (err != RCW_OK)
        return err;
      return pbi_pack_word(ctx, params[1]);
    }

  } else if (strcmp(op, "wait") == 0) {
    if (nparam < 1) {
      rcw_set_error(ctx, "\"wait\" requires 1 parameter: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    return pbi_pack_word(ctx, PBL_CMD_WAIT | params[0]);

  } else if (strcmp(op, "poll") == 0) {
    if (nparam < 3) {
      rcw_set_error(ctx, "\"poll\" requires 3 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    uint32_t cmd = (strcmp(suffix, "long") == 0)
        ? PBI_GENCMD_POLL_LONG : PBI_GENCMD_POLL_SHORT;
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (cmd << PBI_CMD_SHIFT) | params[0]);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[1]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[2]);

  } else if (strcmp(op, "blockcopy") == 0) {
    if (nparam < 4) {
      rcw_set_error(ctx,
          "\"blockcopy\" requires 4 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx, PBL_CMD_BLOCKCOPY | (params[0] & 0xFF));
    if (err != RCW_OK)
      return err;
    for (int i = 1; i < 4; i++) {
      err = pbi_pack_word(ctx, params[i]);
      if (err != RCW_OK)
        return err;
    }
    return RCW_OK;

  } else if (strcmp(op, "loadacwindow") == 0) {
    if (nparam < 1) {
      rcw_set_error(ctx,
          "\"loadacwindow\" requires 1 parameter: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    return pbi_pack_word(ctx, PBL_CMD_LOADACWINDOW | params[0]);

  } else if (strcmp(op, "flush") == 0) {
    /* flush uses pbladdr for old format; for pbiformat=2 it
     * shouldn't appear, but encode it anyway for compatibility */
    err = pbi_pack_word(ctx,
        PBL_CMD_FLUSH | (ctx->vars.pbladdr & RCW_PBLADDR_MASK));
    if (err != RCW_OK)
      return err;

    return pbi_pack_word(ctx, 0);
  }

  rcw_set_error(ctx, "Unknown PBI command: %s", line);

  return RCW_ERR_PBI_SYNTAX;
}
