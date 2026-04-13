/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Bitfield packing/extraction for the 1024-bit RCW register.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "rcw_internal.h"

/* Reverse the bit order within a byte. */
static uint8_t
reverse_byte(uint8_t b) {
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));

  return b;
}

/* Reverse the bit order of the low 'nbits' bits of v. */
static uint64_t
reverse_bits(uint64_t v, int nbits) {
  uint64_t result = 0;

  for (int i = 0; i < nbits; i++) {
    result <<= 1;
    result |= (v & 1);
    v >>= 1;
  }

  return result;
}

/*
 * Set a range of bits [b..e] (b <= e, MSB-first layout where bit 0 is the
 * MSB of byte 0) in a 128-byte buffer.
 *
 * Matches rcw.py: bits += v << ((size - 1) - e)
 * where 'size' is 1024 and bit 0 is the most-significant bit.
 */
static void
set_bits(uint8_t buf[RCW_SIZE_BYTES], int b, int e, uint64_t v) {
  int width = 1 + e - b;

  for (int i = 0; i < width; i++) {
    int bitpos = e - i; /* MSB-first: bit 0 = MSB of byte 0 */
    int bytepos = bitpos / 8;
    int bitoff = 7 - (bitpos % 8);

    if ((v >> i) & 1)
    buf[bytepos] |= (uint8_t)(1 << bitoff);
  }
}


rcw_error_t
rcw_bits_pack(const rcw_symtab_t *syms, const rcw_vars_t *vars, uint8_t out[RCW_SIZE_BYTES]) {
  memset(out, 0, RCW_SIZE_BYTES);

  for (size_t i = 0; i < syms->count; i++) {
    const rcw_symbol_t *sym = &syms->entries[i];

    if (!sym->has_value)
      continue;

    int bb = sym->begin;
    int ee = sym->end;
    int b = (bb < ee) ? bb : ee;
    int e = (bb < ee) ? ee : bb;
    int s = 1 + e - b;

    uint64_t v = sym->value;

    /* Check overflow */
    if ((s < 64) && (v >= ((uint64_t)1 << s))) {
      /* Caller should have checked, but be safe */
      return RCW_ERR_VALUE_OVERFLOW;
    }

    /*
     * If begin > end (classic bit numbering where the declared
     * first bit is the higher-numbered one), reverse the value
     * bits before packing.
     * Matches rcw.py line 661-662:
     *   if b != bb:
     *       v = int(bin(int(v))[2:].zfill(s)[::-1], 2)
     */
    if (b != bb)
      v = reverse_bits(v, s);

    set_bits(out, b, e, v);
  }

  /*
   * Apply per-byte bit-reversal for classicbitnumbers.
   * Matches rcw.py lines 688-691:
   *   if classicbitnumbers:
   *       byte = int(bin(byte)[2:].zfill(8)[::-1], 2)
   */
  if (vars->classicbitnumbers) {
    for (int i = 0; i < RCW_SIZE_BYTES; i++)
    out[i] = reverse_byte(out[i]);
  }

  return RCW_OK;
}

uint64_t
rcw_bits_extract(const uint8_t data[RCW_SIZE_BYTES], int begin, int end, bool classicbitnumbers) {
  uint8_t buf[RCW_SIZE_BYTES];
  memcpy(buf, data, RCW_SIZE_BYTES);

  /* Undo per-byte bit-reversal */
  if (classicbitnumbers) {
    for (int i = 0; i < RCW_SIZE_BYTES; i++)
      buf[i] = reverse_byte(buf[i]);
  }

  /*
   * rcw.py decompile (lines 858-876):
   *   bitstring = ''.join(['{0:08b}'.format(x) for x in bitbytes])[::-1]
   *   bits = int(bitstring, 2)
   *   shift = b  (= min(begin, end))
   *   mask = (1 << s) - 1
   *   v = (bits >> shift) & mask
   *
   * The [::-1] reversal means bit 0 of 'bits' is the LSB of the last
   * byte. In our MSB-first buffer, bit position N (LSB-indexed) is at:
   *   byte_index = (RCW_SIZE_BITS - 1 - N) / 8
   *   bit_in_byte = (RCW_SIZE_BITS - 1 - N) % 8  (MSB=7, LSB=0)
   * But it is equivalent to:
   *   bit N in the reversed representation
   *   = bit (SIZE-1-N) in MSB-first
   *   = position N counting from the end.
   *
   * TBC approach: convert to the same integer as rcw.py does.
   */
  int bb = begin;
  int ee = end;
  int b = (bb < ee) ? bb : ee;
  int e = (bb < ee) ? ee : bb;
  int s = 1 + e - b;

  /*
   * rcw.py decompile:
   *   bitstring = ''.join('{0:08b}'.format(x) for x in bytes)[::-1]
   *   bits = int(bitstring, 2)
   *   v = (bits >> b) & mask
   *
   * int(s, 2) treats s[0] as MSB. After [::-1], bit k (counting
   * from LSB) of 'bits'
   *   = reversed_string[len-1-k]
   *   = original[k].
   *
   * original[k] = byte k/8, bit (7 - k % 8) within that byte.
   */
  uint64_t v = 0;

  for (int i = 0; i < s; i++) {
    int k = b + i;
    int byte_idx = k / 8;
    int bit_idx = 7 - (k % 8);

    if (buf[byte_idx] & (1 << bit_idx))
      v |= ((uint64_t)1 << i);
  }

  /*
   * rcw.py line 874: if b == bb: v = reverse
   * This applies when begin <= end (non-classic ordering).
   */
  if (b == bb)
    v = reverse_bits(v, s);

  return v;
}
