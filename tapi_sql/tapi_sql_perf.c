/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: performance measurements
 */

#define TE_LGR_USER     "TAPI SQL PERF"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_mi_log.h"
#include "logger_api.h"

#include "tapi_sql.h"
#include "tapi_sql_perf.h"

/* Engine-side monotonic clock, in microseconds. */
static double
now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_init(tapi_sql_perf *perf, const char *name)
{
    memset(perf, 0, sizeof(*perf));
    te_strlcpy(perf->name, name != NULL ? name : "sql", sizeof(perf->name));
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_free(tapi_sql_perf *perf)
{
    free(perf->samples);
    perf->samples = NULL;
    perf->count = 0;
    perf->cap = 0;
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_add(tapi_sql_perf *perf, double latency_us, uint64_t rows)
{
    if (perf->count == perf->cap)
    {
        perf->cap = perf->cap == 0 ? 64 : perf->cap * 2;
        perf->samples = realloc(perf->samples,
                                perf->cap * sizeof(*perf->samples));
        if (perf->samples == NULL)
            TE_FATAL_ERROR("Out of memory");
    }
    perf->samples[perf->count++] = latency_us;
    perf->busy_us += latency_us;
    perf->rows += rows;
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_add_error(tapi_sql_perf *perf, te_errno rc)
{
    perf->errors++;
    if (TE_RC_GET_ERROR(rc) == TE_ETIMEDOUT)
        perf->timeouts++;
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_stats_get(const tapi_sql_perf *perf,
                        tapi_sql_perf_stats *stats)
{
    unsigned int attempts = perf->count + perf->errors;
    unsigned int i;

    memset(stats, 0, sizeof(*stats));
    stats->count = perf->count;
    stats->errors = perf->errors;
    stats->timeouts = perf->timeouts;
    if (attempts > 0)
    {
        stats->error_rate = (double)perf->errors / attempts;
        stats->timeout_rate = (double)perf->timeouts / attempts;
    }
    if (perf->count == 0)
        return;

    stats->min_us = perf->samples[0];
    stats->max_us = perf->samples[0];
    for (i = 1; i < perf->count; i++)
    {
        stats->min_us = fmin(stats->min_us, perf->samples[i]);
        stats->max_us = fmax(stats->max_us, perf->samples[i]);
    }
    stats->mean_us = perf->busy_us / perf->count;
    if (perf->busy_us > 0.0)
    {
        stats->ops_per_sec = perf->count * 1e6 / perf->busy_us;
        stats->rows_per_sec = (double)perf->rows * 1e6 / perf->busy_us;
    }
}

static int
cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return x < y ? -1 : x > y ? 1 : 0;
}

/* See description in tapi_sql_perf.h */
double
tapi_sql_perf_percentile(const tapi_sql_perf *perf, double pct)
{
    double      *sorted;
    unsigned int rank;
    double       value;

    if (perf->count == 0 || pct <= 0.0)
        return 0.0;
    if (pct > 100.0)
        pct = 100.0;

    sorted = TE_ALLOC(perf->count * sizeof(*sorted));
    memcpy(sorted, perf->samples, perf->count * sizeof(*sorted));
    qsort(sorted, perf->count, sizeof(*sorted), cmp_double);

    rank = (unsigned int)ceil(pct / 100.0 * perf->count);
    if (rank < 1)
        rank = 1;
    value = sorted[rank - 1];
    free(sorted);
    return value;
}

/* See description in tapi_sql_perf.h */
te_errno
tapi_sql_perf_run(tapi_sql_conn *conn, const char *sql,
                  unsigned int iterations, tapi_sql_perf *perf)
{
    unsigned int i;

    for (i = 0; i < iterations; i++)
    {
        int64_t  affected = 0;
        te_errno rc = tapi_sql_execute(conn, sql, &affected);

        if (rc != 0)
            tapi_sql_perf_add_error(perf, rc);
        else
            tapi_sql_perf_add(perf, (double)tapi_sql_last_elapsed_us(conn),
                              affected > 0 ? (uint64_t)affected : 0);
    }
    return 0;
}

/* See description in tapi_sql_perf.h */
te_errno
tapi_sql_perf_run_query(tapi_sql_conn *conn, const char *sql,
                        unsigned int iterations, unsigned int batch_rows,
                        tapi_sql_perf *perf)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    tapi_sql_stmt  *stmt = NULL;
    unsigned int    i;
    te_errno        rc;

    rc = tapi_sql_prepare(conn, sql, &stmt);
    if (rc != 0)
        return rc;

    for (i = 0; i < iterations; i++)
    {
        double   t0 = now_us();
        uint64_t rows = 0;
        bool     eof = false;

        rc = tapi_sql_stmt_execute(stmt, NULL);
        while (rc == 0 && !eof)
        {
            rc = tapi_sql_fetch(stmt, batch_rows, &res, &eof);
            rows += res.nrows;
        }
        if (rc != 0)
            tapi_sql_perf_add_error(perf, rc);
        else
            tapi_sql_perf_add(perf, now_us() - t0, rows);
    }

    tapi_sql_result_free(&res);
    tapi_sql_statement_free(stmt);
    return 0;
}

/* See description in tapi_sql_perf.h */
te_errno
tapi_sql_perf_run_tx(tapi_sql_conn *conn, const char *const *stmts,
                     unsigned int nstmts, unsigned int iterations,
                     tapi_sql_perf *perf)
{
    unsigned int i;
    unsigned int j;

    for (i = 0; i < iterations; i++)
    {
        double   t0 = now_us();
        uint64_t rows = 0;
        te_errno rc = tapi_sql_begin(conn);

        for (j = 0; j < nstmts && rc == 0; j++)
        {
            int64_t affected = 0;

            rc = tapi_sql_execute(conn, stmts[j], &affected);
            rows += affected > 0 ? (uint64_t)affected : 0;
        }
        if (rc == 0)
            rc = tapi_sql_commit(conn);
        else
            tapi_sql_rollback(conn);

        if (rc != 0)
            tapi_sql_perf_add_error(perf, rc);
        else
            tapi_sql_perf_add(perf, now_us() - t0, rows);
    }
    return 0;
}

/* See description in tapi_sql_perf.h */
te_errno
tapi_sql_perf_run_connect(rcf_rpc_server *rpcs, const tapi_sql_config *cfg,
                          unsigned int iterations, tapi_sql_perf *perf)
{
    unsigned int i;

    for (i = 0; i < iterations; i++)
    {
        tapi_sql_conn *conn = NULL;
        te_errno       rc = tapi_sql_connect_err(rpcs, cfg, &conn, NULL);

        if (rc != 0)
        {
            tapi_sql_perf_add_error(perf, rc);
            continue;
        }
        tapi_sql_perf_add(perf, (double)tapi_sql_last_elapsed_us(conn), 0);
        tapi_sql_disconnect(conn);
    }
    return 0;
}

/* See description in tapi_sql_perf.h */
void
tapi_sql_perf_log(const tapi_sql_perf *perf, const double *percentiles,
                  unsigned int n)
{
    tapi_sql_perf_stats stats;
    te_mi_logger       *logger = NULL;
    te_string           summary = TE_STRING_INIT;
    char                name[96];
    unsigned int        i;

    tapi_sql_perf_stats_get(perf, &stats);

    te_string_append(&summary, "%s: count=%u errors=%u timeouts=%u "
                     "min=%.0fus max=%.0fus mean=%.0fus", perf->name,
                     stats.count, stats.errors, stats.timeouts, stats.min_us,
                     stats.max_us, stats.mean_us);
    for (i = 0; i < n; i++)
        te_string_append(&summary, " p%g=%.0fus", percentiles[i],
                         tapi_sql_perf_percentile(perf, percentiles[i]));
    te_string_append(&summary, " ops/s=%.1f rows/s=%.1f error-rate=%.3f "
                     "timeout-rate=%.3f", stats.ops_per_sec,
                     stats.rows_per_sec, stats.error_rate,
                     stats.timeout_rate);
    RING("%s", summary.ptr);
    te_string_free(&summary);

    if (te_mi_logger_meas_create("tsf-sql", &logger) != 0)
        return;

    te_mi_logger_add_meas_key(logger, NULL, "series", "%s", perf->name);
    te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_EPE, "operations",
                          TE_MI_MEAS_AGGR_SINGLE, stats.count,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
    te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_EPE, "errors",
                          TE_MI_MEAS_AGGR_SINGLE, stats.errors,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
    te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_EPE, "timeouts",
                          TE_MI_MEAS_AGGR_SINGLE, stats.timeouts,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
    te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_RPS, "operations/s",
                          TE_MI_MEAS_AGGR_MEAN, stats.ops_per_sec,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);
    te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_RPS, "rows/s",
                          TE_MI_MEAS_AGGR_MEAN, stats.rows_per_sec,
                          TE_MI_MEAS_MULTIPLIER_PLAIN);

    if (stats.count > 0)
    {
        te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_LATENCY, "summary",
                              TE_MI_MEAS_AGGR_MIN, stats.min_us,
                              TE_MI_MEAS_MULTIPLIER_MICRO);
        te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_LATENCY, "summary",
                              TE_MI_MEAS_AGGR_MAX, stats.max_us,
                              TE_MI_MEAS_MULTIPLIER_MICRO);
        te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_LATENCY, "summary",
                              TE_MI_MEAS_AGGR_MEAN, stats.mean_us,
                              TE_MI_MEAS_MULTIPLIER_MICRO);
        for (i = 0; i < n; i++)
        {
            snprintf(name, sizeof(name), "p%g", percentiles[i]);
            te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_LATENCY, name,
                                  TE_MI_MEAS_AGGR_PERCENTILE,
                                  tapi_sql_perf_percentile(perf,
                                                           percentiles[i]),
                                  TE_MI_MEAS_MULTIPLIER_MICRO);
        }

        /* The samples in the order taken, and the graph over them. */
        for (i = 0; i < perf->count; i++)
            te_mi_logger_add_meas(logger, NULL, TE_MI_MEAS_LATENCY,
                                  "latency", TE_MI_MEAS_AGGR_SINGLE,
                                  perf->samples[i],
                                  TE_MI_MEAS_MULTIPLIER_MICRO);

        snprintf(name, sizeof(name), "%s latency", perf->name);
        te_mi_logger_add_meas_view(logger, NULL, TE_MI_MEAS_VIEW_LINE_GRAPH,
                                   "latency", name);
        te_mi_logger_meas_graph_axis_add_name(logger, NULL,
                                   TE_MI_MEAS_VIEW_LINE_GRAPH, "latency",
                                   TE_MI_GRAPH_AXIS_X,
                                   TE_MI_GRAPH_AUTO_SEQNO);
        te_mi_logger_meas_graph_axis_add(logger, NULL,
                                   TE_MI_MEAS_VIEW_LINE_GRAPH, "latency",
                                   TE_MI_GRAPH_AXIS_Y, TE_MI_MEAS_LATENCY,
                                   "latency");
    }
    te_mi_logger_destroy(logger);
}
