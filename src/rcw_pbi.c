/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * PBI command encoder for pbiformat=2.
 * Includes a minimal expression evaluator for preprocessed macro expansions.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "rcw_internal.h"

/* Operand expressions à la Python */
#define RCW_EVAL_MAX_DEPTH 64

typedef struct {
  const char *pos;
  const char *end;
  int depth;
  bool failed;
  char why[RCW_EVAL_WHY_MAX];
} eval_state_t;

__attribute__((format(printf, 2, 3)))
static void
eval_fail(eval_state_t *st, const char *fmt, ...) {
  va_list ap;

  if (st->failed)
    return; /* the first reason is the useful one */
  st->failed = true;
  va_start(ap, fmt);
  vsnprintf(st->why, sizeof(st->why), fmt, ap);
  va_end(ap);
}

static void
skip_ws(eval_state_t *st) {
  while (st->pos < st->end && isspace((unsigned char)*st->pos))
    st->pos++;
}

/* The operator at the cursor, if it is the one wanted, is consumed. */
static bool
eval_accept(eval_state_t *st, const char *op) {
  size_t n = strlen(op);

  skip_ws(st);
  if ((size_t)(st->end - st->pos) < n || memcmp(st->pos, op, n) != 0)
    return false;
  /* "<<" is not "<", "**" is not "*", "//" is not "/" */
  if (n == 1 && st->pos + 1 < st->end && st->pos[1] == op[0])
    return false;
  st->pos += n;
  return true;
}

static int64_t
eval_floordiv(eval_state_t *st, int64_t a, int64_t d) {
  if (d == 0) {
    eval_fail(st, "division by zero");
    return 0;
  }
  if (a == INT64_MIN && d == -1) {
    eval_fail(st, "value does not fit 64 bits");
    return 0;
  }
  int64_t q = a / d;
  if ((a % d) != 0 && ((a < 0) != (d < 0)))
    q--;
  return q;
}

static int64_t
eval_mod(eval_state_t *st, int64_t a, int64_t d) {
  if (d == 0) {
    eval_fail(st, "division by zero");
    return 0;
  }
  if (a == INT64_MIN && d == -1)
    return 0;
  int64_t r = a % d;
  if (r != 0 && ((r < 0) != (d < 0)))
    r += d;
  return r;
}

static int64_t
eval_pow(eval_state_t *st, int64_t base, int64_t exp) {
  int64_t r = 1;

  if (exp < 0) {
    eval_fail(st, "negative exponent"); /* a float in Python */
    return 0;
  }
  while (exp > 0) {
    if ((exp & 1) && __builtin_mul_overflow(r, base, &r)) {
      eval_fail(st, "value does not fit 64 bits");
      return 0;
    }
    exp >>= 1;
    if (exp > 0 && __builtin_mul_overflow(base, base, &base)) {
      eval_fail(st, "value does not fit 64 bits");
      return 0;
    }
  }
  return r;
}

static int64_t
eval_shl(eval_state_t *st, int64_t a, int64_t n) {
  int64_t r;

  if (n < 0) {
    eval_fail(st, "negative shift count");
    return 0;
  }
  if (a == 0)
    return 0;
  if (n >= 63 || __builtin_mul_overflow(a, (int64_t)1 << n, &r)) {
    eval_fail(st, "value does not fit 64 bits");
    return 0;
  }
  return r;
}

static int64_t
eval_shr(eval_state_t *st, int64_t a, int64_t n) {
  if (n < 0) {
    eval_fail(st, "negative shift count");
    return 0;
  }
  if (n >= 63)
    return a < 0 ? -1 : 0;
  return eval_floordiv(st, a, (int64_t)1 << n);
}

static int64_t eval_or(eval_state_t *st);
static int64_t eval_unary(eval_state_t *st);

