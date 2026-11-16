/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief ta_sql driver: PostgreSQL, over libpq
 *
 * Results are read in libpq's single-row mode, so a large result is
 * never held whole: a row leaves the server when the fetch asks for
 * it. The price is libpq's rule that a connection carries one command
 * at a time; when another command is sent while a result is still
 * open, this driver drops the rest of that result first.
 *
 * Parameters are written "?" in the statement and renumbered to
 * "$1".. here ("??" is a literal question mark, for the jsonb
 * operators). Values go as text with the type left to the server,
 * except binary, which goes in the binary format.
 */

#define TE_LGR_USER     "TA SQL PG"

#include "te_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"

struct pg_stmt;

/** A connection. */
typedef struct pg_conn {
    PGconn         *conn;
    struct pg_stmt *active;     /**< The statement streaming a result. */
    unsigned int    next_id;    /**< For prepared statement names. */
} pg_conn;

/** A statement. */
typedef struct pg_stmt {
    pg_conn  *c;
    char      name[32];
    int       nparams;
    char    **values;       /**< Parameter values, NULL for SQL NULL. */
    int      *lengths;
    int      *formats;
    int       ncols;
    char    **col_names;
    Oid      *col_types;
    PGresult *cur;          /**< The current row. */
    bool      row_pending;  /**< execute() already holds the first row. */
    bool      streaming;    /**< More results are to be read. */
} pg_stmt;

/* ---- errors ------------------------------------------------------- */

static bool
starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* The class of an SQLSTATE. */
static int
pg_class(const char *state, const char *msg)
{
    if (state == NULL || *state == '\0')
        return TA_SQL_ERR_OTHER;
    if (strcmp(state, "42601") == 0)
        return TA_SQL_ERR_SYNTAX;
    if (strcmp(state, "42P01") == 0 || strcmp(state, "42703") == 0 ||
        strcmp(state, "42883") == 0 || strcmp(state, "3F000") == 0)
        return TA_SQL_ERR_NOT_FOUND;
    if (starts(state, "23"))
        return TA_SQL_ERR_CONSTRAINT;
    if (strcmp(state, "40P01") == 0)
        return TA_SQL_ERR_DEADLOCK;
    if (strcmp(state, "40001") == 0)
        return TA_SQL_ERR_SERIALIZATION;
    if (strcmp(state, "55P03") == 0)
        return TA_SQL_ERR_LOCK_TIMEOUT;
    if (strcmp(state, "57014") == 0)
    {
        if (ta_sql_has(msg, "statement timeout"))
            return TA_SQL_ERR_TIMEOUT;
        if (ta_sql_has(msg, "lock timeout"))
            return TA_SQL_ERR_LOCK_TIMEOUT;
        return TA_SQL_ERR_CANCELLED;
    }
    if (strcmp(state, "42501") == 0)
        return TA_SQL_ERR_PERMISSION;
    if (starts(state, "28"))
        return TA_SQL_ERR_AUTH;
    if (starts(state, "08") || starts(state, "57P"))
        return TA_SQL_ERR_CONN_LOST;
    return TA_SQL_ERR_OTHER;
}

/* Describe a failed result (or, with none, the connection's state). */
static void
pg_err(pg_conn *c, const PGresult *res, ta_sql_errinfo *err)
{
    const char *state = NULL;
    const char *msg = NULL;
    int         cls;

    if (res != NULL)
    {
        state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        msg = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
    }
    if (msg == NULL || *msg == '\0')
        msg = PQerrorMessage(c->conn);

    cls = pg_class(state, msg);
    if (PQstatus(c->conn) == CONNECTION_BAD)
        cls = TA_SQL_ERR_CONN_LOST;
    ta_sql_err_set(err, cls, 0, state, "%s", msg);
}

/* The class of a connection that was never established. */
static int
pg_connect_class(const char *msg)
{
    if (ta_sql_has(msg, "password authentication failed") ||
        ta_sql_has(msg, "no password supplied") ||
        ta_sql_has(msg, "pg_hba.conf") ||
        (ta_sql_has(msg, "role") && ta_sql_has(msg, "does not exist")))
        return TA_SQL_ERR_AUTH;
    if (ta_sql_has(msg, "SSL") || ta_sql_has(msg, "certificate") ||
        ta_sql_has(msg, "TLS"))
        return TA_SQL_ERR_TLS;
    return TA_SQL_ERR_CONNECT;
}

/* ---- one command at a time ---------------------------------------- */

