#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Free Mobile - Vincent Jardin
#
# The version script must list exactly the functions rcw.h declares.

set -eu

header=$1
map=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

grep -oE '\brcw_[a-z0-9_]+\(' "$header" | tr -d '(' | sort -u > "$tmp/header"
sed -n 's/^ *\(rcw_[a-z0-9_]*\);$/\1/p' "$map" | sort -u > "$tmp/map"

if ! diff -u "$tmp/header" "$tmp/map"; then
  echo "src/libqoriq-rcw.map and include/qoriq-rcw/rcw.h disagree (- header only, + map only)" >&2
  exit 1
fi
