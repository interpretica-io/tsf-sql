/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: optional bridge to tsf-cybersec
 *
 * For a suite that builds tsf-cybersec and wants the database findings
 * in the same report as its other audits (one report per test, one
 * verdict). Header-only on purpose: tapi_sql does not link
 * tapi_cybersec, so a suite without it loses nothing. Include this
 * header only where tapi_cybersec.h is available.
 */

#ifndef __TAPI_SQL_CYBERSEC_H__
#define __TAPI_SQL_CYBERSEC_H__

#include "tapi_cybersec.h"

#include "tapi_sql_audit.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copy the findings of an SQL report into a tsf-cybersec report. The
 * severities are the same scale and map one to one.
 *
 * @param src  SQL findings.
 * @param dst  The report they go into.
 */
static inline void
tapi_sql_report_to_cybersec(const tapi_sql_report *src,
                            tapi_cybersec_report *dst)
{
    unsigned int i;

    for (i = 0; i < src->n; i++)
    {
        tapi_cybersec_report_add(dst,
                (tapi_cybersec_severity)src->items[i].severity,
                src->items[i].check, src->items[i].subject, "%s",
                src->items[i].detail != NULL ? src->items[i].detail : "");
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_CYBERSEC_H__ */