/* Read and drop whatever the connection still has to deliver. */
static void
pg_flush(pg_conn *c)
{
    PGresult *res;

    while ((res = PQgetResult(c->conn)) != NULL)
        PQclear(res);
}

/* Close the result a statement is streaming, if any. */
static void
pg_drain(pg_conn *c)
{
    pg_stmt *s = c->active;

    if (s == NULL)
        return;

    PQclear(s->cur);
    s->cur = NULL;
    pg_flush(c);
    s->streaming = false;
    s->row_pending = false;
    c->active = NULL;
}

/* Run a command and check it succeeded. */
static int
pg_cmd(pg_conn *c, const char *sql, int64_t *affected, ta_sql_errinfo *err)
{
    PGresult      *res;
    ExecStatusType st;

    pg_drain(c);
    res = PQexec(c->conn, sql);
    st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK &&
        st != PGRES_EMPTY_QUERY)
    {
        pg_err(c, res, err);
        PQclear(res);
        return -1;
    }
    if (affected != NULL)
    {
        const char *n = PQcmdTuples(res);

        *affected = (st == PGRES_COMMAND_OK && n != NULL && *n != '\0') ?
                    atoll(n) : -1;
    }
    PQclear(res);
    return 0;
}

/* ---- driver operations -------------------------------------------- */

static void
pg_version(te_string *out)
{
    int v = PQlibVersion();

    te_string_append(out, "libpq %d.%d", v / 10000, v % 10000);
}

static const char *
pg_sslmode(int tls_mode)
{
    switch (tls_mode)
    {
        case TA_SQL_TLS_PREFER: return "prefer";
        case TA_SQL_TLS_REQUIRE: return "require";
        case TA_SQL_TLS_VERIFY_CA: return "verify-ca";
        case TA_SQL_TLS_VERIFY_FULL: return "verify-full";
        default: return "disable";
    }
}

static int
pg_connect(const ta_sql_config *cfg, void **dc, ta_sql_errinfo *err)
{
    const char *keys[16];
    const char *vals[16];
    char        port[16];
    char        timeout[16];
    int         n = 0;
    pg_conn    *c;

#define PG_PARAM(_key, _val) \
    do {                                                \
        const char *val_ = (_val);                      \
                                                        \
        if (val_ != NULL && *val_ != '\0')              \
        {                                               \
            keys[n] = (_key);                           \
            vals[n] = val_;                             \
            n++;                                        \
        }                                               \
    } while (0)

    PG_PARAM("host", cfg->host);
    if (cfg->port > 0)
    {
        snprintf(port, sizeof(port), "%d", cfg->port);
        PG_PARAM("port", port);
    }
    PG_PARAM("dbname", cfg->dbname);
    PG_PARAM("user", cfg->user);
    PG_PARAM("password", cfg->password);
    PG_PARAM("sslmode", pg_sslmode(cfg->tls_mode));
    PG_PARAM("sslrootcert", cfg->tls_ca);
    PG_PARAM("sslcert", cfg->tls_cert);
    PG_PARAM("sslkey", cfg->tls_key);
    if (cfg->timeout_ms > 0)
    {
        /* libpq counts whole seconds and takes no less than two. */
        snprintf(timeout, sizeof(timeout), "%d",
                 (cfg->timeout_ms + 999) / 1000);
        PG_PARAM("connect_timeout", timeout);
    }
    PG_PARAM("application_name", "tsf-sql");
    keys[n] = NULL;
    vals[n] = NULL;
#undef PG_PARAM

    c = TE_ALLOC(sizeof(*c));
    /* No dbname expansion: a database name is never a connection
     * string here, so nothing in it can smuggle parameters in. */
    c->conn = PQconnectdbParams(keys, vals, 0);
    if (c->conn == NULL || PQstatus(c->conn) != CONNECTION_OK)
    {
        const char *msg = c->conn != NULL ? PQerrorMessage(c->conn) :
                                            "out of memory";

        ta_sql_err_set(err, pg_connect_class(msg), 0, NULL, "%s", msg);
        if (c->conn != NULL)
            PQfinish(c->conn);
        free(c);
        return -1;
    }

    /* Binary values are read back as hex whatever the server default;
     * notices ("table does not exist, skipping") stay off stderr. */
    if (pg_cmd(c, "SET bytea_output = hex; "
                  "SET client_min_messages = warning", NULL, err) != 0)
    {
        PQfinish(c->conn);
        free(c);
        return -1;
    }

    *dc = c;
    return 0;
}

