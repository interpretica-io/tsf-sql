/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL database sessions on an agent: the engine-neutral core
 *
 * The handle tables, the value format, the bounded fetch, transaction
 * control, the asynchronous statements and the redaction of secrets.
 * Everything that names a client library is in a driver unit behind
 * ta_sql_drv.h; introspection is in ta_sql_meta.c.
 */

#define TE_LGR_USER     "TA SQL"

#include "te_config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_string.h"

#include "ta_sql.h"
#include "ta_sql_drv.h"
#include "ta_sql_int.h"

/** Connections one RPC server can hold. */
#define TA_SQL_MAX_CONNS    64
/** Statements one RPC server can hold. */
#define TA_SQL_MAX_STMTS    256
/** Default bounds of a fetch batch. */
#define TA_SQL_DEF_ROWS     1000
#define TA_SQL_DEF_BYTES    (1024 * 1024)

/** States of the statement a connection runs on a thread. */
enum {
    ASYNC_IDLE = 0,
    ASYNC_RUNNING,
    ASYNC_DONE,
};

/** A connection. */
typedef struct sql_conn {
    bool                 used;
    const ta_sql_driver *drv;
    void                *dc;        /**< The driver's connection. */
    char                *secret;    /**< For redaction only. */
    ta_sql_errinfo       err;       /**< Last database failure. */

    /* The asynchronous statement. */
    pthread_mutex_t      alock;
    pthread_cond_t       acond;
    pthread_t            athread;
    int                  astate;
    char                *asql;
    int                  arc;
    int64_t              aaffected;
    uint64_t             aelapsed;
    ta_sql_errinfo       aerr;
} sql_conn;

/** A prepared statement. */
typedef struct sql_stmt {
    bool      used;
    int       conn;     /**< Index of its connection. */
    void     *ds;       /**< The driver's statement. */
    int       ncols;
    bool      open;     /**< A result is open and not exhausted. */
    te_string held;     /**< A row read but not yet handed out. */
} sql_stmt;

static sql_conn conns[TA_SQL_MAX_CONNS];
static sql_stmt stmts[TA_SQL_MAX_STMTS];
static uint64_t max_batch_bytes;

static const ta_sql_driver *const drivers[] = {
#ifdef TA_SQL_HAVE_SQLITE
    &ta_sql_sqlite_driver,
#endif
#ifdef TA_SQL_HAVE_POSTGRES
    &ta_sql_pg_driver,
#endif
#ifdef TA_SQL_HAVE_MYSQL
    &ta_sql_mysql_driver,
#endif
    NULL
};

/* ---- helpers shared with the drivers ------------------------------ */

/* See description in ta_sql_drv.h */
void
ta_sql_err_set(ta_sql_errinfo *err, int cls, int native,
               const char *sqlstate, const char *fmt, ...)
{
    va_list ap;
    size_t  len;

    if (err == NULL)
        return;

    memset(err, 0, sizeof(*err));
    err->cls = cls;
    err->native = native;
    if (sqlstate != NULL)
        snprintf(err->sqlstate, sizeof(err->sqlstate), "%s", sqlstate);

    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);

    /* Client libraries end their messages with a newline or two. */
    len = strlen(err->message);
    while (len > 0 && isspace((unsigned char)err->message[len - 1]))
        err->message[--len] = '\0';
}

/* See description in ta_sql_drv.h */
void
ta_sql_cell_null(te_string *out)
{
    te_string_append(out, "N");
}

/* See description in ta_sql_drv.h */
void
ta_sql_cell_text(te_string *out, char tag, const char *data, size_t len)
{
    char   buf[512];
    size_t used = 0;
    size_t i;

    buf[used++] = tag;
    for (i = 0; i < len; i++)
    {
        char esc = 0;

        switch (data[i])
        {
            case '\\': esc = '\\'; break;
            case '\t': esc = 't'; break;
            case '\n': esc = 'n'; break;
            case '\r': esc = 'r'; break;
            /* A NUL cannot travel in an RPC string; text has none. */
            case '\0': esc = '0'; break;
            default: break;
        }
        if (esc != 0)
        {
            buf[used++] = '\\';
            buf[used++] = esc;
        }
        else
        {
            buf[used++] = data[i];
        }
        if (used >= sizeof(buf) - 2)
        {
            te_string_append_buf(out, buf, used);
            used = 0;
        }
    }
    if (used > 0)
        te_string_append_buf(out, buf, used);
}

