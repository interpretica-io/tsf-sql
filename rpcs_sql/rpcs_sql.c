/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL RPC server library
 *
 * The sql_* RPCs (see sql_rpc.x.m4) on top of ta_sql.
 * TARPC_FUNC_STATIC() binds an RPC to the function of the same name, so
 * each RPC has a plain C function first and the generated wrapper after
 * it. The functions move the te_string results (rows, column lists,
 * messages) into the RPC's string fields; scalars go straight into the
 * out struct. The connections and statements themselves live in ta_sql,
 * keyed by the integer handles these calls pass through.
 *
 * Nothing here logs an argument: the connection secret passes through
 * sql_connect and must not reach a log.
 */

#define TE_LGR_USER     "RPC SQL"

#include "te_config.h"

#include <string.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "rpc_server.h"

#include "ta_sql.h"

/* Hand a te_string result over to an RPC string field (never NULL). */
static char *
take(te_string *str)
{
    return str->ptr != NULL ? str->ptr : TE_STRDUP("");
}

/* ---- available ---------------------------------------------------- */

static te_errno
sql_available(unsigned int *engines, char **versions)
{
    te_string versions_s = TE_STRING_INIT;
    bool      ok = ta_sql_available(engines, &versions_s);

    *versions = take(&versions_s);

    return ok ? 0 : TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
}

TARPC_FUNC_STATIC(sql_available, {},
{
    unsigned int engines = 0;

    MAKE_CALL(out->retval = func(&engines, &out->versions));
    out->engines = engines;
    out->common.errno_changed = false;
})

/* ---- connect / disconnect / ping ---------------------------------- */

static te_errno
sql_connect(const tarpc_sql_connect_in *in, tarpc_sql_connect_out *out)
{
    ta_sql_config  cfg;
    ta_sql_errinfo err;
    uint64_t       elapsed = 0;
    int            handle = -1;
    te_errno       rc;

    memset(&cfg, 0, sizeof(cfg));
    memset(&err, 0, sizeof(err));
    cfg.engine = in->engine;
    cfg.host = in->host;
    cfg.port = in->port;
    cfg.dbname = in->dbname;
    cfg.user = in->user;
    cfg.password = in->password;
    cfg.tls_mode = in->tls_mode;
    cfg.tls_ca = in->tls_ca;
    cfg.tls_cert = in->tls_cert;
    cfg.tls_key = in->tls_key;
    cfg.timeout_ms = in->timeout_ms;

    rc = ta_sql_connect(&cfg, &handle, &elapsed, &err);

    out->handle = handle;
    out->elapsed_us = elapsed;
    out->err_cls = err.cls;
    out->err_native = err.native;
    out->err_sqlstate = TE_STRDUP(err.sqlstate);
    out->err_message = TE_STRDUP(err.message);
    return rc;
}

TARPC_FUNC_STATIC(sql_connect, {},
{
    MAKE_CALL(out->retval = func(in, out));
    out->common.errno_changed = false;
})

static te_errno
sql_disconnect(int handle)
{
    return ta_sql_disconnect(handle);
}

TARPC_FUNC_STATIC(sql_disconnect, {},
{
    MAKE_CALL(out->retval = func(in->handle));
    out->common.errno_changed = false;
})

static te_errno
sql_ping(int handle, uint64_t *elapsed)
{
    return ta_sql_ping(handle, elapsed);
}

TARPC_FUNC_STATIC(sql_ping, {},
{
    uint64_t elapsed = 0;

    MAKE_CALL(out->retval = func(in->handle, &elapsed));
    out->elapsed_us = elapsed;
    out->common.errno_changed = false;
})

/* ---- capabilities ------------------------------------------------- */

static te_errno
sql_capabilities(int handle, tarpc_sql_capabilities_out *out)
{
    te_string    server = TE_STRING_INIT;
    unsigned int caps = 0;
    unsigned int isolation = 0;
    unsigned int default_iso = 0;
    int          tls = 0;
    te_errno     rc;

    rc = ta_sql_capabilities(handle, &caps, &isolation, &default_iso, &tls,
                             &server);
    out->caps = caps;
    out->isolation = isolation;
    out->default_iso = default_iso;
    out->tls = tls;
    out->server = take(&server);
    return rc;
}

TARPC_FUNC_STATIC(sql_capabilities, {},
{
    MAKE_CALL(out->retval = func(in->handle, out));
    out->common.errno_changed = false;
})

/* ---- execute ------------------------------------------------------ */

static te_errno
sql_execute(int handle, const char *sql, int64_t *affected,
            uint64_t *elapsed)
{
    return ta_sql_execute(handle, sql, affected, elapsed);
}

TARPC_FUNC_STATIC(sql_execute, {},
{
    int64_t  affected = -1;
    uint64_t elapsed = 0;

    MAKE_CALL(out->retval = func(in->handle, in->sql, &affected, &elapsed));
    out->affected = affected;
    out->elapsed_us = elapsed;
    out->common.errno_changed = false;
})

/* ---- prepared statements ------------------------------------------ */

static te_errno
sql_prepare(int handle, const char *sql, int *stmt, int *nparams)
{
    return ta_sql_prepare(handle, sql, stmt, nparams);
}

TARPC_FUNC_STATIC(sql_prepare, {},
{
    int stmt = -1;
    int nparams = 0;

    MAKE_CALL(out->retval = func(in->handle, in->sql, &stmt, &nparams));
    out->stmt = stmt;
    out->nparams = nparams;
    out->common.errno_changed = false;
})