static void
pg_disconnect(void *dc)
{
    pg_conn *c = dc;

    PQfinish(c->conn);
    free(c);
}

static int
pg_ping(void *dc, ta_sql_errinfo *err)
{
    return pg_cmd(dc, "SELECT 1", NULL, err);
}

static void
pg_server_version(void *dc, te_string *out)
{
    pg_conn    *c = dc;
    const char *v = PQparameterStatus(c->conn, "server_version");

    te_string_append(out, "PostgreSQL %s", v != NULL ? v : "?");
}

static int
pg_tls_in_use(void *dc)
{
    pg_conn *c = dc;

    return PQsslInUse(c->conn) ? 1 : 0;
}

static int
pg_exec(void *dc, const char *sql, int64_t *affected, ta_sql_errinfo *err)
{
    return pg_cmd(dc, sql, affected, err);
}

/*
 * Rewrite "?" parameters as "$1".. outside string literals, quoted
 * identifiers and comments. "??" is a literal "?".
 */
static char *
pg_placeholders(const char *sql, int *nparams)
{
    te_string   out = TE_STRING_INIT;
    const char *p = sql;
    int         n = 0;

    while (*p != '\0')
    {
        if (*p == '\'' || *p == '"')
        {
            char        quote = *p;
            const char *start = p++;

            while (*p != '\0')
            {
                if (*p == quote)
                {
                    p++;
                    if (*p != quote)
                        break;
                }
                p++;
            }
            te_string_append_buf(&out, start, (size_t)(p - start));
        }
        else if (p[0] == '-' && p[1] == '-')
        {
            const char *start = p;

            while (*p != '\0' && *p != '\n')
                p++;
            te_string_append_buf(&out, start, (size_t)(p - start));
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            const char *start = p;
            const char *end = strstr(p + 2, "*/");

            p = end != NULL ? end + 2 : p + strlen(p);
            te_string_append_buf(&out, start, (size_t)(p - start));
        }
        else if (p[0] == '?' && p[1] == '?')
        {
            te_string_append(&out, "?");
            p += 2;
        }
        else if (*p == '?')
        {
            te_string_append(&out, "$%d", ++n);
            p++;
        }
        else
        {
            te_string_append_buf(&out, p, 1);
            p++;
        }
    }

    *nparams = n;
    return out.ptr != NULL ? out.ptr : strdup("");
}

static int
pg_prepare(void *dc, const char *sql, void **ds, int *nparams,
           ta_sql_errinfo *err)
{
    pg_conn  *c = dc;
    PGresult *res;
    pg_stmt  *s;
    char     *text;
    int       n = 0;

    pg_drain(c);

    s = TE_ALLOC(sizeof(*s));
    s->c = c;
    snprintf(s->name, sizeof(s->name), "tsf_sql_%u", c->next_id++);

    text = pg_placeholders(sql, &n);
    res = PQprepare(c->conn, s->name, text, 0, NULL);
    free(text);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        pg_err(c, res, err);
        PQclear(res);
        free(s);
        return -1;
    }
    PQclear(res);

    s->nparams = n;
    s->values = TE_ALLOC((size_t)(n + 1) * sizeof(*s->values));
    s->lengths = TE_ALLOC((size_t)(n + 1) * sizeof(*s->lengths));
    s->formats = TE_ALLOC((size_t)(n + 1) * sizeof(*s->formats));

    *ds = s;
    *nparams = n;
    return 0;
}

static int
pg_bind(void *ds, int index, char tag, const char *data, size_t len,
        ta_sql_errinfo *err)
{
    pg_stmt *s = ds;

    if (index < 0 || index >= s->nparams)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "parameter %d: the statement takes %d", index + 1,
                       s->nparams);
        return -1;
    }

    free(s->values[index]);
    s->values[index] = NULL;
    s->lengths[index] = 0;
    s->formats[index] = 0;

    switch (tag)
    {
        case 'N':
            break;
        case 'B':
            s->values[index] = strdup(data[0] == '1' ? "t" : "f");
            break;
        case 'X':
            s->values[index] = TE_ALLOC(len + 1);
            memcpy(s->values[index], data, len);
            s->lengths[index] = (int)len;
            s->formats[index] = 1;
            break;
        default:
            s->values[index] = strdup(data);
            break;
    }
    return 0;
}

static void
pg_cols_free(pg_stmt *s)
{
    int i;

    for (i = 0; i < s->ncols; i++)
        free(s->col_names[i]);
    free(s->col_names);
    free(s->col_types);
    s->col_names = NULL;
    s->col_types = NULL;
    s->ncols = 0;
}

