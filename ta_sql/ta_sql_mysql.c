/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief ta_sql driver: MySQL / MariaDB, over MariaDB Connector/C
 *
 * Prepared statements run over the binary protocol and their results
 * are read unbuffered (no mysql_stmt_store_result), so a large result
 * is never held whole. As with libpq, a connection then carries one
 * result at a time; a command sent while a result is still open drops
 * the rest of that result first.
 *
 * Cancelling needs a second connection (KILL QUERY), so the driver
 * keeps the connection parameters, the secret among them, in memory
 * until the connection is closed.
 */

#define TE_LGR_USER     "TA SQL MYSQL"

#include "te_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"

/** Bytes a result cell gets before it is fetched again in full. */
#define MY_CELL_BUF     256

struct my_stmt;

/** A connection. */
typedef struct my_conn {
    MYSQL          *m;
    struct my_stmt *active;     /**< The statement streaming a result. */
    unsigned long   thread_id;  /**< The server's id of this session. */
    /* What a second connection needs, for KILL QUERY. */
    char           *host;
    char           *user;
    char           *password;
    char           *dbname;
    char           *tls_ca;
    char           *tls_cert;
    char           *tls_key;
    int             port;
    int             tls_mode;
    int             timeout_ms;
} my_conn;

/** A bound parameter. */
typedef struct my_param {
    char              *buf;
    unsigned long      len;
    long long          ll;
    unsigned long long ull;
    double             d;
} my_param;

/** A result column. */
typedef struct my_col {
    char         *name;
    char          letter;
    char         *buf;
    double        d;
    unsigned long len;
    my_bool       is_null;
    my_bool       error;
} my_col;

/** A statement. */
typedef struct my_stmt {
    my_conn    *c;
    MYSQL_STMT *st;
    int         nparams;
    MYSQL_BIND *pbind;
    my_param   *params;
    int         ncols;
    MYSQL_BIND *rbind;
    my_col     *cols;
    bool        streaming;
} my_stmt;

/* ---- errors ------------------------------------------------------- */

static int
my_class(unsigned int code)
{
    switch (code)
    {
        case 1064: case 1149:
            return TA_SQL_ERR_SYNTAX;
        case 1146: case 1054: case 1049: case 1051: case 1305:
            return TA_SQL_ERR_NOT_FOUND;
        case 1062: case 1048: case 1451: case 1452: case 1216: case 1217:
        case 1364: case 4025: case 3819: case 1169: case 1557:
            return TA_SQL_ERR_CONSTRAINT;
        case 1213:
            return TA_SQL_ERR_DEADLOCK;
        case 1205:
            return TA_SQL_ERR_LOCK_TIMEOUT;
        case 1969: case 3024:
            return TA_SQL_ERR_TIMEOUT;
        case 1317:
            return TA_SQL_ERR_CANCELLED;
        case 1045: case 1698: case 1130:
            return TA_SQL_ERR_AUTH;
        case 1044: case 1142: case 1143: case 1227: case 1370:
            return TA_SQL_ERR_PERMISSION;
        case 2002: case 2003: case 2005:
            return TA_SQL_ERR_CONNECT;
        case 2006: case 2013: case 1927: case 1053: case 1152: case 1080:
        case 1184: case 5014:
            return TA_SQL_ERR_CONN_LOST;
        case 2026:
            return TA_SQL_ERR_TLS;
        default:
            return TA_SQL_ERR_OTHER;
    }
}

static void
my_err(MYSQL *m, ta_sql_errinfo *err)
{
    unsigned int code = mysql_errno(m);

    ta_sql_err_set(err, my_class(code), (int)code, mysql_sqlstate(m), "%s",
                   mysql_error(m));
}

static void
my_stmt_err(MYSQL_STMT *st, ta_sql_errinfo *err)
{
    unsigned int code = mysql_stmt_errno(st);

    ta_sql_err_set(err, my_class(code), (int)code, mysql_stmt_sqlstate(st),
                   "%s", mysql_stmt_error(st));
}

/* ---- one result at a time ----------------------------------------- */

/* Close the result a statement is streaming, if any. */
static void
my_drain(my_conn *c)
{
    my_stmt *s = c->active;

    if (s == NULL)
        return;
    mysql_stmt_free_result(s->st);
    s->streaming = false;
    c->active = NULL;
}

/* ---- connecting --------------------------------------------------- */

static char *
dup_or_null(const char *s)
{
    return (s != NULL && *s != '\0') ? strdup(s) : NULL;
}