static int64_t
eval_primary(eval_state_t *st) {
  int64_t v = 0;

  skip_ws(st);
  if (st->pos >= st->end) {
    eval_fail(st, "operand expected");
    return 0;
  }
  if (++st->depth > RCW_EVAL_MAX_DEPTH) {
    eval_fail(st, "nested too deep");
    --st->depth;
    return 0;
  }

  if (*st->pos == '(') {
    st->pos++;
    v = eval_or(st);
    skip_ws(st);
    if (st->pos < st->end && *st->pos == ')')
      st->pos++;
    else
      eval_fail(st, "')' expected");
  } else if (isdigit((unsigned char)*st->pos)) {
    const char *endp;
    uint64_t u;

    if (rcw_parse_literal(st->pos, &endp, &u) != RCW_OK || u > INT64_MAX)
      eval_fail(st, "bad number at '%.12s'", st->pos);
    else {
      st->pos = endp;
      v = (int64_t)u;
    }
  } else {
    eval_fail(st, "unexpected '%c'", *st->pos);
  }

  --st->depth;
  return v;
}

static int64_t
eval_power(eval_state_t *st) {
  int64_t v = eval_primary(st);

  if (!st->failed && eval_accept(st, "**"))
    v = eval_pow(st, v, eval_unary(st));
  return v;
}

static int64_t
eval_unary(eval_state_t *st) {
  skip_ws(st);
  if (st->pos < st->end && (*st->pos == '-' || *st->pos == '+' || *st->pos == '~')) {
    char op = *st->pos++;

    if (++st->depth > RCW_EVAL_MAX_DEPTH) {
      eval_fail(st, "nested too deep");
      --st->depth;
      return 0;
    }
    int64_t v = eval_unary(st);
    --st->depth;
    if (op == '-') {
      if (v == INT64_MIN) {
        eval_fail(st, "value does not fit 64 bits");
        return 0;
      }
      return -v;
    }
    if (op == '~')
      return ~v;
    return v;
  }
  return eval_power(st);
}

static int64_t
eval_term(eval_state_t *st) {
  int64_t v = eval_unary(st);

  while (!st->failed) {
    if (eval_accept(st, "*")) {
      if (__builtin_mul_overflow(v, eval_unary(st), &v))
        eval_fail(st, "value does not fit 64 bits");
    } else if (eval_accept(st, "//")) {
      v = eval_floordiv(st, v, eval_unary(st));
    } else if (eval_accept(st, "%")) {
      v = eval_mod(st, v, eval_unary(st));
    } else if (eval_accept(st, "/")) {
      eval_fail(st, "'/' is a float division in Python, use '//'");
    } else {
      break;
    }
  }
  return v;
}

static int64_t
eval_sum(eval_state_t *st) {
  int64_t v = eval_term(st);

  while (!st->failed) {
    if (eval_accept(st, "+")) {
      if (__builtin_add_overflow(v, eval_term(st), &v))
        eval_fail(st, "value does not fit 64 bits");
    } else if (eval_accept(st, "-")) {
      if (__builtin_sub_overflow(v, eval_term(st), &v))
        eval_fail(st, "value does not fit 64 bits");
    } else {
      break;
    }
  }
  return v;
}

static int64_t
eval_shift(eval_state_t *st) {
  int64_t v = eval_sum(st);

  while (!st->failed) {
    if (eval_accept(st, "<<"))
      v = eval_shl(st, v, eval_sum(st));
    else if (eval_accept(st, ">>"))
      v = eval_shr(st, v, eval_sum(st));
    else
      break;
  }
  return v;
}

static int64_t
eval_and(eval_state_t *st) {
  int64_t v = eval_shift(st);

  while (!st->failed && eval_accept(st, "&"))
    v &= eval_shift(st);
  return v;
}

static int64_t
eval_xor(eval_state_t *st) {
  int64_t v = eval_and(st);

  while (!st->failed && eval_accept(st, "^"))
    v ^= eval_and(st);
  return v;
}

static int64_t
eval_or(eval_state_t *st) {
  int64_t v = eval_xor(st);

  while (!st->failed && eval_accept(st, "|"))
    v |= eval_xor(st);
  return v;
}

rcw_error_t
rcw_eval_expr_why(const char *expr, uint32_t *result, char *why, size_t why_len) {
  eval_state_t st = {
    .pos = expr,
    .end = expr + strlen(expr),
  };

  int64_t v = eval_or(&st);

  if (!st.failed) {
    skip_ws(&st);
    if (st.pos < st.end)
      eval_fail(&st, "unexpected '%.12s'", st.pos);
  }
  if (!st.failed && (v < 0 || v > (int64_t)UINT32_MAX))
    eval_fail(&st, "%lld does not fit a 32-bit word", (long long)v);

  if (st.failed) {
    if (why && why_len)
      snprintf(why, why_len, "%s", st.why);
    return RCW_ERR_PBI_SYNTAX;
  }
  *result = (uint32_t)v;
  return RCW_OK;
}

