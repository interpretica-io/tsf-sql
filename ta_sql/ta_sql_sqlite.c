/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief ta_sql driver: SQLite, over the SQLite C API
 *
 * The database is a file on the agent (or ":memory:"). SQLite locks
 * the whole database for a writer, so there are no row locks and no
 * deadlock detection: a second writer gets "busy" at once, or after
 * the lock-wait deadline when one is set. Transactions are always
 * serializable. Foreign keys are switched on for every connection, so
 * that the same schema behaves the same on every engine.
 */

#define TE_LGR_USER     "TA SQL SQLITE"

#include "te_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"

/** A connection. */
typedef struct lite_conn {
    sqlite3        *db;
    int             stmt_timeout_ms;    /**< 0 = none. */
    struct timespec deadline;           /**< Of the running statement. */
    volatile int    timed_out;          /**< The deadline interrupted. */
} lite_conn;

/** A statement. */
typedef struct lite_stmt {
    lite_conn    *conn;
    sqlite3_stmt *st;
    bool          row_pending;  /**< execute() already stepped onto a row. */
    bool          done;         /**< execute() already saw the end. */
} lite_stmt;

/* Classify the failure the connection has just had. */
static void
lite_err(lite_conn *c, sqlite3 *db, int res, ta_sql_errinfo *err)
{
    const char *msg = db != NULL ? sqlite3_errmsg(db) : sqlite3_errstr(res);
    int         primary = res & 0xff;
    int         cls = TA_SQL_ERR_OTHER;

    switch (primary)
    {
        case SQLITE_CONSTRAINT:
            cls = TA_SQL_ERR_CONSTRAINT;
            break;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            cls = TA_SQL_ERR_LOCK_TIMEOUT;
            break;
        case SQLITE_INTERRUPT:
            cls = (c != NULL && c->timed_out) ? TA_SQL_ERR_TIMEOUT :
                                                TA_SQL_ERR_CANCELLED;
            break;
        case SQLITE_READONLY:
        case SQLITE_PERM:
        case SQLITE_AUTH:
            cls = TA_SQL_ERR_PERMISSION;
            break;
        case SQLITE_CANTOPEN:
        case SQLITE_NOTADB:
            cls = TA_SQL_ERR_CONNECT;
            break;
        case SQLITE_IOERR:
        case SQLITE_CORRUPT:
            cls = TA_SQL_ERR_CONN_LOST;
            break;
        case SQLITE_ERROR:
            if (ta_sql_has(msg, "syntax error") ||
                ta_sql_has(msg, "incomplete input"))
                cls = TA_SQL_ERR_SYNTAX;
            else if (ta_sql_has(msg, "no such"))
                cls = TA_SQL_ERR_NOT_FOUND;
            break;
        default:
            break;
    }
    ta_sql_err_set(err, cls, res, NULL, "%s", msg);
}

/* Progress callback: interrupt a statement past its deadline. */
static int
lite_progress(void *arg)
{
    lite_conn      *c = arg;
    struct timespec now;

    if (c->stmt_timeout_ms <= 0)
        return 0;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > c->deadline.tv_sec ||
        (now.tv_sec == c->deadline.tv_sec &&
         now.tv_nsec >= c->deadline.tv_nsec))
    {
        c->timed_out = 1;
        return 1;
    }
    return 0;
}

/* Start the clock of a statement. */
static void
lite_arm(lite_conn *c)
{
    c->timed_out = 0;
    if (c->stmt_timeout_ms <= 0)
        return;

    clock_gettime(CLOCK_MONOTONIC, &c->deadline);
    c->deadline.tv_sec += c->stmt_timeout_ms / 1000;
    c->deadline.tv_nsec += (long)(c->stmt_timeout_ms % 1000) * 1000000L;
    if (c->deadline.tv_nsec >= 1000000000L)
    {
        c->deadline.tv_sec++;
        c->deadline.tv_nsec -= 1000000000L;
    }
}

static void
lite_version(te_string *out)
{
    te_string_append(out, "sqlite %s", sqlite3_libversion());
}

static int
lite_connect(const ta_sql_config *cfg, void **dc, ta_sql_errinfo *err)
{
    const char *path = cfg->dbname;
    lite_conn  *c;
    int         res;

    if (path == NULL || *path == '\0')
    {
        ta_sql_err_set(err, TA_SQL_ERR_CONNECT, 0, NULL,
                       "SQLite needs a database file path");
        return -1;
    }

    c = TE_ALLOC(sizeof(*c));
    res = sqlite3_open_v2(path, &c->db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_URI, NULL);
    if (res != SQLITE_OK)
    {
        lite_err(NULL, c->db, res, err);
        err->cls = TA_SQL_ERR_CONNECT;
        sqlite3_close(c->db);
        free(c);
        return -1;
    }

    sqlite3_extended_result_codes(c->db, 1);
    sqlite3_progress_handler(c->db, 1000, lite_progress, c);
    /* A file that is not a database is found out on the first read. */
    res = sqlite3_exec(c->db, "PRAGMA foreign_keys = ON; "
                              "SELECT count(*) FROM sqlite_master",
                       NULL, NULL, NULL);
    if (res != SQLITE_OK)
    {
        lite_err(NULL, c->db, res, err);
        err->cls = TA_SQL_ERR_CONNECT;
        sqlite3_close(c->db);
        free(c);
        return -1;
    }

    *dc = c;
    return 0;
}

