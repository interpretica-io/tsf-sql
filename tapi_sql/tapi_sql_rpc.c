/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: RPC client wrappers
 *
 * The rcf_rpc_call() boilerplate behind tapi_sql. The RPCs return
 * te_errno; an RPC transport failure is mapped to TE_ECORRUPTED.
 *
 * A statement text is logged, cut short; a bound value, a fetched row
 * and above all the connection secret are not.
 */

#define TE_LGR_USER     "TAPI SQL RPC"

#include "te_config.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_time.h"
#include "te_string.h"
#include "logger_api.h"
#include "tapi_rpc_internal.h"
#include "tarpc.h"

#include "tapi_sql_rpc.h"

#define CHECK_RPC_ERRNO_UNCHANGED(_func, _var) \
    CHECK_RETVAL_VAR_ERR_COND(_func, _var, false,                    \
                              TE_RC(TE_TAPI, TE_ECORRUPTED), false)

/** How much of a statement goes into the log. */
#define SQL_LOG_LEN     200

/* An RPC string argument is never NULL. */
static char *
arg(const char *s)
{
    return (char *)(s != NULL ? s : "");
}

/* Append an RPC string result, when there is one. */
static void
take_string(te_string *dst, const char *src)
{
    if (dst != NULL && src != NULL)
        te_string_append(dst, "%s", src);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_available(rcf_rpc_server *rpcs, unsigned int *engines,
                  te_string *versions)
{
    tarpc_sql_available_in  in;
    tarpc_sql_available_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    rcf_rpc_call(rpcs, "sql_available", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_available, out.retval);
    TAPI_RPC_LOG(rpcs, sql_available, "", "%r engines=0x%x (%s)", out.retval,
                 out.engines, out.versions != NULL ? out.versions : "");

    if (engines != NULL)
        *engines = out.engines;
    take_string(versions, out.versions);

    RETVAL_TE_ERRNO(sql_available, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_connect(rcf_rpc_server *rpcs, const rpc_sql_connect_args *args,
                int *handle, uint64_t *elapsed_us, rpc_sql_errinfo *err)
{
    tarpc_sql_connect_in  in;
    tarpc_sql_connect_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.engine = args->engine;
    in.host = arg(args->host);
    in.port = args->port;
    in.dbname = arg(args->dbname);
    in.user = arg(args->user);
    in.password = arg(args->password);
    in.tls_mode = args->tls_mode;
    in.tls_ca = arg(args->tls_ca);
    in.tls_cert = arg(args->tls_cert);
    in.tls_key = arg(args->tls_key);
    in.timeout_ms = args->timeout_ms;

    rcf_rpc_call(rpcs, "sql_connect", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_connect, out.retval);
    /* The secret is deliberately absent from this line. */
    TAPI_RPC_LOG(rpcs, sql_connect,
                 "engine=%d %s:%d db=%s user=%s tls=%d",
                 "%r handle=%d %s", args->engine, arg(args->host),
                 args->port, arg(args->dbname), arg(args->user),
                 args->tls_mode, out.retval, out.handle,
                 out.err_message != NULL ? out.err_message : "");

    if (out.retval == 0)
    {
        if (handle != NULL)
            *handle = out.handle;
        if (elapsed_us != NULL)
            *elapsed_us = out.elapsed_us;
    }
    if (err != NULL)
    {
        memset(err, 0, sizeof(*err));
        err->cls = out.err_cls;
        err->native = out.err_native;
        snprintf(err->sqlstate, sizeof(err->sqlstate), "%s",
                 out.err_sqlstate != NULL ? out.err_sqlstate : "");
        snprintf(err->message, sizeof(err->message), "%s",
                 out.err_message != NULL ? out.err_message : "");
    }

    RETVAL_TE_ERRNO(sql_connect, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_disconnect(rcf_rpc_server *rpcs, int handle)
{
    tarpc_sql_disconnect_in  in;
    tarpc_sql_disconnect_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "sql_disconnect", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_disconnect, out.retval);
    TAPI_RPC_LOG(rpcs, sql_disconnect, "handle=%d", "%r", handle,
                 out.retval);

    RETVAL_TE_ERRNO(sql_disconnect, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_ping(rcf_rpc_server *rpcs, int handle, uint64_t *elapsed_us)
{
    tarpc_sql_ping_in  in;
    tarpc_sql_ping_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "sql_ping", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_ping, out.retval);
    TAPI_RPC_LOG(rpcs, sql_ping, "handle=%d", "%r %llu us", handle,
                 out.retval, (unsigned long long)out.elapsed_us);

    if (out.retval == 0 && elapsed_us != NULL)
        *elapsed_us = out.elapsed_us;

    RETVAL_TE_ERRNO(sql_ping, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_capabilities(rcf_rpc_server *rpcs, int handle, unsigned int *caps,
                     unsigned int *isolation, unsigned int *default_iso,
                     int *tls, te_string *server)
{
    tarpc_sql_capabilities_in  in;
    tarpc_sql_capabilities_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "sql_capabilities", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_capabilities, out.retval);
    TAPI_RPC_LOG(rpcs, sql_capabilities, "handle=%d",
                 "%r caps=0x%x iso=0x%x default=0x%x tls=%d %s", handle,
                 out.retval, out.caps, out.isolation, out.default_iso,
                 out.tls, out.server != NULL ? out.server : "");

    if (out.retval == 0)
    {
        if (caps != NULL)
            *caps = out.caps;
        if (isolation != NULL)
            *isolation = out.isolation;
        if (default_iso != NULL)
            *default_iso = out.default_iso;
        if (tls != NULL)
            *tls = out.tls;
        take_string(server, out.server);
    }

    RETVAL_TE_ERRNO(sql_capabilities, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_execute(rcf_rpc_server *rpcs, int handle, const char *sql,
                int64_t *affected, uint64_t *elapsed_us)
{
    tarpc_sql_execute_in  in;
    tarpc_sql_execute_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.sql = arg(sql);

    rcf_rpc_call(rpcs, "sql_execute", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_execute, out.retval);
    TAPI_RPC_LOG(rpcs, sql_execute, "handle=%d \"%.*s\"",
                 "%r affected=%lld %llu us", handle, SQL_LOG_LEN, arg(sql),
                 out.retval, (long long)out.affected,
                 (unsigned long long)out.elapsed_us);

    if (out.retval == 0 && affected != NULL)
        *affected = out.affected;
    if (elapsed_us != NULL)
        *elapsed_us = out.elapsed_us;

    RETVAL_TE_ERRNO(sql_execute, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_prepare(rcf_rpc_server *rpcs, int handle, const char *sql,
                int *stmt, int *nparams)
{
    tarpc_sql_prepare_in  in;
    tarpc_sql_prepare_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.sql = arg(sql);

    rcf_rpc_call(rpcs, "sql_prepare", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_prepare, out.retval);
    TAPI_RPC_LOG(rpcs, sql_prepare, "handle=%d \"%.*s\"",
                 "%r stmt=%d nparams=%d", handle, SQL_LOG_LEN, arg(sql),
                 out.retval, out.stmt, out.nparams);

    if (out.retval == 0)
    {
        if (stmt != NULL)
            *stmt = out.stmt;
        if (nparams != NULL)
            *nparams = out.nparams;
    }

    RETVAL_TE_ERRNO(sql_prepare, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_bind(rcf_rpc_server *rpcs, int stmt, int index, const char *cell)
{
    tarpc_sql_bind_in  in;
    tarpc_sql_bind_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.stmt = stmt;
    in.index = index;
    in.cell = arg(cell);

    rcf_rpc_call(rpcs, "sql_bind", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_bind, out.retval);
    /* The value may be sensitive: only its type letter is logged. */
    TAPI_RPC_LOG(rpcs, sql_bind, "stmt=%d index=%d type=%c", "%r", stmt,
                 index, cell != NULL && cell[0] != '\0' ? cell[0] : '?',
                 out.retval);

    RETVAL_TE_ERRNO(sql_bind, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_stmt_execute(rcf_rpc_server *rpcs, int stmt, int *ncols,
                     te_string *columns, int64_t *affected,
                     uint64_t *elapsed_us)
{
    tarpc_sql_stmt_execute_in  in;
    tarpc_sql_stmt_execute_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.stmt = stmt;

    rcf_rpc_call(rpcs, "sql_stmt_execute", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_stmt_execute, out.retval);
    TAPI_RPC_LOG(rpcs, sql_stmt_execute, "stmt=%d",
                 "%r ncols=%d affected=%lld %llu us", stmt, out.retval,
                 out.ncols, (long long)out.affected,
                 (unsigned long long)out.elapsed_us);

    if (out.retval == 0)
    {
        if (ncols != NULL)
            *ncols = out.ncols;
        if (affected != NULL)
            *affected = out.affected;
        take_string(columns, out.columns);
    }
    if (elapsed_us != NULL)
        *elapsed_us = out.elapsed_us;

    RETVAL_TE_ERRNO(sql_stmt_execute, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_fetch(rcf_rpc_server *rpcs, int stmt, int max_rows, int max_bytes,
              te_string *rows, int *nrows, bool *eof)
{
    tarpc_sql_fetch_in  in;
    tarpc_sql_fetch_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.stmt = stmt;
    in.max_rows = max_rows;
    in.max_bytes = max_bytes;

    rcf_rpc_call(rpcs, "sql_fetch", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_fetch, out.retval);
    TAPI_RPC_LOG(rpcs, sql_fetch, "stmt=%d max_rows=%d max_bytes=%d",
                 "%r nrows=%d bytes=%u eof=%d", stmt, max_rows, max_bytes,
                 out.retval, out.nrows,
                 (unsigned)(out.rows != NULL ? strlen(out.rows) : 0),
                 out.eof);

    if (out.retval == 0)
    {
        take_string(rows, out.rows);
        if (nrows != NULL)
            *nrows = out.nrows;
        if (eof != NULL)
            *eof = out.eof != 0;
    }

    RETVAL_TE_ERRNO(sql_fetch, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_stmt_free(rcf_rpc_server *rpcs, int stmt)
{
    tarpc_sql_stmt_free_in  in;
    tarpc_sql_stmt_free_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.stmt = stmt;

    rcf_rpc_call(rpcs, "sql_stmt_free", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_stmt_free, out.retval);
    TAPI_RPC_LOG(rpcs, sql_stmt_free, "stmt=%d", "%r", stmt, out.retval);

    RETVAL_TE_ERRNO(sql_stmt_free, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_tx(rcf_rpc_server *rpcs, int handle, int op, unsigned int isolation,
           const char *name)
{
    tarpc_sql_tx_in  in;
    tarpc_sql_tx_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.op = op;
    in.isolation = isolation;
    in.name = arg(name);

    rcf_rpc_call(rpcs, "sql_tx", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_tx, out.retval);
    TAPI_RPC_LOG(rpcs, sql_tx, "handle=%d op=%d isolation=0x%x name=%s",
                 "%r", handle, op, isolation, arg(name), out.retval);

    RETVAL_TE_ERRNO(sql_tx, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_meta(rcf_rpc_server *rpcs, int handle, int kind, const char *table,
             te_string *rows)
{
    tarpc_sql_meta_in  in;
    tarpc_sql_meta_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.kind = kind;
    in.table = arg(table);

    rcf_rpc_call(rpcs, "sql_meta", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_meta, out.retval);
    TAPI_RPC_LOG(rpcs, sql_meta, "handle=%d kind=%d table=%s", "%r", handle,
                 kind, arg(table), out.retval);

    if (out.retval == 0)
        take_string(rows, out.rows);

    RETVAL_TE_ERRNO(sql_meta, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_set_timeout(rcf_rpc_server *rpcs, int handle, int kind, int ms)
{
    tarpc_sql_set_timeout_in  in;
    tarpc_sql_set_timeout_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.kind = kind;
    in.ms = ms;

    rcf_rpc_call(rpcs, "sql_set_timeout", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_set_timeout, out.retval);
    TAPI_RPC_LOG(rpcs, sql_set_timeout, "handle=%d kind=%d ms=%d", "%r",
                 handle, kind, ms, out.retval);

    RETVAL_TE_ERRNO(sql_set_timeout, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_async_start(rcf_rpc_server *rpcs, int handle, const char *sql)
{
    tarpc_sql_async_start_in  in;
    tarpc_sql_async_start_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.sql = arg(sql);

    rcf_rpc_call(rpcs, "sql_async_start", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_async_start, out.retval);
    TAPI_RPC_LOG(rpcs, sql_async_start, "handle=%d \"%.*s\"", "%r", handle,
                 SQL_LOG_LEN, arg(sql), out.retval);

    RETVAL_TE_ERRNO(sql_async_start, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_async_wait(rcf_rpc_server *rpcs, int handle, int timeout_ms,
                   bool *done, int64_t *affected, uint64_t *elapsed_us)
{
    tarpc_sql_async_wait_in  in;
    tarpc_sql_async_wait_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;
    in.timeout_ms = timeout_ms;

    rcf_rpc_call(rpcs, "sql_async_wait", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_async_wait, out.retval);
    TAPI_RPC_LOG(rpcs, sql_async_wait, "handle=%d timeout=%d ms",
                 "%r done=%d affected=%lld %llu us", handle, timeout_ms,
                 out.retval, out.done, (long long)out.affected,
                 (unsigned long long)out.elapsed_us);

    if (done != NULL)
        *done = out.done != 0;
    if (out.retval == 0 && affected != NULL)
        *affected = out.affected;
    if (elapsed_us != NULL)
        *elapsed_us = out.elapsed_us;

    RETVAL_TE_ERRNO(sql_async_wait, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_cancel(rcf_rpc_server *rpcs, int handle)
{
    tarpc_sql_cancel_in  in;
    tarpc_sql_cancel_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "sql_cancel", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_cancel, out.retval);
    TAPI_RPC_LOG(rpcs, sql_cancel, "handle=%d", "%r", handle, out.retval);

    RETVAL_TE_ERRNO(sql_cancel, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_error(rcf_rpc_server *rpcs, int handle, rpc_sql_errinfo *err)
{
    tarpc_sql_error_in  in;
    tarpc_sql_error_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.handle = handle;

    rcf_rpc_call(rpcs, "sql_error", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_error, out.retval);
    TAPI_RPC_LOG(rpcs, sql_error, "handle=%d",
                 "%r class=%d sqlstate=%s native=%d: %s", handle, out.retval,
                 out.cls, out.sqlstate != NULL ? out.sqlstate : "",
                 out.native, out.message != NULL ? out.message : "");

    if (out.retval == 0 && err != NULL)
    {
        memset(err, 0, sizeof(*err));
        err->cls = out.cls;
        err->native = out.native;
        snprintf(err->sqlstate, sizeof(err->sqlstate), "%s",
                 out.sqlstate != NULL ? out.sqlstate : "");
        snprintf(err->message, sizeof(err->message), "%s",
                 out.message != NULL ? out.message : "");
    }

    RETVAL_TE_ERRNO(sql_error, out.retval);
}

/* See description in tapi_sql_rpc.h */
te_errno
rpc_sql_stats(rcf_rpc_server *rpcs, int *conns, int *stmts,
              uint64_t *max_batch)
{
    tarpc_sql_stats_in  in;
    tarpc_sql_stats_out out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    rcf_rpc_call(rpcs, "sql_stats", &in, &out);
    CHECK_RPC_ERRNO_UNCHANGED(sql_stats, out.retval);
    TAPI_RPC_LOG(rpcs, sql_stats, "", "%r conns=%d stmts=%d max_batch=%llu",
                 out.retval, out.conns, out.stmts,
                 (unsigned long long)out.max_batch);

    if (out.retval == 0)
    {
        if (conns != NULL)
            *conns = out.conns;
        if (stmts != NULL)
            *stmts = out.stmts;
        if (max_batch != NULL)
            *max_batch = out.max_batch;
    }

    RETVAL_TE_ERRNO(sql_stats, out.retval);
}
