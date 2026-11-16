/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief ta_sql internals shared by its own units
 */

#ifndef __TA_SQL_INT_H__
#define __TA_SQL_INT_H__

#include <stddef.h>

#include "te_errno.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode one value cell.
 *
 * @param[in]  cell  The cell (type letter and payload).
 * @param[in]  len   Its length.
 * @param[out] tag   The type letter.
 * @param[out] data  The payload decoded (raw bytes for X, unescaped
 *                   text otherwise), NUL-terminated; free() it.
 * @param[out] dlen  Payload length.
 *
 * @return 0, or TE_EINVAL for a malformed cell.
 */
extern te_errno ta_sql_cell_decode(const char *cell, size_t len, char *tag,
                                   char **data, size_t *dlen);

/**
 * Build the normalised answer of an introspection request.
 *
 * @return 0 on success, -1 with @p err filled on failure.
 */
extern int ta_sql_meta_build(const ta_sql_driver *drv, void *dc, int kind,
                             const char *table, te_string *rows,
                             ta_sql_errinfo *err);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TA_SQL_INT_H__ */
