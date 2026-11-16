/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: performance measurements
 *
 * @defgroup tapi_sql_perf Performance measurements (tapi_sql_perf)
 * @ingroup tapi_sql
 * @{
 *
 * Latency of queries, rows per second, transaction throughput,
 * connection establishment latency, error rate and timeout frequency,
 * collected into a #tapi_sql_perf and reported as count, minimum,
 * maximum, mean and any percentiles asked for - in the log as a TE
 * measurement artifact with a line graph of the samples.
 *
 * A statement's latency is measured on the agent, around the driver
 * call, so the RPC between engine and agent is not in it. Transactions
 * and connections span several calls and are timed on the engine.
 *
 * This is an instrument for a functional suite that wants numbers next
 * to its verdicts. It runs one statement at a time from one session and
 * does not replace a database load generator.
 */

#ifndef __TAPI_SQL_PERF_H__
#define __TAPI_SQL_PERF_H__

#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"

#include "tapi_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A series of measured operations. */
typedef struct tapi_sql_perf {
    char         name[64];      /**< What was measured. */
    double      *samples;       /**< Latency of each success, in us. */
    unsigned int count;         /**< Successful operations. */
    unsigned int cap;           /**< Samples allocated. */
    unsigned int errors;        /**< Failed operations, timeouts
                                     included. */
    unsigned int timeouts;      /**< Operations past a deadline. */
    uint64_t     rows;          /**< Rows processed by the successes. */
    double       busy_us;       /**< Sum of the latencies. */
} tapi_sql_perf;

/** What a series comes to. */
typedef struct tapi_sql_perf_stats {
    unsigned int count;         /**< Successful operations. */
    unsigned int errors;        /**< Failed operations. */
    unsigned int timeouts;      /**< Of which past a deadline. */
    double       min_us;        /**< Fastest. */
    double       max_us;        /**< Slowest. */
    double       mean_us;       /**< Mean latency. */
    double       ops_per_sec;   /**< Operations per second of busy time
                                     (the throughput of one session). */
    double       rows_per_sec;  /**< Rows per second of busy time. */
    double       error_rate;    /**< errors / attempts, 0..1. */
    double       timeout_rate;  /**< timeouts / attempts, 0..1. */
} tapi_sql_perf_stats;

/** Start a series. */
extern void tapi_sql_perf_init(tapi_sql_perf *perf, const char *name);

/** Release a series. */
extern void tapi_sql_perf_free(tapi_sql_perf *perf);

/** Record a successful operation. */
extern void tapi_sql_perf_add(tapi_sql_perf *perf, double latency_us,
                              uint64_t rows);

/** Record a failed operation; TE_ETIMEDOUT counts as a timeout too. */
extern void tapi_sql_perf_add_error(tapi_sql_perf *perf, te_errno rc);

/** Count, minimum, maximum, mean and the rates of a series. */
extern void tapi_sql_perf_stats_get(const tapi_sql_perf *perf,
                                    tapi_sql_perf_stats *stats);

/**
 * A latency percentile (nearest rank).
 *
 * @param perf  Series.
 * @param pct   Percentile, 0 < @p pct <= 100.
 *
 * @return The latency in microseconds; 0 for an empty series.
 */
extern double tapi_sql_perf_percentile(const tapi_sql_perf *perf,
                                       double pct);

/**
 * Query latency: execute a statement @p iterations times, recording the
 * latency of each and the rows it changed. A failure is recorded and
 * the run goes on.
 */
extern te_errno tapi_sql_perf_run(tapi_sql_conn *conn, const char *sql,
                                  unsigned int iterations,
                                  tapi_sql_perf *perf);

/**
 * Rows processed per second: run a query @p iterations times and read
 * each result to its end in batches of @p batch_rows, recording the
 * time and the rows read.
 */
extern te_errno tapi_sql_perf_run_query(tapi_sql_conn *conn, const char *sql,
                                        unsigned int iterations,
                                        unsigned int batch_rows,
                                        tapi_sql_perf *perf);

/**
 * Transaction throughput: begin, the statements, commit - @p iterations
 * times. A transaction with a failing statement is rolled back and
 * recorded as an error.
 */
extern te_errno tapi_sql_perf_run_tx(tapi_sql_conn *conn,
                                     const char *const *stmts,
                                     unsigned int nstmts,
                                     unsigned int iterations,
                                     tapi_sql_perf *perf);

/**
 * Connection establishment latency: connect and disconnect
 * @p iterations times.
 */
extern te_errno tapi_sql_perf_run_connect(rcf_rpc_server *rpcs,
                                          const tapi_sql_config *cfg,
                                          unsigned int iterations,
                                          tapi_sql_perf *perf);

/**
 * Report a series: a measurement artifact with count, minimum, maximum,
 * mean, the percentiles asked for, the rates, and a line graph of the
 * samples in the order they were taken.
 *
 * @param perf         Series.
 * @param percentiles  Percentiles to report (e.g. 50, 95, 99).
 * @param n            How many.
 */
extern void tapi_sql_perf_log(const tapi_sql_perf *perf,
                              const double *percentiles, unsigned int n);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_PERF_H__ */

/** @} */
