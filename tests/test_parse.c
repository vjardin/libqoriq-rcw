/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <cmocka.h>

#include "rcw_internal.h"

/*
 * Redirect stderr to a tmpfile, run `body`, then read what was written
 * and copy it into `out` (NUL-terminated, truncated to out_size-1).
 * Restores stderr afterwards.
 */
#define CAPTURE_STDERR(out, out_size, body)                  \
  do {                                                       \
    fflush(stderr);                                          \
    int _saved = dup(fileno(stderr));                        \
    FILE *_tmp = tmpfile();                                  \
    assert_non_null(_tmp);                                   \
    dup2(fileno(_tmp), fileno(stderr));                      \
    body;                                                    \
    fflush(stderr);                                          \
    rewind(_tmp);                                            \
    size_t _n = fread((out), 1, (out_size) - 1, _tmp);       \
    (out)[_n] = '\0';                                        \
    dup2(_saved, fileno(stderr));                            \
    close(_saved);                                           \
    fclose(_tmp);                                            \
  } while (0)

static const char minimal_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "%nocrc=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "MEM_PLL_RAT[15:10]\n"
  "BOOT_LOC[264:260]\n"
  "PBI_LENGTH[287:276]\n"
  "SYS_PLL_RAT=14\n"
  "MEM_PLL_RAT=21\n"
  "BOOT_LOC=26\n";

/* Test that variables are parsed correctly */
static void
test_parse_variables(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, minimal_source, strlen(minimal_source));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->vars.size, 1024);
  assert_int_equal(ctx->vars.pbiformat, 2);
  assert_true(ctx->vars.classicbitnumbers);
  assert_true(ctx->vars.littleendian);
  assert_true(ctx->vars.nocrc);

  rcw_ctx_free(ctx);
}

/* Test that symbol definitions are parsed correctly */
static void
test_parse_symbols(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, minimal_source, strlen(minimal_source));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 4);

  assert_string_equal(ctx->symbols.entries[0].name, "SYS_PLL_RAT");
  assert_int_equal(ctx->symbols.entries[0].begin, 6);
  assert_int_equal(ctx->symbols.entries[0].end, 2);

  assert_string_equal(ctx->symbols.entries[1].name, "MEM_PLL_RAT");
  assert_int_equal(ctx->symbols.entries[1].begin, 15);
  assert_int_equal(ctx->symbols.entries[1].end, 10);

  assert_string_equal(ctx->symbols.entries[2].name, "BOOT_LOC");
  assert_int_equal(ctx->symbols.entries[2].begin, 264);
  assert_int_equal(ctx->symbols.entries[2].end, 260);

  rcw_ctx_free(ctx);
}

/* Test that assignments are parsed correctly */
static void
test_parse_assignments(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, minimal_source, strlen(minimal_source));
  assert_int_equal(err, RCW_OK);

  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 14);

  assert_true(ctx->symbols.entries[1].has_value);
  assert_int_equal(ctx->symbols.entries[1].value, 21);

  assert_true(ctx->symbols.entries[2].has_value);
  assert_int_equal(ctx->symbols.entries[2].value, 26);

  /* PBI_LENGTH not assigned */
  assert_false(ctx->symbols.entries[3].has_value);

  rcw_ctx_free(ctx);
}

/* Test hex value parsing */
static void
test_parse_hex_values(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "SYSCLK_FREQ[301:292]\n"
    "SYSCLK_FREQ=0x258\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 0x258);

  rcw_ctx_free(ctx);
}

/* Test single-bit field definition: NAME[pos] */
static void
test_parse_single_bit(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "SYS_PLL_SPD[128]\n"
    "SYS_PLL_SPD=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 1);
  assert_int_equal(ctx->symbols.entries[0].begin, 128);
  assert_int_equal(ctx->symbols.entries[0].end, 128);
  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 1);

  rcw_ctx_free(ctx);
}

/* Test PBI block parsing */
static void
test_parse_pbi_block(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%littleendian=1\n"
    "PBI_LENGTH[287:276]\n"
    ".pbi\n"
    "write 0x01e00200,0x12345678\n"
    ".end\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);

  /* PBI should have 8 bytes (1 write = 2 words) */
  assert_int_equal(ctx->pbi.len, 8);

  rcw_ctx_free(ctx);
}

/* Test blank lines and whitespace handling */
static void
test_parse_whitespace(void **state) {
  (void)state;
  const char src[] =
    "  %size=1024  \n"
    "\n"
    "  %pbiformat=2\n"
    "  SYS_PLL_RAT [ 6 : 2 ] \n"
    "  SYS_PLL_RAT = 14 \n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->vars.size, 1024);
  assert_int_equal(ctx->symbols.count, 1);
  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 14);

  rcw_ctx_free(ctx);
}

/*
 * Test -w (warnings) for duplicate bitfield assignments.
 * Mirrors rcw.py's behavior at lines 515-516:
 *   if options.warnings and (name in assignments):
 *       print('Warning: Duplicate assignment for bitfield', name)
 *
 * Wording must match exactly (only the output stream differs:
 * rcw.py prints to stdout, libqoriq-rcw to stderr).
 */
static const char dup_assign_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "SYS_PLL_RAT[6:2]\n"
  "SYS_PLL_RAT=14\n"
  "SYS_PLL_RAT=10\n";

static void
test_parse_dup_assign_warns_when_enabled(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  rcw_ctx_set_warnings(ctx, 1);

  char buf[256];
  rcw_error_t err = RCW_OK;
  CAPTURE_STDERR(buf, sizeof(buf), {
    err = rcw_parse(ctx, dup_assign_source, strlen(dup_assign_source));
  });

  assert_int_equal(err, RCW_OK);
  assert_non_null(strstr(buf, "Warning: Duplicate assignment for bitfield SYS_PLL_RAT"));

  /* Last assignment wins */
  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 10);

  rcw_ctx_free(ctx);
}