static te_errno
sql_bind(int stmt, int index, const char *cell)
{
    return ta_sql_bind(stmt, index, cell);
}

TARPC_FUNC_STATIC(sql_bind, {},
{
    MAKE_CALL(out->retval = func(in->stmt, in->index, in->cell));
    out->common.errno_changed = false;
})

static te_errno
sql_stmt_execute(int stmt, tarpc_sql_stmt_execute_out *out)
{
    te_string columns = TE_STRING_INIT;
    int64_t   affected = -1;
    uint64_t  elapsed = 0;
    int       ncols = 0;
    te_errno  rc;

    rc = ta_sql_stmt_execute(stmt, &ncols, &columns, &affected, &elapsed);
    out->ncols = ncols;
    out->columns = take(&columns);
    out->affected = affected;
    out->elapsed_us = elapsed;
    return rc;
}

TARPC_FUNC_STATIC(sql_stmt_execute, {},
{
    MAKE_CALL(out->retval = func(in->stmt, out));
    out->common.errno_changed = false;
})

static te_errno
sql_fetch(int stmt, int max_rows, int max_bytes, tarpc_sql_fetch_out *out)
{
    te_string rows = TE_STRING_INIT;
    int       nrows = 0;
    int       eof = 0;
    te_errno  rc;

    rc = ta_sql_fetch(stmt, max_rows, max_bytes, &rows, &nrows, &eof);
    out->rows = take(&rows);
    out->nrows = nrows;
    out->eof = eof;
    return rc;
}

TARPC_FUNC_STATIC(sql_fetch, {},
{
    MAKE_CALL(out->retval = func(in->stmt, in->max_rows, in->max_bytes, out));
    out->common.errno_changed = false;
})

static te_errno
sql_stmt_free(int stmt)
{
    return ta_sql_stmt_free(stmt);
}

TARPC_FUNC_STATIC(sql_stmt_free, {},
{
    MAKE_CALL(out->retval = func(in->stmt));
    out->common.errno_changed = false;
})

/* ---- transactions, introspection ---------------------------------- */

static te_errno
sql_tx(int handle, int op, unsigned int isolation, const char *name)
{
    return ta_sql_tx(handle, op, isolation, name);
}

TARPC_FUNC_STATIC(sql_tx, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->op, in->isolation,
                                 in->name));
    out->common.errno_changed = false;
})

static te_errno
sql_meta(int handle, int kind, const char *table, char **rows)
{
    te_string rows_s = TE_STRING_INIT;
    te_errno  rc = ta_sql_meta(handle, kind, table, &rows_s);

    *rows = take(&rows_s);

    return rc;
}

TARPC_FUNC_STATIC(sql_meta, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->kind, in->table,
                                 &out->rows));
    out->common.errno_changed = false;
})

/* ---- deadlines, asynchronous statements, cancellation ------------- */

static te_errno
sql_set_timeout(int handle, int kind, int ms)
{
    return ta_sql_set_timeout(handle, kind, ms);
}

TARPC_FUNC_STATIC(sql_set_timeout, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->kind, in->ms));
    out->common.errno_changed = false;
})

static te_errno
sql_async_start(int handle, const char *sql)
{
    return ta_sql_async_start(handle, sql);
}

TARPC_FUNC_STATIC(sql_async_start, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->sql));
    out->common.errno_changed = false;
})

static te_errno
sql_async_wait(int handle, int timeout_ms, tarpc_sql_async_wait_out *out)
{
    int64_t  affected = -1;
    uint64_t elapsed = 0;
    int      done = 0;
    te_errno rc;

    rc = ta_sql_async_wait(handle, timeout_ms, &done, &affected, &elapsed);
    out->done = done;
    out->affected = affected;
    out->elapsed_us = elapsed;
    return rc;
}

TARPC_FUNC_STATIC(sql_async_wait, {},
{
    MAKE_CALL(out->retval = func(in->handle, in->timeout_ms, out));
    out->common.errno_changed = false;
})

static te_errno
sql_cancel(int handle)
{
    return ta_sql_cancel(handle);
}

TARPC_FUNC_STATIC(sql_cancel, {},
{
    MAKE_CALL(out->retval = func(in->handle));
    out->common.errno_changed = false;
})

/* ---- last error, statistics --------------------------------------- */

static te_errno
sql_error(int handle, tarpc_sql_error_out *out)
{
    ta_sql_errinfo err;
    te_errno       rc;

    memset(&err, 0, sizeof(err));
    rc = ta_sql_error(handle, &err);
    out->cls = err.cls;
    out->native = err.native;
    out->sqlstate = TE_STRDUP(err.sqlstate);
    out->message = TE_STRDUP(err.message);
    return rc;
}

TARPC_FUNC_STATIC(sql_error, {},
{
    MAKE_CALL(out->retval = func(in->handle, out));
    out->common.errno_changed = false;
})

static te_errno
sql_stats(tarpc_sql_stats_out *out)
{
    uint64_t max_batch = 0;
    int      conns = 0;
    int      stmts = 0;

    ta_sql_stats(&conns, &stmts, &max_batch);
    out->conns = conns;
    out->stmts = stmts;
    out->max_batch = max_batch;
    return 0;
}

TARPC_FUNC_STATIC(sql_stats, {},
{
    MAKE_CALL(out->retval = func(out));
    out->common.errno_changed = false;
})