/* Open a session with the given TLS mode. */
static MYSQL *
my_open(const my_conn *c, int tls_mode, ta_sql_errinfo *err)
{
    MYSQL  *m = mysql_init(NULL);
    my_bool on = 1;
    my_bool off = 0;

    if (m == NULL)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL, "out of memory");
        return NULL;
    }

    if (c->timeout_ms > 0)
    {
        unsigned int sec = (unsigned int)(c->timeout_ms + 999) / 1000;

        mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &sec);
    }
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (tls_mode == TA_SQL_TLS_DISABLE)
    {
        mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &off);
        mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &off);
    }
    else
    {
        bool verify = tls_mode >= TA_SQL_TLS_VERIFY_CA;

        mysql_ssl_set(m, c->tls_key, c->tls_cert, c->tls_ca, NULL, NULL);
        mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &on);
        mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                      verify ? &on : &off);
    }

    if (mysql_real_connect(m, c->host, c->user, c->password, c->dbname,
                           (unsigned int)c->port, NULL, 0) == NULL)
    {
        unsigned int code = mysql_errno(m);
        int          cls = my_class(code);

        /* Before a session exists, "access denied to the database" is
         * the account being refused, and an unknown database is a
         * server that cannot be reached as asked. */
        if (code == 1044)
            cls = TA_SQL_ERR_AUTH;
        else if (cls == TA_SQL_ERR_NOT_FOUND || cls == TA_SQL_ERR_OTHER ||
                 cls == TA_SQL_ERR_CONN_LOST)
            cls = TA_SQL_ERR_CONNECT;
        ta_sql_err_set(err, cls, (int)code, mysql_sqlstate(m), "%s",
                       mysql_error(m));
        mysql_close(m);
        return NULL;
    }
    return m;
}

/* "Prefer": try encrypted, fall back to plain when TLS itself fails. */
static MYSQL *
my_open_any(const my_conn *c, ta_sql_errinfo *err)
{
    MYSQL *m;

    if (c->tls_mode != TA_SQL_TLS_PREFER)
        return my_open(c, c->tls_mode, err);

    m = my_open(c, TA_SQL_TLS_REQUIRE, err);
    if (m == NULL && err->cls == TA_SQL_ERR_TLS)
        m = my_open(c, TA_SQL_TLS_DISABLE, err);
    return m;
}

static void
my_conn_free(my_conn *c)
{
    if (c->password != NULL)
        memset(c->password, 0, strlen(c->password));
    free(c->host);
    free(c->user);
    free(c->password);
    free(c->dbname);
    free(c->tls_ca);
    free(c->tls_cert);
    free(c->tls_key);
    free(c);
}

static void
my_version(te_string *out)
{
    te_string_append(out, "mariadb %s", mysql_get_client_info());
}

static int
my_connect(const ta_sql_config *cfg, void **dc, ta_sql_errinfo *err)
{
    my_conn *c = TE_ALLOC(sizeof(*c));

    c->host = dup_or_null(cfg->host);
    c->user = dup_or_null(cfg->user);
    c->password = dup_or_null(cfg->password);
    c->dbname = dup_or_null(cfg->dbname);
    c->tls_ca = dup_or_null(cfg->tls_ca);
    c->tls_cert = dup_or_null(cfg->tls_cert);
    c->tls_key = dup_or_null(cfg->tls_key);
    c->port = cfg->port;
    c->tls_mode = cfg->tls_mode;
    c->timeout_ms = cfg->timeout_ms;

    c->m = my_open_any(c, err);
    if (c->m == NULL)
    {
        my_conn_free(c);
        return -1;
    }
    c->thread_id = mysql_thread_id(c->m);

    *dc = c;
    return 0;
}

static void
my_disconnect(void *dc)
{
    my_conn *c = dc;

    mysql_close(c->m);
    my_conn_free(c);
}

static int
my_ping(void *dc, ta_sql_errinfo *err)
{
    my_conn *c = dc;

    my_drain(c);
    if (mysql_ping(c->m) != 0)
    {
        my_err(c->m, err);
        return -1;
    }
    return 0;
}

static void
my_server_version(void *dc, te_string *out)
{
    my_conn *c = dc;

    te_string_append(out, "%s", mysql_get_server_info(c->m));
}

static int
my_tls_in_use(void *dc)
{
    my_conn *c = dc;

    return mysql_get_ssl_cipher(c->m) != NULL ? 1 : 0;
}

/* ---- plain statements --------------------------------------------- */

