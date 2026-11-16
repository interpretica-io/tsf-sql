#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Static checks of the module, the same locally and in CI:
#
#  - layout: no tabs, no trailing blanks, a final newline, no CR, lines
#    of at most 80 columns in C, headers and RPC definitions;
#  - every source carries the SPDX licence line and the copyright line;
#  - cppcheck over the agent-side library and the contract test;
#  - shellcheck over the scripts;
#  - actionlint over the GitHub workflows (when actionlint is present).
#
# Usage: ci/lint.sh     (from anywhere; exits non-zero on any finding)

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

failed=0
fail() {
    echo "lint: $*" >&2
    failed=1
}

# The files git tracks or would track (not ignored) matching patterns.
files() {
    git ls-files --cached --others --exclude-standard "$@" | sort -u
}

# Arrays filled with read loops: bash 3.2 (macOS) has no mapfile.
c_files=() ; m4_files=() ; src_files=() ; sh_files=() ; text_files=()
while IFS= read -r f ; do c_files+=("$f") ; done < <(files '*.c' '*.h')
while IFS= read -r f ; do m4_files+=("$f") ; done < <(files '*.m4')
while IFS= read -r f ; do sh_files+=("$f") ; done < <(files '*.sh')
while IFS= read -r f ; do src_files+=("$f") ; done < \
    <(files '*.c' '*.h' '*.m4' '*.sh')
while IFS= read -r f ; do text_files+=("$f") ; done < \
    <(files '*.c' '*.h' '*.m4' '*.sh' 'meson.build' '*.yml' '*.env' \
            '*.md' '.gitignore' 'LICENSE')

# --- layout -------------------------------------------------------------
for f in ${text_files[@]+"${text_files[@]}"} ; do
    if grep -n $'\t' "$f" >/dev/null ; then
        grep -n $'\t' "$f" | head -3 | sed "s|^|$f:|" >&2
        fail "$f: tab character (indent with spaces)"
    fi
    if grep -n '[[:space:]]$' "$f" >/dev/null ; then
        grep -n '[[:space:]]$' "$f" | head -3 | sed "s|^|$f:|" >&2
        fail "$f: trailing blanks"
    fi
    if grep -q $'\r' "$f" ; then
        fail "$f: CR line endings"
    fi
    if [ -s "$f" ] && [ "$(tail -c 1 "$f" | od -An -c | tr -d ' ')" != '\n' ]
    then
        fail "$f: no newline at the end of the file"
    fi
done

for f in ${c_files[@]+"${c_files[@]}"} ${m4_files[@]+"${m4_files[@]}"} ; do
    awk -v f="$f" 'length > 80 {
        printf "%s:%d: %d columns\n", f, FNR, length; bad = 1 }
        END { exit bad }' "$f" >&2 || fail "$f: lines longer than 80 columns"
done

# --- licence and copyright ----------------------------------------------
for f in ${src_files[@]+"${src_files[@]}"} ; do
    head -5 "$f" | grep -q 'SPDX-License-Identifier: MIT' ||
        fail "$f: no 'SPDX-License-Identifier: MIT' in the first lines"
    head -5 "$f" | grep -q 'Copyright (C) 20[0-9-]* Interpretica' ||
        fail "$f: no Interpretica copyright line in the first lines"
done

# --- cppcheck -----------------------------------------------------------
# The style level is on, minus two rules that fight the Test Environment
# coding style: variables are declared at the top of their block
# (variableScope), and a callback keeps the signature its interface
# gives it (constParameterCallback, constParameterPointer). The
# agent-side units are checked against the stand-in TE headers of
# ci/stubs. rpcs_sql.c is RPC glue built from TE's TARPC_FUNC_STATIC
# macro and the engine-side tapi_sql needs TE's own headers; both are
# compiled, with -Werror, by a suite build (tsf-sql-ts).
if command -v cppcheck >/dev/null ; then
    cppcheck --quiet --error-exitcode=1 --inline-suppr --std=c11 \
        --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem --suppress=missingInclude \
        --suppress=variableScope --suppress=constParameterCallback \
        --suppress=constParameterPointer --suppress=constVariablePointer \
        --suppress=checkersReport \
        -D_GNU_SOURCE -DTA_SQL_HAVE_SQLITE -DTA_SQL_HAVE_POSTGRES \
        -DTA_SQL_HAVE_MYSQL \
        -I ta_sql -I ci/stubs \
        ta_sql ci ||
        fail "cppcheck reported findings"
else
    fail "cppcheck is not installed"
fi

# --- shellcheck ---------------------------------------------------------
if command -v shellcheck >/dev/null ; then
    shellcheck -x ${sh_files[@]+"${sh_files[@]}"} ||
        fail "shellcheck reported findings"
else
    fail "shellcheck is not installed"
fi

# --- actionlint ---------------------------------------------------------
if command -v actionlint >/dev/null ; then
    actionlint || fail "actionlint reported findings"
else
    echo "lint: actionlint is not installed; workflows not checked" >&2
fi

if [ "${failed}" -ne 0 ] ; then
    echo "lint: FAILED" >&2
    exit 1
fi
echo "lint: ok"
