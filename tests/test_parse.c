/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "rcw_internal.h"

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
static void test_parse_variables(void **state)
{
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
static void test_parse_symbols(void **state)
{
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
static void test_parse_assignments(void **state)
{
	(void)state;
	rcw_ctx_t *ctx = rcw_ctx_new();
	assert_non_null(ctx);

	rcw_error_t err = rcw_parse(ctx, minimal_source,
				    strlen(minimal_source));
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
static void test_parse_hex_values(void **state)
{
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
static void test_parse_single_bit(void **state)
{
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
static void test_parse_pbi_block(void **state)
{
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
static void test_parse_whitespace(void **state)
{
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

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_parse_variables),
		cmocka_unit_test(test_parse_symbols),
		cmocka_unit_test(test_parse_assignments),
		cmocka_unit_test(test_parse_hex_values),
		cmocka_unit_test(test_parse_single_bit),
		cmocka_unit_test(test_parse_pbi_block),
		cmocka_unit_test(test_parse_whitespace),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