static int
my_exec(void *dc, const char *sql, int64_t *affected, ta_sql_errinfo *err)
{
    my_conn     *c = dc;
    my_ulonglong n = (my_ulonglong)-1;
    int          more;

    my_drain(c);
    if (mysql_real_query(c->m, sql, strlen(sql)) != 0)
    {
        my_err(c->m, err);
        return -1;
    }

    do {
        /* Rows are read and dropped one by one, never stored. */
        MYSQL_RES *res = mysql_use_result(c->m);

        if (res != NULL)
        {
            while (mysql_fetch_row(res) != NULL)
                ;
            mysql_free_result(res);
            if (mysql_errno(c->m) != 0)
            {
                my_err(c->m, err);
                return -1;
            }
        }
        else if (mysql_field_count(c->m) != 0)
        {
            my_err(c->m, err);
            return -1;
        }
        else
        {
            n = mysql_affected_rows(c->m);
        }
        more = mysql_next_result(c->m);
    } while (more == 0);

    if (more > 0)
    {
        my_err(c->m, err);
        return -1;
    }
    if (affected != NULL)
        *affected = n == (my_ulonglong)-1 ? -1 : (int64_t)n;
    return 0;
}

/* ---- prepared statements ------------------------------------------ */

static int
my_prepare(void *dc, const char *sql, void **ds, int *nparams,
           ta_sql_errinfo *err)
{
    my_conn *c = dc;
    my_stmt *s;
    int      i;

    my_drain(c);

    s = TE_ALLOC(sizeof(*s));
    s->c = c;
    s->st = mysql_stmt_init(c->m);
    if (s->st == NULL)
    {
        my_err(c->m, err);
        free(s);
        return -1;
    }
    if (mysql_stmt_prepare(s->st, sql, strlen(sql)) != 0)
    {
        my_stmt_err(s->st, err);
        mysql_stmt_close(s->st);
        free(s);
        return -1;
    }

    s->nparams = (int)mysql_stmt_param_count(s->st);
    s->pbind = TE_ALLOC((size_t)(s->nparams + 1) * sizeof(*s->pbind));
    s->params = TE_ALLOC((size_t)(s->nparams + 1) * sizeof(*s->params));
    for (i = 0; i < s->nparams; i++)
        s->pbind[i].buffer_type = MYSQL_TYPE_NULL;

    *ds = s;
    *nparams = s->nparams;
    return 0;
}

static int
my_bind(void *ds, int index, char tag, const char *data, size_t len,
        ta_sql_errinfo *err)
{
    my_stmt    *s = ds;
    MYSQL_BIND *b;
    my_param   *p;

    if (index < 0 || index >= s->nparams)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "parameter %d: the statement takes %d", index + 1,
                       s->nparams);
        return -1;
    }

    b = &s->pbind[index];
    p = &s->params[index];
    free(p->buf);
    memset(p, 0, sizeof(*p));
    memset(b, 0, sizeof(*b));

    switch (tag)
    {
        case 'N':
            b->buffer_type = MYSQL_TYPE_NULL;
            break;
        case 'B':
        case 'I':
            p->ll = tag == 'B' ? (data[0] == '1') : strtoll(data, NULL, 10);
            b->buffer_type = MYSQL_TYPE_LONGLONG;
            b->buffer = &p->ll;
            break;
        case 'U':
            p->ull = strtoull(data, NULL, 10);
            b->buffer_type = MYSQL_TYPE_LONGLONG;
            b->buffer = &p->ull;
            b->is_unsigned = 1;
            break;
        case 'F':
            p->d = strtod(data, NULL);
            b->buffer_type = MYSQL_TYPE_DOUBLE;
            b->buffer = &p->d;
            break;
        default:
            p->buf = TE_ALLOC(len + 1);
            memcpy(p->buf, data, len);
            p->len = (unsigned long)len;
            b->buffer_type = tag == 'X' ? MYSQL_TYPE_BLOB :
                                          MYSQL_TYPE_STRING;
            b->buffer = p->buf;
            b->buffer_length = p->len;
            b->length = &p->len;
            break;
    }
    return 0;
}

static void
my_cols_free(my_stmt *s)
{
    int i;

    for (i = 0; i < s->ncols; i++)
    {
        free(s->cols[i].name);
        free(s->cols[i].buf);
    }
    free(s->cols);
    free(s->rbind);
    s->cols = NULL;
    s->rbind = NULL;
    s->ncols = 0;
}

/* The type letter of a result field. */
static char
my_letter(const MYSQL_FIELD *f)
{
    switch (f->type)
    {
        case MYSQL_TYPE_TINY:
            if (f->length == 1)
                return 'B';
            return (f->flags & UNSIGNED_FLAG) != 0 ? 'U' : 'I';
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_YEAR:
            return (f->flags & UNSIGNED_FLAG) != 0 ? 'U' : 'I';
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            return 'F';
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return 'D';
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
            return 'M';
        case MYSQL_TYPE_JSON:
            return 'J';
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_VARCHAR:
            /* Character set 63 is "binary". */
            return f->charsetnr == 63 ? 'X' : 'T';
        default:
            return 'T';
    }
}

