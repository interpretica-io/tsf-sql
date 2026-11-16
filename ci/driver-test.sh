#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Build ta_sql with every driver whose client library is installed,
# under AddressSanitizer and UndefinedBehaviorSanitizer, and run
# ci/driver_test.c against the databases named in the environment
# (TSF_SQL_SQLITE, TSF_SQL_PG, TSF_SQL_MY - see ci/driver_test.c).
#
# Usage: ci/driver-test.sh
#
# The client libraries are found with pkg-config (Debian/Ubuntu:
# libsqlite3-dev, libpq-dev, libmariadb-dev). The flags match the Test
# Environment platform build (-D_GNU_SOURCE) plus -Werror, so a warning
# in a driver fails here before it reaches a suite build.

set -euo pipefail

top="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "${out}"' EXIT

defs=()
srcs=("${top}/ta_sql/ta_sql.c" "${top}/ta_sql/ta_sql_meta.c")
pkgs=()

if pkg-config --exists sqlite3 ; then
    defs+=(-DTA_SQL_HAVE_SQLITE)
    srcs+=("${top}/ta_sql/ta_sql_sqlite.c")
    pkgs+=(sqlite3)
fi
if pkg-config --exists libpq ; then
    defs+=(-DTA_SQL_HAVE_POSTGRES)
    srcs+=("${top}/ta_sql/ta_sql_pg.c")
    pkgs+=(libpq)
fi
if pkg-config --exists libmariadb ; then
    defs+=(-DTA_SQL_HAVE_MYSQL)
    srcs+=("${top}/ta_sql/ta_sql_mysql.c")
    pkgs+=(libmariadb)
fi
if [ "${#pkgs[@]}" -eq 0 ] ; then
    echo "driver-test: no client library found" >&2
    exit 1
fi
echo "driver-test: building with ${pkgs[*]}"

cc="${CC:-cc}"
# shellcheck disable=SC2046 # pkg-config output is a list of flags
"${cc}" -std=c11 -D_GNU_SOURCE -O1 -g \
    -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wstrict-prototypes \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer \
    "${defs[@]}" \
    -I"${top}/ci/stubs" -I"${top}/ta_sql" \
    $(pkg-config --cflags "${pkgs[@]}") \
    "${top}/ci/driver_test.c" "${srcs[@]}" \
    $(pkg-config --libs "${pkgs[@]}") -lpthread -lm \
    -o "${out}/driver_test"

# The client libraries keep process-lifetime allocations of their own
# (OpenSSL, the MariaDB plugin table); leaks are checked for ta_sql's
# own frames only.
cat > "${out}/lsan.supp" <<'SUPP'
leak:libcrypto
leak:libssl
leak:libmariadb
leak:libgnutls
leak:libpq
SUPP

TSF_SQL_SQLITE="${TSF_SQL_SQLITE:-${out}/test.sqlite}" \
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}" \
LSAN_OPTIONS="${LSAN_OPTIONS:-suppressions=${out}/lsan.supp:print_suppressions=0}" \
UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
    "${out}/driver_test"
