/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief CI stand-in for the Test Environment's te_string.h
 *
 * The subset ta_sql uses: a NUL-terminated growable string with
 * printf-style and counted append. The fields are named as TE names
 * them.
 */
#ifndef CI_STUB_TE_STRING_H
#define CI_STUB_TE_STRING_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_alloc.h"

typedef struct te_string {
    char   *ptr;    /**< The string, or NULL when nothing was appended. */
    size_t  size;   /**< Bytes allocated. */
    size_t  len;    /**< Length, without the NUL. */
} te_string;

#define TE_STRING_INIT { NULL, 0, 0 }

static inline void
ci_te_string_reserve(te_string *s, size_t extra)
{
    if (s->len + extra + 1 > s->size)
    {
        size_t size = s->size == 0 ? 64 : s->size;

        while (size < s->len + extra + 1)
            size *= 2;
        s->ptr = realloc(s->ptr, size);
        if (s->ptr == NULL)
            abort();
        s->size = size;
    }
}

static inline void
te_string_append(te_string *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static inline void
te_string_append(te_string *s, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        abort();

    ci_te_string_reserve(s, (size_t)n);

    va_start(ap, fmt);
    vsnprintf(s->ptr + s->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->len += (size_t)n;
}

static inline int
te_string_append_buf(te_string *s, const char *buf, size_t len)
{
    ci_te_string_reserve(s, len);
    memcpy(s->ptr + s->len, buf, len);
    s->len += len;
    s->ptr[s->len] = '\0';
    return 0;
}

static inline void
te_string_reset(te_string *s)
{
    if (s->ptr != NULL)
        s->ptr[0] = '\0';
    s->len = 0;
}

static inline void
te_string_free(te_string *s)
{
    free(s->ptr);
    s->ptr = NULL;
    s->size = 0;
    s->len = 0;
}

#endif /* !CI_STUB_TE_STRING_H */