static int
my_execute(void *ds, int *ncols, int64_t *affected, ta_sql_errinfo *err)
{
    my_stmt     *s = ds;
    my_conn     *c = s->c;
    MYSQL_RES   *meta;
    MYSQL_FIELD *fields;
    my_ulonglong n;
    int          i;

    my_drain(c);
    mysql_stmt_free_result(s->st);
    s->streaming = false;
    my_cols_free(s);

    if (s->nparams > 0 && mysql_stmt_bind_param(s->st, s->pbind) != 0)
    {
        my_stmt_err(s->st, err);
        return -1;
    }
    if (mysql_stmt_execute(s->st) != 0)
    {
        my_stmt_err(s->st, err);
        return -1;
    }

    meta = mysql_stmt_result_metadata(s->st);
    if (meta == NULL)
    {
        n = mysql_stmt_affected_rows(s->st);
        *ncols = 0;
        if (affected != NULL)
            *affected = n == (my_ulonglong)-1 ? -1 : (int64_t)n;
        return 0;
    }

    s->ncols = (int)mysql_num_fields(meta);
    fields = mysql_fetch_fields(meta);
    s->cols = TE_ALLOC((size_t)(s->ncols + 1) * sizeof(*s->cols));
    s->rbind = TE_ALLOC((size_t)(s->ncols + 1) * sizeof(*s->rbind));
    for (i = 0; i < s->ncols; i++)
    {
        my_col     *col = &s->cols[i];
        MYSQL_BIND *b = &s->rbind[i];

        col->name = strdup(fields[i].name != NULL ? fields[i].name : "");
        col->letter = my_letter(&fields[i]);
        b->is_null = &col->is_null;
        b->error = &col->error;
        b->length = &col->len;
        if (col->letter == 'F')
        {
            /* Read as a double: no detour through a decimal string. */
            b->buffer_type = MYSQL_TYPE_DOUBLE;
            b->buffer = &col->d;
        }
        else
        {
            col->buf = TE_ALLOC(MY_CELL_BUF);
            b->buffer_type = col->letter == 'X' ? MYSQL_TYPE_BLOB :
                                                  MYSQL_TYPE_STRING;
            b->buffer = col->buf;
            b->buffer_length = MY_CELL_BUF;
        }
    }
    mysql_free_result(meta);

    if (mysql_stmt_bind_result(s->st, s->rbind) != 0)
    {
        my_stmt_err(s->st, err);
        mysql_stmt_free_result(s->st);
        return -1;
    }

    s->streaming = true;
    c->active = s;
    *ncols = s->ncols;
    if (affected != NULL)
        *affected = -1;
    return 0;
}

static const char *
my_col_name(void *ds, int col)
{
    my_stmt *s = ds;

    return col < s->ncols ? s->cols[col].name : "";
}

static char
my_col_type(void *ds, int col)
{
    my_stmt *s = ds;

    return col < s->ncols ? s->cols[col].letter : 'T';
}

static int
my_step(void *ds, ta_sql_errinfo *err)
{
    my_stmt *s = ds;
    int      res;

    if (!s->streaming)
        return 0;

    res = mysql_stmt_fetch(s->st);
    if (res == 0 || res == MYSQL_DATA_TRUNCATED)
        return 1;

    s->streaming = false;
    if (s->c->active == s)
        s->c->active = NULL;

    if (res == MYSQL_NO_DATA)
    {
        mysql_stmt_free_result(s->st);
        return 0;
    }
    my_stmt_err(s->st, err);
    mysql_stmt_free_result(s->st);
    return -1;
}

static void
my_cell(void *ds, int col, te_string *out)
{
    my_stmt    *s = ds;
    my_col     *c = &s->cols[col];
    const char *data = c->buf;
    char       *full = NULL;

    if (c->is_null)
    {
        ta_sql_cell_null(out);
        return;
    }
    if (c->letter == 'F')
    {
        ta_sql_cell_double(out, c->d);
        return;
    }

    if (c->len > MY_CELL_BUF)
    {
        /* The cell did not fit: fetch this column again, in full. */
        MYSQL_BIND    b;
        unsigned long len = 0;

        memset(&b, 0, sizeof(b));
        full = TE_ALLOC(c->len + 1);
        b.buffer_type = s->rbind[col].buffer_type;
        b.buffer = full;
        b.buffer_length = c->len;
        b.length = &len;
        if (mysql_stmt_fetch_column(s->st, &b, (unsigned int)col, 0) != 0)
        {
            free(full);
            ta_sql_cell_null(out);
            return;
        }
        data = full;
    }

    switch (c->letter)
    {
        case 'B':
            te_string_append(out, "B%d", data[0] != '0');
            break;
        case 'X':
            ta_sql_cell_bin(out, data, c->len);
            break;
        default:
            ta_sql_cell_text(out, c->letter, data, c->len);
            break;
    }
    free(full);
}