static void
lite_disconnect(void *dc)
{
    lite_conn *c = dc;

    sqlite3_close_v2(c->db);
    free(c);
}

static int
lite_exec(void *dc, const char *sql, int64_t *affected, ta_sql_errinfo *err)
{
    lite_conn *c = dc;
    int        before = sqlite3_total_changes(c->db);
    int        res;

    lite_arm(c);
    res = sqlite3_exec(c->db, sql, NULL, NULL, NULL);
    if (res != SQLITE_OK)
    {
        lite_err(c, c->db, res, err);
        return -1;
    }
    if (affected != NULL)
        *affected = sqlite3_total_changes(c->db) - before;
    return 0;
}

static int
lite_ping(void *dc, ta_sql_errinfo *err)
{
    return lite_exec(dc, "SELECT 1", NULL, err);
}

static void
lite_server_version(void *dc, te_string *out)
{
    UNUSED(dc);
    te_string_append(out, "SQLite %s", sqlite3_libversion());
}

static int
lite_prepare(void *dc, const char *sql, void **ds, int *nparams,
             ta_sql_errinfo *err)
{
    lite_conn    *c = dc;
    sqlite3_stmt *st = NULL;
    const char   *tail = NULL;
    lite_stmt    *s;
    int           res;

    res = sqlite3_prepare_v2(c->db, sql, -1, &st, &tail);
    if (res != SQLITE_OK)
    {
        lite_err(c, c->db, res, err);
        return -1;
    }
    if (st == NULL)
    {
        ta_sql_err_set(err, TA_SQL_ERR_SYNTAX, 0, NULL, "empty statement");
        return -1;
    }

    s = TE_ALLOC(sizeof(*s));
    s->conn = c;
    s->st = st;
    *ds = s;
    *nparams = sqlite3_bind_parameter_count(st);
    return 0;
}

static int
lite_bind(void *ds, int index, char tag, const char *data, size_t len,
          ta_sql_errinfo *err)
{
    lite_stmt *s = ds;
    int        pos = index + 1;
    int        res;

    /* A binding cannot change under an open result. */
    sqlite3_reset(s->st);
    s->row_pending = false;
    s->done = false;

    switch (tag)
    {
        case 'N':
            res = sqlite3_bind_null(s->st, pos);
            break;
        case 'B':
            res = sqlite3_bind_int(s->st, pos, data[0] == '1');
            break;
        case 'I':
            res = sqlite3_bind_int64(s->st, pos, strtoll(data, NULL, 10));
            break;
        case 'U':
        {
            unsigned long long value;

            errno = 0;
            value = strtoull(data, NULL, 10);
            if (errno != 0 || value > (unsigned long long)LLONG_MAX)
            {
                ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                               "parameter %d: %s does not fit SQLite's "
                               "signed 64-bit integer", pos, data);
                return -1;
            }
            res = sqlite3_bind_int64(s->st, pos, (sqlite3_int64)value);
            break;
        }
        case 'F':
            res = sqlite3_bind_double(s->st, pos, strtod(data, NULL));
            break;
        case 'X':
            res = sqlite3_bind_blob64(s->st, pos, len > 0 ? data : "", len,
                                      SQLITE_TRANSIENT);
            break;
        default:
            res = sqlite3_bind_text64(s->st, pos, data, len,
                                      SQLITE_TRANSIENT, SQLITE_UTF8);
            break;
    }
    if (res != SQLITE_OK)
    {
        lite_err(s->conn, s->conn->db, res, err);
        return -1;
    }
    return 0;
}

static int
lite_execute(void *ds, int *ncols, int64_t *affected, ta_sql_errinfo *err)
{
    lite_stmt *s = ds;
    int        before = sqlite3_total_changes(s->conn->db);
    int        res;

    sqlite3_reset(s->st);
    s->row_pending = false;
    s->done = false;

    /* SQLite runs a statement as it is stepped; the first step is what
     * makes a constraint violation or a busy database show up here. */
    lite_arm(s->conn);
    res = sqlite3_step(s->st);
    if (res == SQLITE_ROW)
    {
        s->row_pending = true;
    }
    else if (res == SQLITE_DONE)
    {
        s->done = true;
    }
    else
    {
        lite_err(s->conn, s->conn->db, res, err);
        sqlite3_reset(s->st);
        return -1;
    }

    *ncols = sqlite3_column_count(s->st);
    if (affected != NULL)
        *affected = *ncols == 0 ?
                    sqlite3_total_changes(s->conn->db) - before : -1;
    return 0;
}

