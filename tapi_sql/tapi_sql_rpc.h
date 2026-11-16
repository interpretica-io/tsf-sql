/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: RPC client wrappers
 *
 * Client wrappers of the sql_* RPCs, see sql_rpc.x.m4. Tests use
 * tapi_sql.h; these are the calls behind it, one per RPC. Scalars come
 * back by pointer, text (rows, column lists, messages) in a te_string
 * the engine side parses. A connection and a statement are identified
 * by the integer handles sql_connect / sql_prepare returned.
 *
 * The connection secret is an argument of rpc_sql_connect() and is the
 * one argument these wrappers never log.
 */

#ifndef __TAPI_SQL_RPC_H__
#define __TAPI_SQL_RPC_H__

#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What rpc_sql_connect() needs; strings may be @c NULL. */
typedef struct rpc_sql_connect_args {
    int         engine;
    const char *host;
    int         port;
    const char *dbname;
    const char *user;
    const char *password;
    int         tls_mode;
    const char *tls_ca;
    const char *tls_cert;
    const char *tls_key;
    int         timeout_ms;
} rpc_sql_connect_args;

/** A database failure as the agent reports it. */
typedef struct rpc_sql_errinfo {
    int  cls;
    int  native;
    char sqlstate[8];
    char message[1024];
} rpc_sql_errinfo;

/** sql_available(): driver mask and client versions; 0 when any. */
extern te_errno rpc_sql_available(rcf_rpc_server *rpcs,
                                  unsigned int *engines,
                                  te_string *versions);

/** sql_connect(): open a connection; @p err is filled on failure. */
extern te_errno rpc_sql_connect(rcf_rpc_server *rpcs,
                                const rpc_sql_connect_args *args,
                                int *handle, uint64_t *elapsed_us,
                                rpc_sql_errinfo *err);

/** sql_disconnect(): close a connection and its statements. */
extern te_errno rpc_sql_disconnect(rcf_rpc_server *rpcs, int handle);

/** sql_ping(): round trip to the server. */
extern te_errno rpc_sql_ping(rcf_rpc_server *rpcs, int handle,
                             uint64_t *elapsed_us);

/** sql_capabilities(): driver capabilities and server version. */
extern te_errno rpc_sql_capabilities(rcf_rpc_server *rpcs, int handle,
                                     unsigned int *caps,
                                     unsigned int *isolation,
                                     unsigned int *default_iso, int *tls,
                                     te_string *server);

/** sql_execute(): run a statement, discard rows. */
extern te_errno rpc_sql_execute(rcf_rpc_server *rpcs, int handle,
                                const char *sql, int64_t *affected,
                                uint64_t *elapsed_us);

/** sql_prepare(): prepare a statement with "?" parameters. */
extern te_errno rpc_sql_prepare(rcf_rpc_server *rpcs, int handle,
                                const char *sql, int *stmt, int *nparams);

/** sql_bind(): bind parameter @p index (1-based) to an encoded cell. */
extern te_errno rpc_sql_bind(rcf_rpc_server *rpcs, int stmt, int index,
                             const char *cell);

/** sql_stmt_execute(): run a prepared statement. */
extern te_errno rpc_sql_stmt_execute(rcf_rpc_server *rpcs, int stmt,
                                     int *ncols, te_string *columns,
                                     int64_t *affected,
                                     uint64_t *elapsed_us);

/** sql_fetch(): the next bounded batch of rows. */
extern te_errno rpc_sql_fetch(rcf_rpc_server *rpcs, int stmt, int max_rows,
                              int max_bytes, te_string *rows, int *nrows,
                              bool *eof);

/** sql_stmt_free(): release a statement. */
extern te_errno rpc_sql_stmt_free(rcf_rpc_server *rpcs, int stmt);

/** sql_tx(): transaction control. */
extern te_errno rpc_sql_tx(rcf_rpc_server *rpcs, int handle, int op,
                           unsigned int isolation, const char *name);

/** sql_meta(): normalised introspection rows. */
extern te_errno rpc_sql_meta(rcf_rpc_server *rpcs, int handle, int kind,
                             const char *table, te_string *rows);

/** sql_set_timeout(): statement or lock-wait deadline. */
extern te_errno rpc_sql_set_timeout(rcf_rpc_server *rpcs, int handle,
                                    int kind, int ms);

/** sql_async_start(): start a statement on an agent thread. */
extern te_errno rpc_sql_async_start(rcf_rpc_server *rpcs, int handle,
                                    const char *sql);

/** sql_async_wait(): collect it; TE_EINPROGRESS while it runs. */
extern te_errno rpc_sql_async_wait(rcf_rpc_server *rpcs, int handle,
                                   int timeout_ms, bool *done,
                                   int64_t *affected, uint64_t *elapsed_us);

/** sql_cancel(): stop what the connection runs. */
extern te_errno rpc_sql_cancel(rcf_rpc_server *rpcs, int handle);

/** sql_error(): the last database failure on a connection. */
extern te_errno rpc_sql_error(rcf_rpc_server *rpcs, int handle,
                              rpc_sql_errinfo *err);

/** sql_stats(): what the RPC server still holds. */
extern te_errno rpc_sql_stats(rcf_rpc_server *rpcs, int *conns, int *stmts,
                              uint64_t *max_batch);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_RPC_H__ */
