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
rcw_crc32(const uint8_t *data, size_t len) {
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

  /* Calculate the CRC (matches rcw.py lines 134-138) */
  uint32_t crc = RCW_CRC32_INIT;

  for (size_t i = 0; i < len; i++)
    crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];

  return crc;
}