/* See description in ta_sql_drv.h */
void
ta_sql_cell_bin(te_string *out, const void *data, size_t len)
{
    static const char    hex[] = "0123456789abcdef";
    const unsigned char *bytes = data;
    char                 buf[512];
    size_t               used = 0;
    size_t               i;

    buf[used++] = 'X';
    for (i = 0; i < len; i++)
    {
        buf[used++] = hex[bytes[i] >> 4];
        buf[used++] = hex[bytes[i] & 0x0f];
        if (used >= sizeof(buf) - 2)
        {
            te_string_append_buf(out, buf, used);
            used = 0;
        }
    }
    te_string_append_buf(out, buf, used);
}

/* See description in ta_sql_drv.h */
void
ta_sql_cell_double(te_string *out, double value)
{
    if (isnan(value))
        te_string_append(out, "Fnan");
    else if (isinf(value))
        te_string_append(out, value > 0 ? "Finf" : "F-inf");
    else
        te_string_append(out, "F%.17g", value);
}

/* See description in ta_sql_drv.h */
bool
ta_sql_has(const char *haystack, const char *needle)
{
    return haystack != NULL && strcasestr(haystack, needle) != NULL;
}

/* See description in ta_sql_drv.h */
char
ta_sql_type_letter(const char *native)
{
    if (native == NULL || *native == '\0')
        return 'T';
    if (ta_sql_has(native, "bool") || ta_sql_has(native, "tinyint(1)"))
        return 'B';
    if (ta_sql_has(native, "json"))
        return 'J';
    if (ta_sql_has(native, "blob") || ta_sql_has(native, "binary") ||
        ta_sql_has(native, "bytea"))
        return 'X';
    /* Names with "int" inside that are not integers. */
    if (ta_sql_has(native, "interval") || ta_sql_has(native, "point"))
        return 'T';
    if (ta_sql_has(native, "int") || ta_sql_has(native, "serial"))
        return ta_sql_has(native, "unsigned") ? 'U' : 'I';
    if (ta_sql_has(native, "double") || ta_sql_has(native, "real") ||
        ta_sql_has(native, "float"))
        return 'F';
    if (ta_sql_has(native, "numeric") || ta_sql_has(native, "decimal") ||
        ta_sql_has(native, "money"))
        return 'D';
    if (ta_sql_has(native, "date") || ta_sql_has(native, "time"))
        return 'M';
    return 'T';
}

/* ---- internal helpers --------------------------------------------- */

/* See description in ta_sql_int.h */
te_errno
ta_sql_cell_decode(const char *cell, size_t len, char *tag, char **data,
                   size_t *dlen)
{
    char  *out;
    size_t n = 0;
    size_t i;

    if (len == 0)
        return TE_EINVAL;

    *tag = cell[0];
    out = TE_ALLOC(len + 1);

    if (*tag == 'X')
    {
        if ((len - 1) % 2 != 0)
        {
            free(out);
            return TE_EINVAL;
        }
        for (i = 1; i + 1 < len; i += 2)
        {
            unsigned int byte;
            const char   pair[3] = { cell[i], cell[i + 1], '\0' };

            if (!isxdigit((unsigned char)pair[0]) ||
                !isxdigit((unsigned char)pair[1]))
            {
                free(out);
                return TE_EINVAL;
            }
            sscanf(pair, "%2x", &byte);
            out[n++] = (char)byte;
        }
    }
    else
    {
        for (i = 1; i < len; i++)
        {
            if (cell[i] == '\\' && i + 1 < len)
            {
                i++;
                switch (cell[i])
                {
                    case 't': out[n++] = '\t'; break;
                    case 'n': out[n++] = '\n'; break;
                    case 'r': out[n++] = '\r'; break;
                    case '0': out[n++] = '\0'; break;
                    default: out[n++] = cell[i]; break;
                }
            }
            else
            {
                out[n++] = cell[i];
            }
        }
    }
    out[n] = '\0';
    *data = out;
    *dlen = n;
    return 0;
}