static void
test_parse_dup_assign_silent_when_disabled(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  /* warnings disabled (default) */

  char buf[256];
  rcw_error_t err = RCW_OK;
  CAPTURE_STDERR(buf, sizeof(buf), {
    err = rcw_parse(ctx, dup_assign_source, strlen(dup_assign_source));
  });

  assert_int_equal(err, RCW_OK);
  assert_null(strstr(buf, "Duplicate assignment"));

  /* Last assignment still wins regardless of -w */
  assert_true(ctx->symbols.entries[0].has_value);
  assert_int_equal(ctx->symbols.entries[0].value, 10);

  rcw_ctx_free(ctx);
}

/*
 * A1 - bitfield position must fit in the 1024-bit RCW.
 *
 * Without the bound check, FOO[2000:2000]=1 reaches set_bits() with
 * bytepos=250 and writes byte 250 of the 128-byte stack buffer in
 * rcw_bits_pack(). Confirmed under ASan to corrupt the saved return
 * address. Parser must drop the definition (no symbol added) and the
 * downstream assignment must fall through as "Unknown bitfield".
 */
static void
test_parse_bitpos_oob_range(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[2000:2000]\n"
    "FOO=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  char buf[256];
  rcw_error_t err = RCW_OK;
  CAPTURE_STDERR(buf, sizeof(buf), {
    err = rcw_parse(ctx, src, strlen(src));
  });

  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 0);
  /* Assignment to the dropped symbol must report unknown */
  assert_non_null(strstr(buf, "Unknown bitfield"));

  rcw_ctx_free(ctx);
}

/* A1 - upper edge: 1024 is one past the last legal position. */
static void
test_parse_bitpos_just_over_max(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[1024]\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 0);

  rcw_ctx_free(ctx);
}

/* A1 - exact maximum (1023) is accepted. */
static void
test_parse_bitpos_max_accepted(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "TOPBIT[1023]\n"
    "TOPBIT=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 1);
  assert_true(ctx->symbols.entries[0].has_value);

  rcw_ctx_free(ctx);
}

/*
 * A1 - negative literal. strtoul("-1") wraps to ULONG_MAX which casts
 * to int=-1. set_bits would then write buf[-1]. The parser must
 * reject the leading '-'.
 */
static void
test_parse_bitpos_negative(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[-1:0]\n"
    "FOO=1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 0);

  rcw_ctx_free(ctx);
}

/*
 * B3 - assignment value with strtoull overflow. The value field is
 * uint64_t and must be rejected when the literal does not fit (or
 * has trailing garbage / leading minus).
 */
static void
test_parse_value_overflow(void **state) {
  (void)state;
  /* ULLONG_MAX + 1 forces ERANGE on strtoull. */
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[7:0]\n"
    "FOO=99999999999999999999999\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  /* Symbol exists but assignment was rejected */
  assert_int_equal(ctx->symbols.count, 1);
  assert_false(ctx->symbols.entries[0].has_value);

  rcw_ctx_free(ctx);
}

/* B3 - negative assignment must be rejected (bitfield is unsigned). */
static void
test_parse_value_negative(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[7:0]\n"
    "FOO=-1\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 1);
  assert_false(ctx->symbols.entries[0].has_value);

  rcw_ctx_free(ctx);
}

/* B3 - trailing garbage in value (e.g. "1abc") must be rejected. */
static void
test_parse_value_trailing_garbage(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "FOO[7:0]\n"
    "FOO=42xyz\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_false(ctx->symbols.entries[0].has_value);

  rcw_ctx_free(ctx);
}

/*
 * B5 - bitfield names that would otherwise truncate inside sym->name
 * (RCW_FIELD_NAME_MAX = 64) must be rejected. Otherwise two distinct
 * 70-char names sharing a 63-char prefix become aliases of each other,
 * defeating duplicate detection.
 */
static void
test_parse_name_too_long(void **state) {
  (void)state;
  /* 80-character name */
  char src[256];
  snprintf(src, sizeof(src),
           "%%size=1024\n"
           "%%pbiformat=2\n"
           "%-80s[7:0]\n", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

  rcw_ctx_t *ctx = rcw_ctx_new();
  rcw_error_t err = rcw_parse(ctx, src, strlen(src));
  assert_int_equal(err, RCW_OK);
  assert_int_equal(ctx->symbols.count, 0);

  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_parse_variables),
    cmocka_unit_test(test_parse_symbols),
    cmocka_unit_test(test_parse_assignments),
    cmocka_unit_test(test_parse_hex_values),
    cmocka_unit_test(test_parse_single_bit),
    cmocka_unit_test(test_parse_pbi_block),
    cmocka_unit_test(test_parse_whitespace),
    cmocka_unit_test(test_parse_dup_assign_warns_when_enabled),
    cmocka_unit_test(test_parse_dup_assign_silent_when_disabled),
    cmocka_unit_test(test_parse_bitpos_oob_range),
    cmocka_unit_test(test_parse_bitpos_just_over_max),
    cmocka_unit_test(test_parse_bitpos_max_accepted),
    cmocka_unit_test(test_parse_bitpos_negative),
    cmocka_unit_test(test_parse_value_overflow),
    cmocka_unit_test(test_parse_value_negative),
    cmocka_unit_test(test_parse_value_trailing_garbage),
    cmocka_unit_test(test_parse_name_too_long),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