static void
pg_cols_take(pg_stmt *s, const PGresult *res)
{
    int i;

    pg_cols_free(s);
    s->ncols = PQnfields(res);
    s->col_names = TE_ALLOC((size_t)(s->ncols + 1) * sizeof(*s->col_names));
    s->col_types = TE_ALLOC((size_t)(s->ncols + 1) * sizeof(*s->col_types));
    for (i = 0; i < s->ncols; i++)
    {
        s->col_names[i] = strdup(PQfname(res, i));
        s->col_types[i] = PQftype(res, i);
    }
}

static int
pg_execute(void *ds, int *ncols, int64_t *affected, ta_sql_errinfo *err)
{
    pg_stmt  *s = ds;
    pg_conn  *c = s->c;
    PGresult *res;

    pg_drain(c);
    PQclear(s->cur);
    s->cur = NULL;
    s->row_pending = false;
    s->streaming = false;

    if (!PQsendQueryPrepared(c->conn, s->name, s->nparams,
                             (const char *const *)s->values, s->lengths,
                             s->formats, 0))
    {
        pg_err(c, NULL, err);
        return -1;
    }
    /* From here a row is delivered when it is asked for. */
    PQsetSingleRowMode(c->conn);

    res = PQgetResult(c->conn);
    switch (PQresultStatus(res))
    {
        case PGRES_SINGLE_TUPLE:
            pg_cols_take(s, res);
            s->cur = res;
            s->row_pending = true;
            s->streaming = true;
            c->active = s;
            if (affected != NULL)
                *affected = -1;
            break;

        case PGRES_TUPLES_OK:
            /* A result with no rows at all. */
            pg_cols_take(s, res);
            PQclear(res);
            pg_flush(c);
            if (affected != NULL)
                *affected = -1;
            break;

        case PGRES_COMMAND_OK:
        {
            const char *n = PQcmdTuples(res);

            pg_cols_free(s);
            if (affected != NULL)
                *affected = (n != NULL && *n != '\0') ? atoll(n) : -1;
            PQclear(res);
            pg_flush(c);
            break;
        }

        default:
            pg_err(c, res, err);
            PQclear(res);
            pg_flush(c);
            return -1;
    }

    *ncols = s->ncols;
    return 0;
}

static const char *
pg_col_name(void *ds, int col)
{
    pg_stmt *s = ds;

    return col < s->ncols ? s->col_names[col] : "";
}

/* The type letter of a PostgreSQL type. */
static char
pg_letter(Oid type)
{
    switch (type)
    {
        case 16: return 'B';                    /* bool */
        case 20: case 21: case 23: return 'I';  /* int8, int2, int4 */
        case 26: case 28: return 'U';           /* oid, xid */
        case 700: case 701: return 'F';         /* float4, float8 */
        case 1700: case 790: return 'D';        /* numeric, money */
        case 17: return 'X';                    /* bytea */
        case 1082: case 1083: case 1114:        /* date, time, timestamp */
        case 1184: case 1266: return 'M';       /* timestamptz, timetz */
        case 114: case 3802: return 'J';        /* json, jsonb */
        default: return 'T';
    }
}

static char
pg_col_type(void *ds, int col)
{
    pg_stmt *s = ds;

    return col < s->ncols ? pg_letter(s->col_types[col]) : 'T';
}

static int
pg_step(void *ds, ta_sql_errinfo *err)
{
    pg_stmt  *s = ds;
    pg_conn  *c = s->c;
    PGresult *res;

    if (s->row_pending)
    {
        s->row_pending = false;
        return 1;
    }
    if (!s->streaming)
        return 0;

    PQclear(s->cur);
    s->cur = NULL;

    res = PQgetResult(c->conn);
    if (res != NULL && PQresultStatus(res) == PGRES_SINGLE_TUPLE)
    {
        s->cur = res;
        return 1;
    }

    s->streaming = false;
    if (c->active == s)
        c->active = NULL;

    if (res == NULL)
        return 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK)
    {
        PQclear(res);
        pg_flush(c);
        return 0;
    }

    pg_err(c, res, err);
    PQclear(res);
    pg_flush(c);
    return -1;
}