static void
my_reset(void *ds)
{
    my_stmt *s = ds;

    if (s->streaming)
        mysql_stmt_free_result(s->st);
    s->streaming = false;
    if (s->c->active == s)
        s->c->active = NULL;
}

static void
my_finish(void *ds)
{
    my_stmt *s = ds;
    int      i;

    my_reset(s);
    my_drain(s->c);
    mysql_stmt_close(s->st);

    for (i = 0; i < s->nparams; i++)
        free(s->params[i].buf);
    free(s->params);
    free(s->pbind);
    my_cols_free(s);
    free(s);
}

/* ---- cancellation and deadlines ----------------------------------- */

static int
my_cancel(void *dc)
{
    my_conn       *c = dc;
    ta_sql_errinfo err;
    MYSQL         *killer;
    char           sql[64];
    int            res;

    memset(&err, 0, sizeof(err));
    killer = my_open_any(c, &err);
    if (killer == NULL)
        return -1;

    snprintf(sql, sizeof(sql), "KILL QUERY %lu", c->thread_id);
    res = mysql_query(killer, sql);
    mysql_close(killer);
    return res == 0 ? 0 : -1;
}

static int
my_set_timeout(void *dc, int kind, int ms, ta_sql_errinfo *err)
{
    my_conn *c = dc;
    char     sql[96];

    if (kind == TA_SQL_TIMEOUT_LOCK)
    {
        /* InnoDB counts whole seconds; 0 here restores its default. */
        snprintf(sql, sizeof(sql),
                 "SET SESSION innodb_lock_wait_timeout = %d",
                 ms == 0 ? 50 : (ms + 999) / 1000);
        return my_exec(c, sql, NULL, err);
    }

    /* MariaDB: seconds with a fraction, any statement. */
    snprintf(sql, sizeof(sql), "SET SESSION max_statement_time = %d.%03d",
             ms / 1000, ms % 1000);
    if (my_exec(c, sql, NULL, err) == 0)
        return 0;
    if (err->native != 1193)
        return -1;

    /* MySQL: milliseconds, SELECT only. */
    snprintf(sql, sizeof(sql), "SET SESSION max_execution_time = %d", ms);
    return my_exec(c, sql, NULL, err);
}

static void
my_thread_init(void)
{
    mysql_thread_init();
}

static void
my_thread_fini(void)
{
    mysql_thread_end();
}

const ta_sql_driver ta_sql_mysql_driver = {
    .engine = TA_SQL_ENGINE_MYSQL,
    .name = "mysql",
    .caps = TA_SQL_CAP_TX | TA_SQL_CAP_SAVEPOINT | TA_SQL_CAP_TLS |
            TA_SQL_CAP_UINT64 | TA_SQL_CAP_DECIMAL |
            TA_SQL_CAP_STMT_TIMEOUT | TA_SQL_CAP_LOCK_TIMEOUT |
            TA_SQL_CAP_CANCEL | TA_SQL_CAP_ROW_LOCKS | TA_SQL_CAP_DEADLOCK |
            TA_SQL_CAP_NETWORK | TA_SQL_CAP_AUTH | TA_SQL_CAP_CHECK_META,
    .isolation = TA_SQL_ISO_READ_UNCOMMITTED | TA_SQL_ISO_READ_COMMITTED |
                 TA_SQL_ISO_REPEATABLE_READ | TA_SQL_ISO_SERIALIZABLE,
    .default_iso = TA_SQL_ISO_REPEATABLE_READ,
    .version = my_version,
    .connect = my_connect,
    .disconnect = my_disconnect,
    .ping = my_ping,
    .server_version = my_server_version,
    .tls_in_use = my_tls_in_use,
    .exec = my_exec,
    .prepare = my_prepare,
    .bind = my_bind,
    .execute = my_execute,
    .col_name = my_col_name,
    .col_type = my_col_type,
    .step = my_step,
    .cell = my_cell,
    .reset = my_reset,
    .finish = my_finish,
    .cancel = my_cancel,
    .set_timeout = my_set_timeout,
    .thread_init = my_thread_init,
    .thread_fini = my_thread_fini,
};
