/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

/*
 * Tests use mkstemp/strdup/unlink (POSIX). Enable POSIX.1-2008 so the
 * prototypes are visible under -std=c17. Must come BEFORE any include.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <unistd.h>
#include <cmocka.h>

#include "rcw_internal.h"

/*
 * Minimal source for a nocrc board (lx2160a-style).
 * This is what gcc -E would produce after preprocessing.
 */
static const char nocrc_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "%nocrc=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "PBI_LENGTH[287:276]\n"
  "SYS_PLL_RAT=7\n"
  ".pbi\n"
  "write 0x01e00620,0x80000000\n"
  ".end\n";

/* Test nocrc binary generation (Stop command, no CRC) */
static void
test_binary_nocrc(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, nocrc_source, strlen(nocrc_source), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /*
   * Expected layout:
   *   4 bytes: preamble 0xAA55AA55 (LE)
   *   4 bytes: Load RCW cmd 0x80100000 (LE) 128 bytes: RCW data
   *   4 bytes: checksum
   *   8 bytes: PBI write command
   *   4 bytes: Stop 0x80FF0000 (LE)
   *   4 bytes: 0x00000000
   * Total: 4+4+128+4+8+4+4 = 156
   */
  assert_int_equal(len, 156);

  /* Check preamble (LE) */
  assert_int_equal(out[0], 0x55);
  assert_int_equal(out[1], 0xAA);
  assert_int_equal(out[2], 0x55);
  assert_int_equal(out[3], 0xAA);

  /* Check Stop command near the end (LE: 0x80FF0000) */
  size_t stop_off = len - 8;
  uint32_t stop_word = (uint32_t)out[stop_off]
                    | ((uint32_t)out[stop_off+1] << 8)
                    | ((uint32_t)out[stop_off+2] << 16)
                    | ((uint32_t)out[stop_off+3] << 24);

  assert_int_equal(stop_word, 0x80FF0000);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/* Test no-pbl mode (raw RCW bits only) */
static void
test_binary_no_pbl(void **state) {
  (void)state;
  const char src[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "SYS_PLL_RAT=7\n";

  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);
  rcw_ctx_set_pbl(ctx, 0);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /* No PBL: just 128 bytes of RCW data */
  assert_int_equal(len, 128);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/*
 * Minimal source for a CRC board (ls1088-style, no %nocrc).
 */
static const char crc_source[] =
  "%size=1024\n"
  "%pbiformat=2\n"
  "%classicbitnumbers=1\n"
  "%littleendian=1\n"
  "SYS_PLL_RAT[6:2]\n"
  "PBI_LENGTH[287:276]\n"
  "SYS_PLL_RAT=7\n"
  ".pbi\n"
  "write 0x01e00620,0x80000000\n"
  ".end\n";

/* Test CRC binary generation (CRC-and-Stop command) */
static void
test_binary_with_crc(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, crc_source, strlen(crc_source), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_non_null(out);

  /* Same size as nocrc: 156 bytes */
  assert_int_equal(len, 156);

  /* Check CRC-and-Stop command near the end (LE: 0x808F0000) */
  size_t crc_off = len - 8;
  uint32_t crc_word = (uint32_t)out[crc_off]
     | ((uint32_t)out[crc_off+1] << 8)
     | ((uint32_t)out[crc_off+2] << 16)
     | ((uint32_t)out[crc_off+3] << 24);
  assert_int_equal(crc_word, 0x808F0000);

  /* CRC value should be non-zero */
  uint32_t crc_val = (uint32_t)out[crc_off+4]
    | ((uint32_t)out[crc_off+5] << 8)
    | ((uint32_t)out[crc_off+6] << 16)
    | ((uint32_t)out[crc_off+7] << 24);
  assert_true(crc_val != 0);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/* Test that PBI_LENGTH is auto-calculated */
static void
test_binary_pbi_length_auto(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, nocrc_source, strlen(nocrc_source), &out, &len);
  assert_int_equal(err, RCW_OK);

  /* PBI_LENGTH should have been set: 1 write = 2 words of PBI, +2 for Stop = 4 */
  bool found = false;
  for (size_t i = 0; i < ctx->symbols.count; i++) {
    if (strcmp(ctx->symbols.entries[i].name, "PBI_LENGTH") == 0) {
      assert_true(ctx->symbols.entries[i].has_value);
      assert_int_equal(ctx->symbols.entries[i].value, 4);
      found = true;
      break;
    }
  }
  assert_true(found);

  rcw_free(out);
  rcw_ctx_free(ctx);
}

/*
 * Helpers for testing the PBL_SP_SCRATCH_CEILING_BYTES warning.
 *
 * The warning is emitted to stderr inside rcw_binary_generate(); the
 * tests below redirect stderr to a temp file, run a compile, then
 * read the file back to assert presence/absence of the warning text.
 */

/* Redirect stderr to a fresh temp file. Returns the path (caller frees). */
static char *
redirect_stderr_to_tmp(void) {
  char *path = strdup("/tmp/libqoriq_rcw_test_stderr_XXXXXX");
  assert_non_null(path);

  int fd = mkstemp(path);
  assert_true(fd >= 0);
  close(fd);

  fflush(stderr);
  FILE *fp = freopen(path, "w", stderr);
  assert_non_null(fp);

  return path;
}

/* Restore stderr to /dev/tty (best-effort) and unlink the temp file. */
static void
restore_stderr(char *path) {
  fflush(stderr);
  /*
   * Reattach stderr to /dev/null so subsequent prints don't go to the
   * old (now-deleted) temp file. Tests don't depend on a TTY here.
   */
  freopen("/dev/null", "w", stderr);
  unlink(path);
  free(path);
}

/* Read a file fully into a malloc'd null-terminated buffer. */
static char *
read_file(const char *path) {
  FILE *fp = fopen(path, "r");
  assert_non_null(fp);
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  assert_true(n >= 0);

  char *buf = malloc((size_t)n + 1);
  assert_non_null(buf);
  size_t got = fread(buf, 1, (size_t)n, fp);
  buf[got] = '\0';
  fclose(fp);
  return buf;
}

/*
 * Build a synthetic .rcw source whose PBI body is large enough to
 * push the total binary at-or-over the ceiling.
 *
 * Each `wait` PBI command is 4 bytes (one 32-bit word). The fixed
 * overhead in a PBL binary is:
 *   8 bytes (preamble + Load RCW cmd)
 * + RCW_SIZE_BYTES (= 128 bytes RCW body)
 * + 4 bytes (checksum)
 * + 8 bytes (terminator: Stop or CRC+Stop)
 * = 148 bytes.
 *
 * To hit total >= ceiling we need at least
 *   ceil((ceiling - 148) / 4)  wait commands.
 * The caller passes `target_total` and we round the wait count up so
 * the produced binary is `target_total` bytes (or a few bytes more).
 */
static char *
build_oversized_source(size_t target_total) {
  const char header[] =
    "%size=1024\n"
    "%pbiformat=2\n"
    "%classicbitnumbers=1\n"
    "%littleendian=1\n"
    "%nocrc=1\n"
    "SYS_PLL_RAT[6:2]\n"
    "PBI_LENGTH[287:276]\n"
    "SYS_PLL_RAT=7\n"
    ".pbi\n";
  const char footer[] = ".end\n";
  const char wait_line[] = "wait 0x0001\n";

  size_t fixed_overhead = 148;
  size_t pbi_needed = (target_total > fixed_overhead)
                        ? (target_total - fixed_overhead) : 0;
  size_t nwaits = (pbi_needed + 3) / 4; /* round up: each wait = 4 B */

  size_t cap = sizeof(header) + sizeof(footer)
               + nwaits * sizeof(wait_line) + 64;
  char *src = malloc(cap);
  assert_non_null(src);
  size_t pos = 0;

  pos += (size_t)snprintf(src + pos, cap - pos, "%s", header);
  for (size_t i = 0; i < nwaits; i++)
    pos += (size_t)snprintf(src + pos, cap - pos, "%s", wait_line);
  pos += (size_t)snprintf(src + pos, cap - pos, "%s", footer);
  src[pos] = '\0';
  return src;
}

/*
 * Test that compiling a SMALL PBL emits NO warning on stderr.
 *
 * Uses the existing nocrc_source (156 bytes total) -- well below
 * PBL_SP_SCRATCH_CEILING_BYTES.
 */
static void
test_binary_no_warning_under_ceiling(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  char *log_path = redirect_stderr_to_tmp();

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, nocrc_source,
                                       strlen(nocrc_source), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_int_equal(len, 156);
  assert_true(len < PBL_SP_SCRATCH_CEILING_BYTES);

  fflush(stderr);
  char *captured = read_file(log_path);

  /* No warning text should appear at all -- stderr stays empty. */
  assert_int_equal(strlen(captured), 0);

  free(captured);
  restore_stderr(log_path);
  rcw_free(out);
  rcw_ctx_free(ctx);
}

/*
 * Test that compiling a PBL at-or-over PBL_SP_SCRATCH_CEILING_BYTES
 * emits a warning to stderr.
 *
 * Builds a synthetic source large enough to land the binary just
 * over the ceiling, captures stderr, and asserts the expected
 * warning markers are present.
 */
static void
test_binary_warning_at_ceiling(void **state) {
  (void)state;
  rcw_ctx_t *ctx = rcw_ctx_new();
  assert_non_null(ctx);

  /*
   * Aim for ceiling + 4 bytes (one extra wait beyond the boundary)
   * so the warning fires on the very first oversized build.
   */
  size_t target = PBL_SP_SCRATCH_CEILING_BYTES + 4;
  char *src = build_oversized_source(target);

  char *log_path = redirect_stderr_to_tmp();

  uint8_t *out = NULL;
  size_t len = 0;
  rcw_error_t err = rcw_compile_buffer(ctx, src, strlen(src), &out, &len);
  assert_int_equal(err, RCW_OK);
  assert_true(len >= PBL_SP_SCRATCH_CEILING_BYTES);

  fflush(stderr);
  char *captured = read_file(log_path);

  /*
   * The warning is multi-line. We assert on a few stable substrings
   * rather than the whole text, so cosmetic re-wording of the
   * warning doesn't break this test.
   */
  assert_non_null(strstr(captured, "warning:"));
  assert_non_null(strstr(captured, "SP scratch ceiling"));
  assert_non_null(strstr(captured, "boot SILENTLY"));
  /* The actual size and the ceiling should both appear in the text. */
  char buf[64];
  snprintf(buf, sizeof(buf), "%zu", len);
  assert_non_null(strstr(captured, buf));
  snprintf(buf, sizeof(buf), "%u", PBL_SP_SCRATCH_CEILING_BYTES);
  assert_non_null(strstr(captured, buf));

  free(captured);
  free(src);
  restore_stderr(log_path);
  rcw_free(out);
  rcw_ctx_free(ctx);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_binary_nocrc),
    cmocka_unit_test(test_binary_no_pbl),
    cmocka_unit_test(test_binary_with_crc),
    cmocka_unit_test(test_binary_pbi_length_auto),
    cmocka_unit_test(test_binary_no_warning_under_ceiling),
    cmocka_unit_test(test_binary_warning_at_ceiling),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
