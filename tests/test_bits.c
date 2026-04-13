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

/* Test packing a single field: SYS_PLL_RAT[6:2] = 14 */
static void
test_pack_single_field(void **state) {
	(void)state;

	rcw_symtab_t syms = {0};
	rcw_vars_t vars = {
		.size = 1024,
		.pbiformat = 2,
		.classicbitnumbers = false,
		.littleendian = true,
	};

	/* SYS_PLL_RAT[6:2] - 5-bit field at bits 2..6 */
	rcw_symbol_t *sym = &syms.entries[syms.count++];
	snprintf(sym->name, RCW_FIELD_NAME_MAX, "SYS_PLL_RAT");
	sym->begin = 6;
	sym->end = 2;
	sym->has_value = true;
	sym->value = 14;

	uint8_t out[RCW_SIZE_BYTES];
	rcw_error_t err = rcw_bits_pack(&syms, &vars, out);
	assert_int_equal(err, RCW_OK);

	/* Without classicbitnumbers, bit 2..6 (MSB-first) should have value
	 * 14 = 0b01110. In MSB-first layout:
	 *   bit 0 = MSB of byte 0
	 *   bits 2..6 are in byte 0
	 *   byte 0 bit pattern: 0b00_01110_0 = 0x38? Let's check.
	 *
	 * Actually, since begin(6) > end(2), the value gets bit-reversed.
	 * 14 = 0b01110, reversed(5 bits) = 0b01110 (palindrome).
	 *
	 * Bit positions 2..6, value 14 = 0b01110.
	 * set_bits sets bit e-i for bit i of value.
	 * bit 6: (14>>0)&1 = 0
	 * bit 5: (14>>1)&1 = 1
	 * bit 4: (14>>2)&1 = 1
	 * bit 3: (14>>3)&1 = 1
	 * bit 2: (14>>4)&1 = 0
	 * In byte 0 (bits 0-7, MSB-first): bits 2-6 = 0,1,1,1,0
	 * byte 0 = 0b00_01110_0 = 0x1C
	 * Wait: bit 0 is MSB (bit 7 of byte), bit 7 is LSB (bit 0 of byte)
	 * byte0 = bit0<<7 | bit1<<6 | bit2<<5 | bit3<<4 | bit4<<3 |
	 *         bit5<<2 | bit6<<1 | bit7<<0
	 * = 0<<7 | 0<<6 | 0<<5 | 1<<4 | 1<<3 | 1<<2 | 0<<1 | 0<<0
	 * = 0x1C
	 */
	assert_int_equal(out[0], 0x1C);

	/* Rest should be zero */
	for (int i = 1; i < RCW_SIZE_BYTES; i++)
		assert_int_equal(out[i], 0);
}

/* Test with classicbitnumbers=1 (per-byte bit reversal in output) */
static void
test_pack_classicbitnumbers(void **state) {
	(void)state;

	rcw_symtab_t syms = {0};
	rcw_vars_t vars = {
		.size = 1024,
		.pbiformat = 2,
		.classicbitnumbers = true,
		.littleendian = true,
	};

	/* SYS_PLL_RAT[6:2] = 14 */
	rcw_symbol_t *sym = &syms.entries[syms.count++];
	snprintf(sym->name, RCW_FIELD_NAME_MAX, "SYS_PLL_RAT");
	sym->begin = 6;
	sym->end = 2;
	sym->has_value = true;
	sym->value = 14;

	uint8_t out[RCW_SIZE_BYTES];
	rcw_error_t err = rcw_bits_pack(&syms, &vars, out);
	assert_int_equal(err, RCW_OK);

	/*
	 * Before bit-reversal, byte 0 = 0x1C = 0b00011100
	 * After per-byte bit-reversal: 0b00111000 = 0x38
	 */
	assert_int_equal(out[0], 0x38);
}

/* Test extracting a field value (round-trip with pack) */
static void
test_extract_roundtrip(void **state) {
	(void)state;

	rcw_symtab_t syms = {0};
	rcw_vars_t vars = {
		.size = 1024,
		.pbiformat = 2,
		.classicbitnumbers = true,
		.littleendian = true,
	};

	/* Set up several fields */
	rcw_symbol_t *s;

	s = &syms.entries[syms.count++];
	snprintf(s->name, RCW_FIELD_NAME_MAX, "SYS_PLL_RAT");
	s->begin = 6; s->end = 2;
	s->has_value = true; s->value = 14;

	s = &syms.entries[syms.count++];
	snprintf(s->name, RCW_FIELD_NAME_MAX, "MEM_PLL_RAT");
	s->begin = 15; s->end = 10;
	s->has_value = true; s->value = 21;

	s = &syms.entries[syms.count++];
	snprintf(s->name, RCW_FIELD_NAME_MAX, "BOOT_LOC");
	s->begin = 264; s->end = 260;
	s->has_value = true; s->value = 26;

	uint8_t packed[RCW_SIZE_BYTES];
	rcw_error_t err = rcw_bits_pack(&syms, &vars, packed);
	assert_int_equal(err, RCW_OK);

	/* Extract and verify */
	uint64_t v;

	v = rcw_bits_extract(packed, 6, 2, true);
	assert_int_equal(v, 14);

	v = rcw_bits_extract(packed, 15, 10, true);
	assert_int_equal(v, 21);

	v = rcw_bits_extract(packed, 264, 260, true);
	assert_int_equal(v, 26);
}

/* Test field with no value set (should leave bits as zero) */
static void
test_pack_no_value(void **state) {
	(void)state;

	rcw_symtab_t syms = {0};
	rcw_vars_t vars = {
		.size = 1024,
		.pbiformat = 2,
		.classicbitnumbers = false,
	};

	rcw_symbol_t *sym = &syms.entries[syms.count++];
	snprintf(sym->name, RCW_FIELD_NAME_MAX, "TEST");
	sym->begin = 7; sym->end = 0;
	sym->has_value = false;

	uint8_t out[RCW_SIZE_BYTES];
	rcw_error_t err = rcw_bits_pack(&syms, &vars, out);
	assert_int_equal(err, RCW_OK);

	/* All zeros */
	for (int i = 0; i < RCW_SIZE_BYTES; i++)
		assert_int_equal(out[i], 0);
}

int
main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pack_single_field),
    cmocka_unit_test(test_pack_classicbitnumbers),
    cmocka_unit_test(test_extract_roundtrip),
    cmocka_unit_test(test_pack_no_value),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