static void
pg_cell(void *ds, int col, te_string *out)
{
    pg_stmt    *s = ds;
    const char *value;
    size_t      len;
    char        letter;

    if (s->cur == NULL || PQgetisnull(s->cur, 0, col))
    {
        ta_sql_cell_null(out);
        return;
    }

    value = PQgetvalue(s->cur, 0, col);
    len = (size_t)PQgetlength(s->cur, 0, col);
    letter = pg_letter(s->col_types[col]);

    switch (letter)
    {
        case 'B':
            te_string_append(out, "B%d", value[0] == 't');
            break;
        case 'F':
            /* "NaN" and "Infinity" as PostgreSQL spells them parse. */
            ta_sql_cell_double(out, strtod(value, NULL));
            break;
        case 'X':
            if (len >= 2 && value[0] == '\\' && value[1] == 'x')
            {
                te_string_append(out, "X");
                te_string_append_buf(out, value + 2, len - 2);
            }
            else
            {
                ta_sql_cell_bin(out, value, len);
            }
            break;
        default:
            ta_sql_cell_text(out, letter, value, len);
            break;
    }
}

static void
pg_reset(void *ds)
{
    pg_stmt *s = ds;

    if (s->c->active == s)
        pg_drain(s->c);
    PQclear(s->cur);
    s->cur = NULL;
    s->row_pending = false;
    s->streaming = false;
}

static void
pg_finish(void *ds)
{
    pg_stmt *s = ds;
    pg_conn *c = s->c;
    char     sql[64];
    int      i;

    pg_reset(s);

    /* Best effort: in a failed transaction the server refuses, and the
     * statement goes when the connection does. */
    if (PQstatus(c->conn) == CONNECTION_OK &&
        PQtransactionStatus(c->conn) != PQTRANS_INERROR)
    {
        pg_drain(c);
        snprintf(sql, sizeof(sql), "DEALLOCATE %s", s->name);
        PQclear(PQexec(c->conn, sql));
    }

    for (i = 0; i < s->nparams; i++)
        free(s->values[i]);
    free(s->values);
    free(s->lengths);
    free(s->formats);
    pg_cols_free(s);
    free(s);
}

static int
pg_cancel(void *dc)
{
    pg_conn  *c = dc;
    PGcancel *cancel = PQgetCancel(c->conn);
    char      errbuf[256];
    int       ok;

    if (cancel == NULL)
        return -1;
    ok = PQcancel(cancel, errbuf, sizeof(errbuf));
    PQfreeCancel(cancel);
    return ok ? 0 : -1;
}

static int
pg_set_timeout(void *dc, int kind, int ms, ta_sql_errinfo *err)
{
    char sql[64];

    snprintf(sql, sizeof(sql), "SET %s = %d",
             kind == TA_SQL_TIMEOUT_LOCK ? "lock_timeout" :
                                           "statement_timeout", ms);
    return pg_cmd(dc, sql, NULL, err);
}

const ta_sql_driver ta_sql_pg_driver = {
    .engine = TA_SQL_ENGINE_POSTGRES,
    .name = "postgres",
    .caps = TA_SQL_CAP_TX | TA_SQL_CAP_SAVEPOINT | TA_SQL_CAP_TLS |
            TA_SQL_CAP_BOOL | TA_SQL_CAP_JSON | TA_SQL_CAP_DECIMAL |
            TA_SQL_CAP_STMT_TIMEOUT | TA_SQL_CAP_LOCK_TIMEOUT |
            TA_SQL_CAP_CANCEL | TA_SQL_CAP_ROW_LOCKS | TA_SQL_CAP_DEADLOCK |
            TA_SQL_CAP_NETWORK | TA_SQL_CAP_AUTH | TA_SQL_CAP_CHECK_META,
    .isolation = TA_SQL_ISO_READ_UNCOMMITTED | TA_SQL_ISO_READ_COMMITTED |
                 TA_SQL_ISO_REPEATABLE_READ | TA_SQL_ISO_SERIALIZABLE,
    .default_iso = TA_SQL_ISO_READ_COMMITTED,
    .version = pg_version,
    .connect = pg_connect,
    .disconnect = pg_disconnect,
    .ping = pg_ping,
    .server_version = pg_server_version,
    .tls_in_use = pg_tls_in_use,
    .exec = pg_exec,
    .prepare = pg_prepare,
    .bind = pg_bind,
    .execute = pg_execute,
    .col_name = pg_col_name,
    .col_type = pg_col_type,
    .step = pg_step,
    .cell = pg_cell,
    .reset = pg_reset,
    .finish = pg_finish,
    .cancel = pg_cancel,
    .set_timeout = pg_set_timeout,
    .thread_init = NULL,
    .thread_fini = NULL,
};