/* Monotonic time in microseconds. */
static uint64_t
now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* The TE status of an error class. */
static te_errno
cls2rc(int cls)
{
    te_errno e;

    switch (cls)
    {
        case TA_SQL_ERR_NONE: return 0;
        case TA_SQL_ERR_SYNTAX: e = TE_EINVAL; break;
        case TA_SQL_ERR_NOT_FOUND: e = TE_ENOENT; break;
        case TA_SQL_ERR_CONSTRAINT: e = TE_EEXIST; break;
        case TA_SQL_ERR_DEADLOCK: e = TE_EDEADLK; break;
        case TA_SQL_ERR_SERIALIZATION: e = TE_EAGAIN; break;
        case TA_SQL_ERR_LOCK_TIMEOUT: e = TE_ETIMEDOUT; break;
        case TA_SQL_ERR_TIMEOUT: e = TE_ETIMEDOUT; break;
        case TA_SQL_ERR_CANCELLED: e = TE_ECANCELED; break;
        case TA_SQL_ERR_CONN_LOST: e = TE_ECONNRESET; break;
        case TA_SQL_ERR_AUTH: e = TE_EACCES; break;
        case TA_SQL_ERR_TLS: e = TE_EPROTO; break;
        case TA_SQL_ERR_PERMISSION: e = TE_EPERM; break;
        case TA_SQL_ERR_CONNECT: e = TE_ECONNREFUSED; break;
        default: e = TE_EFAIL; break;
    }
    return TE_RC(TE_TA_UNIX, e);
}

/*
 * Replace every occurrence of the secret in a message with "***". A
 * client library has no business quoting a password, but some quote
 * the whole connection string on a parse error.
 */