rcw_error_t
rcw_eval_expr(const char *expr, uint32_t *result) {
  return rcw_eval_expr_why(expr, result, NULL, 0);
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
 * The CSF header can be given as a quoted file path instead of 20 inline
 * words. Detect and extract that before the numeric parameter parser runs,
 * since a path is not a number and would otherwise fail there.
 */
static bool
line_has_quoted_arg(const char *line) {
  return strchr(line, '"') != NULL;
}

static rcw_error_t
extract_quoted_arg(const char *line, char *out, size_t out_sz) {
  const char *a = strchr(line, '"');
  if (!a)
    return RCW_ERR_PBI_SYNTAX;
  const char *b = strchr(a + 1, '"');
  if (!b || (size_t)(b - a - 1) >= out_sz)
    return RCW_ERR_PBI_SYNTAX;
  memcpy(out, a + 1, (size_t)(b - a - 1));
  out[b - a - 1] = '\0';
  return RCW_OK;
}

/*
 * Parse a PBI command line and encode it.
 * Format: op[.suffix] param1[,param2[,...]]
 */
rcw_error_t
rcw_pbi_encode_line(rcw_ctx_t *ctx, const char *line) {
  char op[32] = {0};
  char suffix[16] = {0};
  uint32_t params[PBI_MAX_PARAMS];
  int nparam = 0;

  /* Parse "op" or "op.suffix" */
  const char *p = line;
  /*
   * isalnum, not isalpha: "loadboot1csfptr" (RM 8.3.9 "Load Boot 1 CSF
   * Header Pointer") carries a digit mid-mnemonic. Every pre-existing
   * mnemonic is all-alpha, so this cannot change how any of them tokenise;
   * the qoriq-rcw cross-validate suite proves that byte-for-byte across
   * every pbiformat=2 board.
   */
  while (*p && isalnum((unsigned char)*p))
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

  /*
   * Parse comma-separated parameters.
   *
   * Skipped entirely when the argument is a quoted string: the only such
   * command is loadsecurityheader with a CSF file path, and a path is not
   * a number, so the evaluator below would reject it before the encoder
   * ever sees it. The encoder re-reads the path off `line` itself. Any
   * other command given a quoted argument simply sees zero parameters and
   * fails with its own "requires N parameters" message.
   */
  /* Only two commands exceed the standard operand count. */
  const int param_cap = (strcmp(op, "awrite") == 0) ? PBI_AWRITE_MAX_PARAMS
      : (strcmp(op, "loadsecurityheader") == 0) ? (1 + PBI_CSF_HEADER_WORDS)
      : PBI_STD_MAX_PARAMS;

  if (*p && strchr(p, '"') == NULL) {
    char *params_str = strdup(p);
    if (!params_str)
      return RCW_ERR_NOMEM;
    char *tok = params_str;

    while (tok && *tok) {
      if (nparam >= param_cap) {
        rcw_set_error(ctx,
            "Too many PBI parameters (max %d): %s",
            param_cap, line);
        free(params_str);
        return RCW_ERR_PBI_SYNTAX;
      }

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
      char why[RCW_EVAL_WHY_MAX];
      rcw_error_t err = rcw_eval_expr_why(tok, &params[nparam], why, sizeof(why));
      if (err != RCW_OK) {
        rcw_set_error(ctx,
            "Cannot evaluate PBI parameter '%s': %s, in: %s",
            tok, why, line);
        free(params_str);
        return err;
      }
      nparam++;

      tok = comma ? comma + 1 : NULL;
    }
    free(params_str);
  }

  /* Encode each command (pbiformat=2 only) */
  rcw_error_t err;

  if (strcmp(op, "write") == 0) {
    if (nparam < 2) {
      rcw_set_error(ctx, "\"write\" requires 2 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] > PBI_CCSR_ADDR_MAX) {
      rcw_set_error(ctx,
          "\"write\" address 0x%x exceeds 28-bit CCSR field "
          "(max 0x%x); upper bits would corrupt the byte-count "
          "field: %s",
          params[0], PBI_CCSR_ADDR_MAX, line);
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
    uint32_t b = PBI_AWRITE_B3;
    if (suffix[0] != '\0') {
      uint64_t sb = 0;
      const char *endp = NULL;
      if (suffix[0] != 'b' ||
          rcw_parse_literal(suffix + 1, &endp, &sb) != RCW_OK || *endp != '\0' ||
          sb < PBI_AWRITE_B_MIN || sb > PBI_AWRITE_B_MAX) {
        rcw_set_error(ctx,
            "\"awrite.%s\": the width suffix is .b1 to .b%d "
            "(1 to 512 bytes, RM Table 35): %s",
            suffix, PBI_AWRITE_B_MAX, line);
        return RCW_ERR_PBI_SYNTAX;
      }
      b = (uint32_t)sb;
    }
    uint32_t nbytes = 1u << (b - 1);
    int nwords = (int)((nbytes + PBI_WORD_BYTES - 1) / PBI_WORD_BYTES);
    if (nparam != 1 + nwords) {
      rcw_set_error(ctx,
          "\"awrite.b%u\" writes %u byte%s: the offset then %d data word%s, "
          "got %d parameter%s: %s",
          b, nbytes, nbytes == 1 ? "" : "s", nwords, nwords == 1 ? "" : "s",
          nparam, nparam == 1 ? "" : "s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] > PBI_AWRITE_ADDR_MAX) {
      rcw_set_error(ctx,
          "\"awrite\" offset 0x%x exceeds 26-bit field (max 0x%x): %s",
          params[0], PBI_AWRITE_ADDR_MAX, line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (b > 2 && (params[0] % PBI_WORD_BYTES) != 0) {
      rcw_set_error(ctx,
          "\"awrite.b%u\" offset 0x%x must be word aligned for writes "
          "wider than 2 bytes (RM Table 35): %s",
          b, params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (b << PBI_AWRITE_B_SHIFT) | params[0]);
    if (err != RCW_OK)
      return err;
    for (int i = 1; i <= nwords; i++) {
      err = pbi_pack_word(ctx, params[i]);
      if (err != RCW_OK)
        return err;
    }
    return RCW_OK;

  } else if (strcmp(op, "wait") == 0) {
    if (nparam < 1) {
      rcw_set_error(ctx, "\"wait\" requires 1 parameter: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * The cycle count occupies bits [15:0] of the WAIT command word
     * (LX2160A RM Table 45 "Wait Command Format": HDR=0x80, CMD=0x82,
     * 16-bit operand). Larger values silently overflow into the CMD
     * field on both libqoriq-rcw and rcw.py, producing an opcode the
     * SP misinterprets (frequently it falls through to BOOTLOCPTR).
     * Reject up front so the misencoding cannot reach a flashed PBL.
     * Callers needing a longer delay should chain multiple waits.
     */
    if (params[0] > 0xFFFFu) {
      rcw_set_error(ctx,
          "\"wait\" operand 0x%x exceeds 16-bit field (max 0xFFFF); "
          "chain multiple waits instead: %s",
          params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    return pbi_pack_word(ctx, PBL_CMD_WAIT | params[0]);

  } else if (strcmp(op, "poll") == 0) {
    if (nparam < 3) {
      rcw_set_error(ctx, "\"poll\" requires 3 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * This intentionally diverges from rcw.py, which does the
     * same ORing and additionally computes 0x80000000 + (cmd << 24),
     * overflowing 32 bits for cmd=0x80, it cannot emit a valid poll
     * at all. No board in the upstream rcw/ tree uses poll, so the
     * byte-identity cross-validation never covered this path and is
     * unaffected by the correction.
     */
    uint32_t cmd = (strcmp(suffix, "long") == 0)
        ? PBI_GENCMD_POLL_LONG : PBI_GENCMD_POLL_SHORT;
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (cmd << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[0]);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[1]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[2]);

  } else if (strcmp(op, "loadsecurityheader") == 0) {
    /*
     * LX2160A RM Table 41 "Load Security Header", 84 bytes:
     *   word 0     HDR=0x80, CMD=0x20
     *   words 1-20 CSF_WORD_0 .. CSF_WORD_19
     *
     * Required for secure boot only; a non-secure PBI never emits it. The
     * 20 words are CSF header content produced by NXP's Code Signing Tool
     * -- their layout is in the QorIQ Trust Architecture UG, not the RM,
     * so they are carried verbatim and NOT interpreted here.
     *
     * Two source forms, both hex:
     *   loadsecurityheader 0xw0,0xw1,...,0xw19     (20 inline words)
     *   loadsecurityheader "csf_hdr.txt"           (hex text file)
     * The file form resolves like #include: as given, then each -I path.
     *
     * NEVER EXECUTED ON nbxv3 SILICON. This board boots non-secure (ITS
     * unburned, DBG_LVL=000), so the encoding is asserted against the RM
     * table in tests and nothing more. Treat a first secure boot as the
     * real validation.
     */
    uint32_t csf[PBI_CSF_HEADER_WORDS];

    if (line_has_quoted_arg(line)) {
      char path[RCW_PATH_MAX];
      if (extract_quoted_arg(line, path, sizeof(path)) != RCW_OK) {
        rcw_set_error(ctx,
            "\"loadsecurityheader\" file argument is not a closed "
            "quoted string: %s", line);
        return RCW_ERR_PBI_SYNTAX;
      }
      err = rcw_csf_read_words(ctx, path, csf, PBI_CSF_HEADER_WORDS);
      if (err != RCW_OK)
        return err;   /* rcw_csf_read_words already set a precise message */
    } else {
      if (nparam != (int)PBI_CSF_HEADER_WORDS) {
        rcw_set_error(ctx,
            "\"loadsecurityheader\" needs exactly %d hex words or a "
            "quoted file path, got %d: %s",
            (int)PBI_CSF_HEADER_WORDS, nparam, line);
        return RCW_ERR_PBI_SYNTAX;
      }
      for (size_t i = 0; i < PBI_CSF_HEADER_WORDS; i++)
        csf[i] = params[i];
    }

    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_LOAD_SEC_HDR << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    for (size_t i = 0; i < PBI_CSF_HEADER_WORDS; i++) {
      err = pbi_pack_word(ctx, csf[i]);
      if (err != RCW_OK)
        return err;
    }
    return RCW_OK;

  } else if (strcmp(op, "loadboot1csfptr") == 0) {
    if (nparam < 1) {
      rcw_set_error(ctx,
          "\"loadboot1csfptr\" requires 1 parameter (CSF pointer): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * LX2160A RM Table 42 "Load Boot 1 CSF Header Pointer", 8 bytes:
     *   word 0  HDR=0x80, CMD=0x22
     *   word 1  CSF_PTR -- must be 4-byte aligned (the RM's only check)
     *
     * Points at the CSF header authenticating the Boot 1 / SPL image.
     * Secure boot only, and likewise never executed on this board.
     */
    if (params[0] % 4u != 0u) {
      rcw_set_error(ctx,
          "\"loadboot1csfptr\" pointer 0x%x must be 4-byte aligned "
          "(RM Table 42): %s", params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_LOAD_CSF_PTR << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[0]);

  } else if (strcmp(op, "ccsrcopy") == 0) {
    if (nparam < 3) {
      rcw_set_error(ctx,
          "\"ccsrcopy\" requires 3 parameters (src, dest, words): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * LX2160A RM Table 37 "CCSR Write from Address", 16 bytes:
     *   word 0  HDR=0x80, CMD=0x02, low 2 bits = WORD TYPE
     *   word 1  SRC_ADDR  [31:0]  -- randomly accessible memory (OCRAM/SPRAM)
     *   word 2  DEST_ADDR [27:0]  -- CCSR, upper nibble reserved
     *   word 3  COPY LENGTH in words
     *
     * Word type is the width of the DESTINATION CCSR registers, not of the
     * copy: 1 = 8-bit, 2 = 16-bit, 3 = 32-bit, and 0 is explicitly invalid.
     * The suffix names the width in bytes (.b1/.b2/.b4) to match awrite.
     */
    uint32_t wtype;
    if (strcmp(suffix, "b1") == 0)      wtype = 1;
    else if (strcmp(suffix, "b2") == 0) wtype = 2;
    else if (strcmp(suffix, "b4") == 0) wtype = 3;
    else {
      rcw_set_error(ctx,
          "\"ccsrcopy\" needs a width suffix .b1, .b2 or .b4 "
          "(destination register width; word type 0 is invalid): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[1] > PBI_CCSR_ADDR_MAX) {
      rcw_set_error(ctx,
          "\"ccsrcopy\" destination 0x%x exceeds the 28-bit CCSR field: %s",
          params[1], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_CCSR_COPY << PBI_CMD_SHIFT) | wtype);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[0]);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[1]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[2]);

  } else if (strcmp(op, "ccsrset") == 0 ||
             strcmp(op, "ccsrclear") == 0 ||
             strcmp(op, "ccsrreplace") == 0) {
    /*
     * LX2160A RM Table 43 "CCSR Read, Modify and Write", 16 bytes:
     *   word 0  HDR=0x80, CMD=0x42, bits [3:2] ops type, [1:0] mask type
     *   word 1  Destination CCSR Address [27:0]
     *   word 2  Mask
     *   word 3  Data  -- REPLACE only; ignored for set/clear
     *
     * ops type: 0 set (reg |= mask), 1 clear (reg &= ~mask),
     *           2 replace (reg = (reg & ~mask) | (data & mask)), 3 reserved.
     * mask type: 1 = MASK[7:0], 2 = MASK[15:0], 3 = MASK[31:0]; 0 invalid.
     *
     * DOC NOTE: RM Table 43 is titled "CCSR Write from Address" and prints
     * "CMD = 0x02", but its body is the read-modify-write semantics and
     * 0x02 is already Table 37's opcode. Table 27 "PBI Command Summary"
     * lists this command as 0x42, which is what we emit -- two commands
     * cannot share an opcode.
     *
     * The mask width is spelled explicitly rather than derived from the
     * mask value: it selects the ACCESS WIDTH of the register, so deriving
     * .b1 from a mask of 0x000000FF would silently turn a 32-bit register
     * access into an 8-bit one.
     */
    uint32_t ops = (strcmp(op, "ccsrset") == 0) ? PBI_RMW_OPS_SET
                 : (strcmp(op, "ccsrclear") == 0) ? PBI_RMW_OPS_CLEAR
                 : PBI_RMW_OPS_REPLACE;
    int want = (ops == PBI_RMW_OPS_REPLACE) ? 3 : 2;
    if (nparam < want) {
      rcw_set_error(ctx,
          "\"%s\" requires %d parameters (addr, mask%s): %s",
          op, want, (ops == PBI_RMW_OPS_REPLACE) ? ", data" : "", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    uint32_t mtype;
    if (strcmp(suffix, "b1") == 0)      mtype = 1;
    else if (strcmp(suffix, "b2") == 0) mtype = 2;
    else if (strcmp(suffix, "b4") == 0) mtype = 3;
    else {
      rcw_set_error(ctx,
          "\"%s\" needs a mask-width suffix .b1, .b2 or .b4 "
          "(mask type 0 is invalid): %s", op, line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /* RM: destination CCSR address must be above 0x100_0000. */
    if (params[0] <= 0x1000000u) {
      rcw_set_error(ctx,
          "\"%s\" destination 0x%x must be greater than 0x1000000: %s",
          op, params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] > PBI_CCSR_ADDR_MAX) {
      rcw_set_error(ctx,
          "\"%s\" destination 0x%x exceeds the 28-bit CCSR field: %s",
          op, params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /* RM: 4-byte mask needs a 4-byte aligned address, 2-byte needs 2. */
    if (mtype == 3u && (params[0] % 4u) != 0u) {
      rcw_set_error(ctx,
          "\"%s.b4\" address 0x%x must be 4-byte aligned: %s",
          op, params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (mtype == 2u && (params[0] % 2u) != 0u) {
      rcw_set_error(ctx,
          "\"%s.b2\" address 0x%x must be 2-byte aligned: %s",
          op, params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_CCSR_RMW << PBI_CMD_SHIFT) |
        (ops << PBI_RMW_OPS_SHIFT) | mtype);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[0]);
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[1]);
    if (err != RCW_OK)
      return err;
    /* Data word is present for all three; the SP ignores it unless REPLACE. */
    return pbi_pack_word(ctx, (ops == PBI_RMW_OPS_REPLACE) ? params[2] : 0u);

  } else if (strcmp(op, "loadcondition") == 0) {
    if (nparam < 2) {
      rcw_set_error(ctx,
          "\"loadcondition\" requires 2 parameters (addr, mask): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * LX2160A RM Table 40 "Load Condition Command Format", 12 bytes:
     *   word 0  HDR=0x80, CMD=0x14
     *   word 1  ADDR   -- read at PBI time, typically a CCSR register
     *   word 2  MASK   -- ANDed with the loaded value
     * The result is kept in an internal SP variable and consumed by the
     * next conditional jump. Only "jumpif" consumes it today; the RM
     * notes conditional jump is the only conditional command defined.
     */
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_LOADCONDITION << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[0]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[1]);

  } else if (strcmp(op, "jump") == 0) {
    if (nparam < 1) {
      rcw_set_error(ctx,
          "\"jump\" requires 1 parameter (byte offset): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * LX2160A RM Table 46 "Jump Command Format", 8 bytes:
     *   word 0  HDR=0x80, CMD=0x84
     *   word 1  OFFSET -- bytes forward from the start of THIS command
     *
     * FORWARD ONLY. The SP cannot jump backwards, so this cannot build a
     * loop; it exists to skip a block, e.g. configuration for IP that a
     * given personality does not use. The RM's error checks are "4-byte
     * alignment" and "OFFSET greater than 8", both enforced here -- an
     * offset of exactly 8 would target the word right after the command
     * and is rejected by the SP, and a misaligned offset lands mid-command.
     */
    if (params[0] % 4u != 0u) {
      rcw_set_error(ctx,
          "\"jump\" offset 0x%x is not 4-byte aligned: %s",
          params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] <= 8u) {
      rcw_set_error(ctx,
          "\"jump\" offset 0x%x must be greater than 8 (RM Table 46): %s",
          params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_JUMP << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[0]);

  } else if (strcmp(op, "jumpif") == 0) {
    if (nparam < 2) {
      rcw_set_error(ctx,
          "\"jumpif\" requires 2 parameters (offset, condition): %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    /*
     * LX2160A RM Table 47 "Conditional Jump Command Format", 12 bytes:
     *   word 0  HDR=0x80, CMD=0x85
     *   word 1  OFFSET    -- bytes forward from the start of THIS command
     *   word 2  CONDITION -- compared against the value most recently
     *                        loaded by "loadcondition"
     *
     * Same forward-only restriction and the same two RM error checks as
     * the unconditional jump. Must be preceded by a "loadcondition", or
     * the SP compares against whatever the internal variable held.
     */
    if (params[0] % 4u != 0u) {
      rcw_set_error(ctx,
          "\"jumpif\" offset 0x%x is not 4-byte aligned: %s",
          params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] <= 8u) {
      rcw_set_error(ctx,
          "\"jumpif\" offset 0x%x must be greater than 8 (RM Table 47): %s",
          params[0], line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx,
        PBL_CMD_GENERAL | (PBI_GENCMD_JUMP_COND << PBI_CMD_SHIFT));
    if (err != RCW_OK)
      return err;
    err = pbi_pack_word(ctx, params[0]);
    if (err != RCW_OK)
      return err;
    return pbi_pack_word(ctx, params[1]);

  } else if (strcmp(op, "blockcopy") == 0) {
    if (nparam < 4) {
      rcw_set_error(ctx,
          "\"blockcopy\" requires 4 parameters: %s", line);
      return RCW_ERR_PBI_SYNTAX;
    }
    if (params[0] > PBI_BLOCKCOPY_SRC_MAX) {
      rcw_set_error(ctx,
          "\"blockcopy\" SRC 0x%x exceeds 8-bit interface field "
          "(max 0x%x): %s",
          params[0], PBI_BLOCKCOPY_SRC_MAX, line);
      return RCW_ERR_PBI_SYNTAX;
    }
    err = pbi_pack_word(ctx, PBL_CMD_BLOCKCOPY | params[0]);
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
    if (params[0] > PBI_ACWINDOW_MAX) {
      rcw_set_error(ctx,
          "\"loadacwindow\" 0x%x exceeds 14-bit field "
          "(max 0x%x); upper bits would corrupt the CMD byte: %s",
          params[0], PBI_ACWINDOW_MAX, line);
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
