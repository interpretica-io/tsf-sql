/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief CI stand-in for the Test Environment's te_defs.h
 *
 * ta_sql uses little of TE: a growable string, a checked allocation
 * and the status codes. These stubs give exactly that, so
 * ci/driver_test.c can build ta_sql and run it against real databases
 * without a TE build tree.
 */
#ifndef CI_STUB_TE_DEFS_H
#define CI_STUB_TE_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define UNUSED(x)       ((void)(x))

#endif /* !CI_STUB_TE_DEFS_H */