static void
redact(char *message, size_t size, const char *secret)
{
    char   out[sizeof(((ta_sql_errinfo *)NULL)->message)];
    size_t slen;
    size_t used = 0;
    char  *p = message;

    if (secret == NULL || (slen = strlen(secret)) < 3 ||
        strstr(message, secret) == NULL)
        return;

    while (*p != '\0' && used + 4 < sizeof(out))
    {
        if (strncmp(p, secret, slen) == 0)
        {
            memcpy(out + used, "***", 3);
            used += 3;
            p += slen;
        }
        else
        {
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    snprintf(message, size, "%s", out);
}

/* Find a driver by engine bit. */
static const ta_sql_driver *
driver_find(int engine)
{
    size_t i;

    for (i = 0; drivers[i] != NULL; i++)
    {
        if (drivers[i]->engine == engine)
            return drivers[i];
    }
    return NULL;
}

/* A connection by handle, or NULL. */
static sql_conn *
conn_get(int handle)
{
    if (handle < 0 || handle >= TA_SQL_MAX_CONNS || !conns[handle].used)
        return NULL;
    return &conns[handle];
}

/* A statement by handle, or NULL. */
static sql_stmt *
stmt_get(int handle)
{
    if (handle < 0 || handle >= TA_SQL_MAX_STMTS || !stmts[handle].used)
        return NULL;
    return &stmts[handle];
}

/* Is the connection running (or holding) an asynchronous statement? */
static bool
conn_busy(sql_conn *c)
{
    bool busy;

    pthread_mutex_lock(&c->alock);
    busy = c->astate != ASYNC_IDLE;
    pthread_mutex_unlock(&c->alock);
    return busy;
}

/* Look a connection up for an ordinary (synchronous) operation. */
static te_errno
conn_for_op(int handle, sql_conn **conn)
{
    sql_conn *c = conn_get(handle);

    if (c == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);
    if (conn_busy(c))
        return TE_RC(TE_TA_UNIX, TE_EBUSY);
    *conn = c;
    return 0;
}

/* Keep a driver failure as the connection's last error. */
static te_errno
conn_fail(sql_conn *c, const ta_sql_errinfo *err)
{
    c->err = *err;
    if (c->err.cls == TA_SQL_ERR_NONE)
        c->err.cls = TA_SQL_ERR_OTHER;
    redact(c->err.message, sizeof(c->err.message), c->secret);
    return cls2rc(c->err.cls);
}

/* Close a statement slot. */
static void
stmt_close(sql_stmt *s)
{
    sql_conn *c = &conns[s->conn];

    c->drv->finish(s->ds);
    te_string_free(&s->held);
    memset(s, 0, sizeof(*s));
}

/* ---- available / connect / disconnect ----------------------------- */

/* See description in ta_sql.h */
bool
ta_sql_available(unsigned int *engines, te_string *versions)
{
    unsigned int mask = 0;
    size_t       i;

    for (i = 0; drivers[i] != NULL; i++)
    {
        mask |= (unsigned int)drivers[i]->engine;
        if (versions != NULL)
        {
            if (i > 0)
                te_string_append(versions, "; ");
            drivers[i]->version(versions);
        }
    }
    if (engines != NULL)
        *engines = mask;
    return mask != 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_connect(const ta_sql_config *cfg, int *handle, uint64_t *elapsed_us,
               ta_sql_errinfo *err)
{
    const ta_sql_driver *drv = driver_find(cfg->engine);
    ta_sql_errinfo       local;
    sql_conn            *c = NULL;
    uint64_t             t0;
    void                *dc = NULL;
    int                  i;

    memset(&local, 0, sizeof(local));
    if (err == NULL)
        err = &local;
    memset(err, 0, sizeof(*err));

    if (drv == NULL)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "no driver for engine %d in this agent", cfg->engine);
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
    }

    for (i = 0; i < TA_SQL_MAX_CONNS; i++)
    {
        if (!conns[i].used)
        {
            c = &conns[i];
            break;
        }
    }
    if (c == NULL)
    {
        ta_sql_err_set(err, TA_SQL_ERR_OTHER, 0, NULL,
                       "too many connections (%d)", TA_SQL_MAX_CONNS);
        return TE_RC(TE_TA_UNIX, TE_EMFILE);
    }

    t0 = now_us();
    if (drv->connect(cfg, &dc, err) != 0)
    {
        if (err->cls == TA_SQL_ERR_NONE)
            err->cls = TA_SQL_ERR_CONNECT;
        redact(err->message, sizeof(err->message), cfg->password);
        return cls2rc(err->cls);
    }
    if (elapsed_us != NULL)
        *elapsed_us = now_us() - t0;

    memset(c, 0, sizeof(*c));
    c->used = true;
    c->drv = drv;
    c->dc = dc;
    if (cfg->password != NULL && cfg->password[0] != '\0')
        c->secret = strdup(cfg->password);
    pthread_mutex_init(&c->alock, NULL);
    pthread_cond_init(&c->acond, NULL);
    c->astate = ASYNC_IDLE;

    *handle = i;
    return 0;
}

/* Collect the asynchronous statement of a connection being closed. */
static void
async_abandon(sql_conn *c)
{
    bool running;

    pthread_mutex_lock(&c->alock);
    running = c->astate != ASYNC_IDLE;
    pthread_mutex_unlock(&c->alock);

    if (!running)
        return;

    if (c->drv->cancel != NULL)
        c->drv->cancel(c->dc);
    pthread_join(c->athread, NULL);
    free(c->asql);
    c->asql = NULL;
    c->astate = ASYNC_IDLE;
}

/* See description in ta_sql.h */
te_errno
ta_sql_disconnect(int handle)
{
    sql_conn *c = conn_get(handle);
    int       i;

    if (c == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);

    async_abandon(c);

    for (i = 0; i < TA_SQL_MAX_STMTS; i++)
    {
        if (stmts[i].used && stmts[i].conn == handle)
            stmt_close(&stmts[i]);
    }

    c->drv->disconnect(c->dc);
    pthread_mutex_destroy(&c->alock);
    pthread_cond_destroy(&c->acond);
    if (c->secret != NULL)
    {
        memset(c->secret, 0, strlen(c->secret));
        free(c->secret);
    }
    memset(c, 0, sizeof(*c));
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_ping(int handle, uint64_t *elapsed_us)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    uint64_t       t0;
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    t0 = now_us();
    if (c->drv->ping(c->dc, &err) != 0)
    {
        if (err.cls == TA_SQL_ERR_NONE || err.cls == TA_SQL_ERR_OTHER)
            err.cls = TA_SQL_ERR_CONN_LOST;
        return conn_fail(c, &err);
    }
    if (elapsed_us != NULL)
        *elapsed_us = now_us() - t0;
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_capabilities(int handle, unsigned int *caps, unsigned int *isolation,
                    unsigned int *default_iso, int *tls, te_string *server)
{
    sql_conn *c = NULL;
    te_errno  rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    *caps = c->drv->caps;
    *isolation = c->drv->isolation;
    *default_iso = c->drv->default_iso;
    *tls = c->drv->tls_in_use != NULL ? c->drv->tls_in_use(c->dc) : 0;
    if (server != NULL)
        c->drv->server_version(c->dc, server);
    return 0;
}

/* ---- execute ------------------------------------------------------ */

/* See description in ta_sql.h */
te_errno
ta_sql_execute(int handle, const char *sql, int64_t *affected,
               uint64_t *elapsed_us)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    int64_t        n = -1;
    uint64_t       t0;
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    t0 = now_us();
    if (c->drv->exec(c->dc, sql, &n, &err) != 0)
    {
        if (elapsed_us != NULL)
            *elapsed_us = now_us() - t0;
        return conn_fail(c, &err);
    }
    if (elapsed_us != NULL)
        *elapsed_us = now_us() - t0;
    if (affected != NULL)
        *affected = n;
    return 0;
}

/* ---- prepared statements ------------------------------------------ */

/* See description in ta_sql.h */
te_errno
ta_sql_prepare(int handle, const char *sql, int *stmt, int *nparams)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    sql_stmt      *s = NULL;
    void          *ds = NULL;
    int            n = 0;
    int            i;
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    for (i = 0; i < TA_SQL_MAX_STMTS; i++)
    {
        if (!stmts[i].used)
        {
            s = &stmts[i];
            break;
        }
    }
    if (s == NULL)
    {
        ta_sql_err_set(&err, TA_SQL_ERR_OTHER, 0, NULL,
                       "too many statements (%d)", TA_SQL_MAX_STMTS);
        conn_fail(c, &err);
        return TE_RC(TE_TA_UNIX, TE_EMFILE);
    }

    if (c->drv->prepare(c->dc, sql, &ds, &n, &err) != 0)
        return conn_fail(c, &err);

    memset(s, 0, sizeof(*s));
    s->used = true;
    s->conn = handle;
    s->ds = ds;
    s->held = (te_string)TE_STRING_INIT;

    *stmt = i;
    if (nparams != NULL)
        *nparams = n;
    return 0;
}