static const char *
lite_col_name(void *ds, int col)
{
    lite_stmt *s = ds;

    return sqlite3_column_name(s->st, col);
}

static char
lite_col_type(void *ds, int col)
{
    lite_stmt  *s = ds;
    const char *decl = sqlite3_column_decltype(s->st, col);

    return ta_sql_type_letter(decl);
}

static int
lite_step(void *ds, ta_sql_errinfo *err)
{
    lite_stmt *s = ds;
    int        res;

    if (s->row_pending)
    {
        s->row_pending = false;
        return 1;
    }
    if (s->done)
        return 0;

    res = sqlite3_step(s->st);
    if (res == SQLITE_ROW)
        return 1;
    if (res == SQLITE_DONE)
    {
        s->done = true;
        return 0;
    }
    lite_err(s->conn, s->conn->db, res, err);
    return -1;
}

static void
lite_cell(void *ds, int col, te_string *out)
{
    lite_stmt  *s = ds;
    const char *decl = sqlite3_column_decltype(s->st, col);
    char        letter = ta_sql_type_letter(decl);

    /* A value has a storage class of its own; the declared type only
     * says how to read an integer (boolean) or a text (decimal, date,
     * JSON). */
    switch (sqlite3_column_type(s->st, col))
    {
        case SQLITE_NULL:
            ta_sql_cell_null(out);
            break;
        case SQLITE_INTEGER:
            if (decl != NULL && letter == 'B')
                te_string_append(out, "B%d",
                                 sqlite3_column_int64(s->st, col) != 0);
            else
                te_string_append(out, "I%lld",
                        (long long)sqlite3_column_int64(s->st, col));
            break;
        case SQLITE_FLOAT:
            ta_sql_cell_double(out, sqlite3_column_double(s->st, col));
            break;
        case SQLITE_BLOB:
            ta_sql_cell_bin(out, sqlite3_column_blob(s->st, col),
                            (size_t)sqlite3_column_bytes(s->st, col));
            break;
        default:
        {
            const char *text = (const char *)sqlite3_column_text(s->st, col);
            size_t      len = (size_t)sqlite3_column_bytes(s->st, col);
            char        tag = 'T';

            if (decl != NULL &&
                (letter == 'D' || letter == 'M' || letter == 'J'))
                tag = letter;
            ta_sql_cell_text(out, tag, text != NULL ? text : "", len);
            break;
        }
    }
}

static void
lite_reset(void *ds)
{
    lite_stmt *s = ds;

    sqlite3_reset(s->st);
    s->row_pending = false;
    s->done = false;
}

static void
lite_finish(void *ds)
{
    lite_stmt *s = ds;

    sqlite3_finalize(s->st);
    free(s);
}

static int
lite_cancel(void *dc)
{
    lite_conn *c = dc;

    sqlite3_interrupt(c->db);
    return 0;
}

static int
lite_set_timeout(void *dc, int kind, int ms, ta_sql_errinfo *err)
{
    lite_conn *c = dc;

    UNUSED(err);
    if (kind == TA_SQL_TIMEOUT_LOCK)
        sqlite3_busy_timeout(c->db, ms);
    else
        c->stmt_timeout_ms = ms;
    return 0;
}

const ta_sql_driver ta_sql_sqlite_driver = {
    .engine = TA_SQL_ENGINE_SQLITE,
    .name = "sqlite",
    .caps = TA_SQL_CAP_TX | TA_SQL_CAP_SAVEPOINT | TA_SQL_CAP_MULTI_CURSOR |
            TA_SQL_CAP_STMT_TIMEOUT | TA_SQL_CAP_LOCK_TIMEOUT |
            TA_SQL_CAP_CANCEL,
    .isolation = TA_SQL_ISO_SERIALIZABLE,
    .default_iso = TA_SQL_ISO_SERIALIZABLE,
    .version = lite_version,
    .connect = lite_connect,
    .disconnect = lite_disconnect,
    .ping = lite_ping,
    .server_version = lite_server_version,
    .tls_in_use = NULL,
    .exec = lite_exec,
    .prepare = lite_prepare,
    .bind = lite_bind,
    .execute = lite_execute,
    .col_name = lite_col_name,
    .col_type = lite_col_type,
    .step = lite_step,
    .cell = lite_cell,
    .reset = lite_reset,
    .finish = lite_finish,
    .cancel = lite_cancel,
    .set_timeout = lite_set_timeout,
    .thread_init = NULL,
    .thread_fini = NULL,
};
