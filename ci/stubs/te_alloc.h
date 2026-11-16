/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief CI stand-in for the Test Environment's te_alloc.h
 *
 * TE_ALLOC returns zeroed memory and never NULL; so does this one.
 */
#ifndef CI_STUB_TE_ALLOC_H
#define CI_STUB_TE_ALLOC_H

#include <stdio.h>
#include <stdlib.h>

static inline void *
ci_te_alloc(size_t size)
{
    void *p = calloc(1, size == 0 ? 1 : size);

    if (p == NULL)
    {
        fprintf(stderr, "out of memory (%zu bytes)\n", size);
        abort();
    }
    return p;
}

#define TE_ALLOC(size) ci_te_alloc(size)

#endif /* !CI_STUB_TE_ALLOC_H */