/* A statement and its connection, for a synchronous operation. */
static te_errno
stmt_for_op(int stmt, sql_stmt **sp, sql_conn **cp)
{
    sql_stmt *s = stmt_get(stmt);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);
    if (conn_busy(&conns[s->conn]))
        return TE_RC(TE_TA_UNIX, TE_EBUSY);
    *sp = s;
    *cp = &conns[s->conn];
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_bind(int stmt, int index, const char *cell)
{
    ta_sql_errinfo err;
    sql_stmt      *s = NULL;
    sql_conn      *c = NULL;
    char          *data = NULL;
    size_t         len = 0;
    char           tag = 0;
    te_errno       rc;
    int            res;

    if ((rc = stmt_for_op(stmt, &s, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    if (index < 1 || cell == NULL ||
        ta_sql_cell_decode(cell, strlen(cell), &tag, &data, &len) != 0 ||
        strchr("NBIUFDTJMX", tag) == NULL)
    {
        free(data);
        ta_sql_err_set(&err, TA_SQL_ERR_OTHER, 0, NULL,
                       "bad parameter %d: not a value cell", index);
        conn_fail(c, &err);
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    }

    res = c->drv->bind(s->ds, index - 1, tag, data, len, &err);
    free(data);
    if (res != 0)
        return conn_fail(c, &err);
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_stmt_execute(int stmt, int *ncols, te_string *columns,
                    int64_t *affected, uint64_t *elapsed_us)
{
    ta_sql_errinfo err;
    sql_stmt      *s = NULL;
    sql_conn      *c = NULL;
    int64_t        n = -1;
    int            cols = 0;
    uint64_t       t0;
    te_errno       rc;
    int            i;

    if ((rc = stmt_for_op(stmt, &s, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    c->drv->reset(s->ds);
    s->open = false;
    te_string_reset(&s->held);

    t0 = now_us();
    if (c->drv->execute(s->ds, &cols, &n, &err) != 0)
    {
        if (elapsed_us != NULL)
            *elapsed_us = now_us() - t0;
        return conn_fail(c, &err);
    }
    if (elapsed_us != NULL)
        *elapsed_us = now_us() - t0;

    s->ncols = cols;
    s->open = cols > 0;
    if (ncols != NULL)
        *ncols = cols;
    if (affected != NULL)
        *affected = n;
    if (columns != NULL)
    {
        for (i = 0; i < cols; i++)
        {
            const char *name = c->drv->col_name(s->ds, i);
            char        letter = c->drv->col_type(s->ds, i);

            if (i > 0)
                te_string_append(columns, "\n");
            /* The name goes through the text escape, minus its tag. */
            if (name == NULL)
                name = "";
            {
                te_string esc = TE_STRING_INIT;

                ta_sql_cell_text(&esc, 'T', name, strlen(name));
                te_string_append(columns, "%s\t%c", esc.ptr + 1, letter);
                te_string_free(&esc);
            }
        }
    }
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_fetch(int stmt, int max_rows, int max_bytes, te_string *rows,
             int *nrows, int *eof)
{
    ta_sql_errinfo err;
    sql_stmt      *s = NULL;
    sql_conn      *c = NULL;
    size_t         start = rows->len;
    int            n = 0;
    te_errno       rc;
    int            i;

    if ((rc = stmt_for_op(stmt, &s, &c)) != 0)
        return rc;

    if (max_rows <= 0)
        max_rows = TA_SQL_DEF_ROWS;
    if (max_bytes <= 0)
        max_bytes = TA_SQL_DEF_BYTES;

    *eof = 0;
    memset(&err, 0, sizeof(err));

    while (n < max_rows)
    {
        if (s->held.len == 0)
        {
            int res;

            if (!s->open)
            {
                *eof = 1;
                break;
            }
            res = c->drv->step(s->ds, &err);
            if (res < 0)
            {
                s->open = false;
                c->drv->reset(s->ds);
                return conn_fail(c, &err);
            }
            if (res == 0)
            {
                s->open = false;
                c->drv->reset(s->ds);
                *eof = 1;
                break;
            }
            for (i = 0; i < s->ncols; i++)
            {
                if (i > 0)
                    te_string_append(&s->held, "\t");
                c->drv->cell(s->ds, i, &s->held);
            }
        }

        /* The byte bound never holds the first row of a batch back. */
        if (n > 0 &&
            rows->len - start + 1 + s->held.len > (size_t)max_bytes)
            break;

        if (n > 0)
            te_string_append(rows, "\n");
        te_string_append_buf(rows, s->held.ptr, s->held.len);
        te_string_reset(&s->held);
        n++;
    }

    if (rows->len - start > max_batch_bytes)
        max_batch_bytes = rows->len - start;
    *nrows = n;
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_stmt_free(int stmt)
{
    sql_stmt *s = stmt_get(stmt);

    if (s == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);
    if (conn_busy(&conns[s->conn]))
        return TE_RC(TE_TA_UNIX, TE_EBUSY);
    stmt_close(s);
    return 0;
}

/* ---- transactions ------------------------------------------------- */

/* The SQL name of an isolation level. */
static const char *
iso_name(unsigned int isolation)
{
    switch (isolation)
    {
        case TA_SQL_ISO_READ_UNCOMMITTED: return "READ UNCOMMITTED";
        case TA_SQL_ISO_READ_COMMITTED: return "READ COMMITTED";
        case TA_SQL_ISO_REPEATABLE_READ: return "REPEATABLE READ";
        case TA_SQL_ISO_SERIALIZABLE: return "SERIALIZABLE";
        default: return NULL;
    }
}

/* Is this a name a savepoint may have? */
static bool
ident_ok(const char *name)
{
    const char *p;

    if (name == NULL || *name == '\0' || strlen(name) > 63)
        return false;
    for (p = name; *p != '\0'; p++)
    {
        if (!isalnum((unsigned char)*p) && *p != '_')
            return false;
    }
    return !isdigit((unsigned char)name[0]);
}

/* Run a statement the core wrote itself. */
static te_errno
conn_exec(sql_conn *c, const char *sql)
{
    ta_sql_errinfo err;

    memset(&err, 0, sizeof(err));
    if (c->drv->exec(c->dc, sql, NULL, &err) != 0)
        return conn_fail(c, &err);
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_tx(int handle, int op, unsigned int isolation, const char *name)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    char           sql[160];
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    switch (op)
    {
        case TA_SQL_TX_BEGIN:
            if (isolation != TA_SQL_ISO_DEFAULT &&
                ((c->drv->isolation & isolation) == 0 ||
                 iso_name(isolation) == NULL))
            {
                ta_sql_err_set(&err, TA_SQL_ERR_OTHER, 0, NULL,
                               "isolation level 0x%x is not supported by %s",
                               isolation, c->drv->name);
                conn_fail(c, &err);
                return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
            }
            if (c->drv->engine == TA_SQL_ENGINE_SQLITE)
                return conn_exec(c, "BEGIN");
            if (isolation == TA_SQL_ISO_DEFAULT)
                return conn_exec(c, c->drv->engine == TA_SQL_ENGINE_MYSQL ?
                                    "START TRANSACTION" : "BEGIN");
            if (c->drv->engine == TA_SQL_ENGINE_MYSQL)
            {
                /* Applies to the next transaction only. */
                snprintf(sql, sizeof(sql),
                         "SET TRANSACTION ISOLATION LEVEL %s",
                         iso_name(isolation));
                if ((rc = conn_exec(c, sql)) != 0)
                    return rc;
                return conn_exec(c, "START TRANSACTION");
            }
            snprintf(sql, sizeof(sql), "BEGIN ISOLATION LEVEL %s",
                     iso_name(isolation));
            return conn_exec(c, sql);

        case TA_SQL_TX_COMMIT:
            return conn_exec(c, "COMMIT");

        case TA_SQL_TX_ROLLBACK:
            return conn_exec(c, "ROLLBACK");

        case TA_SQL_TX_SAVEPOINT:
        case TA_SQL_TX_ROLLBACK_TO:
        case TA_SQL_TX_RELEASE:
            if (!ident_ok(name))
            {
                ta_sql_err_set(&err, TA_SQL_ERR_OTHER, 0, NULL,
                               "bad savepoint name");
                conn_fail(c, &err);
                return TE_RC(TE_TA_UNIX, TE_EINVAL);
            }
            snprintf(sql, sizeof(sql), "%s %s",
                     op == TA_SQL_TX_SAVEPOINT ? "SAVEPOINT" :
                     op == TA_SQL_TX_ROLLBACK_TO ? "ROLLBACK TO SAVEPOINT" :
                     "RELEASE SAVEPOINT", name);
            return conn_exec(c, sql);

        default:
            return TE_RC(TE_TA_UNIX, TE_EINVAL);
    }
}

/* ---- introspection ------------------------------------------------ */

/* See description in ta_sql.h */
te_errno
ta_sql_meta(int handle, int kind, const char *table, te_string *rows)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    if (kind != TA_SQL_META_TABLES && (table == NULL || *table == '\0'))
    {
        ta_sql_err_set(&err, TA_SQL_ERR_OTHER, 0, NULL,
                       "introspection needs a table name");
        conn_fail(c, &err);
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    }

    if (ta_sql_meta_build(c->drv, c->dc, kind, table, rows, &err) != 0)
        return conn_fail(c, &err);
    return 0;
}

/* ---- deadlines, asynchronous statements, cancellation ------------- */

/* See description in ta_sql.h */
te_errno
ta_sql_set_timeout(int handle, int kind, int ms)
{
    ta_sql_errinfo err;
    sql_conn      *c = NULL;
    unsigned int   cap;
    te_errno       rc;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    memset(&err, 0, sizeof(err));
    cap = kind == TA_SQL_TIMEOUT_STATEMENT ? TA_SQL_CAP_STMT_TIMEOUT :
          kind == TA_SQL_TIMEOUT_LOCK ? TA_SQL_CAP_LOCK_TIMEOUT : 0;
    if (cap == 0 || ms < 0)
        return TE_RC(TE_TA_UNIX, TE_EINVAL);
    if ((c->drv->caps & cap) == 0 || c->drv->set_timeout == NULL)
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);

    if (c->drv->set_timeout(c->dc, kind, ms, &err) != 0)
        return conn_fail(c, &err);
    return 0;
}

/* The thread of an asynchronous statement. */
static void *
async_main(void *arg)
{
    sql_conn      *c = arg;
    ta_sql_errinfo err;
    int64_t        affected = -1;
    uint64_t       t0;
    int            res;

    memset(&err, 0, sizeof(err));
    if (c->drv->thread_init != NULL)
        c->drv->thread_init();

    t0 = now_us();
    res = c->drv->exec(c->dc, c->asql, &affected, &err);

    pthread_mutex_lock(&c->alock);
    c->aelapsed = now_us() - t0;
    c->arc = res;
    c->aaffected = affected;
    c->aerr = err;
    c->astate = ASYNC_DONE;
    pthread_cond_broadcast(&c->acond);
    pthread_mutex_unlock(&c->alock);

    if (c->drv->thread_fini != NULL)
        c->drv->thread_fini();
    return NULL;
}

/* See description in ta_sql.h */
te_errno
ta_sql_async_start(int handle, const char *sql)
{
    sql_conn *c = NULL;
    te_errno  rc;
    int       res;

    if ((rc = conn_for_op(handle, &c)) != 0)
        return rc;

    c->asql = strdup(sql != NULL ? sql : "");
    if (c->asql == NULL)
        return TE_RC(TE_TA_UNIX, TE_ENOMEM);

    pthread_mutex_lock(&c->alock);
    c->astate = ASYNC_RUNNING;
    pthread_mutex_unlock(&c->alock);

    res = pthread_create(&c->athread, NULL, async_main, c);
    if (res != 0)
    {
        c->astate = ASYNC_IDLE;
        free(c->asql);
        c->asql = NULL;
        return TE_RC(TE_TA_UNIX, TE_EAGAIN);
    }
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_async_wait(int handle, int timeout_ms, int *done, int64_t *affected,
                  uint64_t *elapsed_us)
{
    sql_conn       *c = conn_get(handle);
    struct timespec deadline;
    int             state;
    int             res;

    *done = 0;
    if (c == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);

    clock_gettime(CLOCK_REALTIME, &deadline);
    if (timeout_ms > 0)
    {
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&c->alock);
    if (c->astate == ASYNC_IDLE)
    {
        pthread_mutex_unlock(&c->alock);
        return TE_RC(TE_TA_UNIX, TE_ENOENT);
    }
    res = 0;
    while (c->astate == ASYNC_RUNNING && timeout_ms > 0 && res == 0)
        res = pthread_cond_timedwait(&c->acond, &c->alock, &deadline);
    state = c->astate;
    pthread_mutex_unlock(&c->alock);

    if (state == ASYNC_RUNNING)
        return TE_RC(TE_TA_UNIX, TE_EINPROGRESS);

    pthread_join(c->athread, NULL);
    free(c->asql);
    c->asql = NULL;
    c->astate = ASYNC_IDLE;

    *done = 1;
    if (elapsed_us != NULL)
        *elapsed_us = c->aelapsed;
    if (c->arc != 0)
        return conn_fail(c, &c->aerr);
    if (affected != NULL)
        *affected = c->aaffected;
    return 0;
}

/* See description in ta_sql.h */
te_errno
ta_sql_cancel(int handle)
{
    sql_conn *c = conn_get(handle);

    if (c == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);
    if ((c->drv->caps & TA_SQL_CAP_CANCEL) == 0 || c->drv->cancel == NULL)
        return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
    if (c->drv->cancel(c->dc) != 0)
        return TE_RC(TE_TA_UNIX, TE_EFAIL);
    return 0;
}

/* ---- last error, statistics --------------------------------------- */

/* See description in ta_sql.h */
te_errno
ta_sql_error(int handle, ta_sql_errinfo *err)
{
    sql_conn *c = conn_get(handle);

    if (c == NULL)
        return TE_RC(TE_TA_UNIX, TE_EBADF);
    *err = c->err;
    return 0;
}

/* See description in ta_sql.h */
void
ta_sql_stats(int *conns_open, int *stmts_open, uint64_t *max_batch)
{
    int n;
    int i;

    for (n = 0, i = 0; i < TA_SQL_MAX_CONNS; i++)
        n += conns[i].used ? 1 : 0;
    if (conns_open != NULL)
        *conns_open = n;

    for (n = 0, i = 0; i < TA_SQL_MAX_STMTS; i++)
        n += stmts[i].used ? 1 : 0;
    if (stmts_open != NULL)
        *stmts_open = n;

    if (max_batch != NULL)
        *max_batch = max_batch_bytes;
}
