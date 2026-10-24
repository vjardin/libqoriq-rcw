/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * Custom CRC-32 matching rcw.py (polynomial 0x04C11DB7).
 * NOT compatible with zlib/binascii crc32.
 */

#include <stdint.h>
#include <stddef.h>

#include "rcw_internal.h"

uint32_t
rcw_crc32_update(uint32_t crc, const uint8_t *data, size_t len, bool reverse_bits) {
  /* Build the CRC table (matches rcw.py lines 125-132) */
  uint32_t table[RCW_CRC32_TABLE_SIZE];

  for (unsigned i = 0; i < RCW_CRC32_TABLE_SIZE; i++) {
    uint32_t mask = (uint32_t)i << 24;
    for (int j = 0; j < RCW_CRC32_BITS_PER_BYTE; j++) {
      if (mask & 0x80000000)
        mask = (mask << 1) ^ RCW_CRC32_POLY;
      else
        mask <<= 1;
    }
    table[i] = mask;
  }

  for (size_t i = 0; i < len; i++) {
    uint8_t b = reverse_bits ? rcw_reverse_byte(data[i]) : data[i];
    crc = (crc << 8) ^ table[(crc >> 24) ^ b];
  }

  return crc;
}

uint32_t
rcw_crc32(const uint8_t *data, size_t len) {
  return rcw_crc32_update(RCW_CRC32_INIT, data, len, false);
}
