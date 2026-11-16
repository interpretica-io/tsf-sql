/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL databases from a test
 *
 * The engine-side layer over the sql_* RPCs: it keeps the opaque
 * connection and statement objects, turns typed values into the cells
 * the agent takes and the rows it returns back into typed values, and
 * shapes the introspection answers into the public structures. The
 * database client itself runs on the agent.
 */

#define TE_LGR_USER     "TAPI SQL"

#include "te_config.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_sql.h"
#include "tapi_sql_rpc.h"

/** Operations of the sql_tx RPC. */
enum {
    TX_BEGIN = 1,
    TX_COMMIT = 2,
    TX_ROLLBACK = 3,
    TX_SAVEPOINT = 4,
    TX_ROLLBACK_TO = 5,
    TX_RELEASE = 6,
};

/** Kinds of the sql_meta RPC. */
enum {
    META_TABLES = 1,
    META_COLUMNS = 2,
    META_INDEXES = 3,
    META_CONSTRAINTS = 4,
    META_FOREIGN_KEYS = 5,
};

/** Rows asked for at a time when a whole result is wanted. */
#define QUERY_BATCH_ROWS    1000

struct tapi_sql_conn {
    rcf_rpc_server *rpcs;
    int             handle;
    tapi_sql_config cfg;        /**< Owned copies of the strings. */
    tapi_sql_error  err;
    uint64_t        elapsed_us;
    bool            caps_known;
    tapi_sql_caps   caps;
};

struct tapi_sql_stmt {
    tapi_sql_conn  *conn;
    int             handle;
    unsigned int    nparams;
    unsigned int    ncols;
    char          **names;
    tapi_sql_type  *types;
};

/* ---- names -------------------------------------------------------- */

/* See description in tapi_sql.h */
const char *
tapi_sql_engine2str(tapi_sql_engine engine)
{
    switch (engine)
    {
        case TAPI_SQL_ENGINE_SQLITE: return "sqlite";
        case TAPI_SQL_ENGINE_POSTGRES: return "postgres";
        case TAPI_SQL_ENGINE_MYSQL: return "mysql";
        default: return "unknown";
    }
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_engine_from_str(const char *name, tapi_sql_engine *engine)
{
    if (name == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    if (strcasecmp(name, "sqlite") == 0)
        *engine = TAPI_SQL_ENGINE_SQLITE;
    else if (strcasecmp(name, "postgres") == 0 ||
             strcasecmp(name, "postgresql") == 0 ||
             strcasecmp(name, "pg") == 0)
        *engine = TAPI_SQL_ENGINE_POSTGRES;
    else if (strcasecmp(name, "mysql") == 0 ||
             strcasecmp(name, "mariadb") == 0)
        *engine = TAPI_SQL_ENGINE_MYSQL;
    else
        return TE_RC(TE_TAPI, TE_EINVAL);
    return 0;
}

/* See description in tapi_sql.h */
const char *
tapi_sql_err_class2str(tapi_sql_err_class cls)
{
    switch (cls)
    {
        case TAPI_SQL_ERR_NONE: return "none";
        case TAPI_SQL_ERR_SYNTAX: return "syntax";
        case TAPI_SQL_ERR_NOT_FOUND: return "not found";
        case TAPI_SQL_ERR_CONSTRAINT: return "constraint";
        case TAPI_SQL_ERR_DEADLOCK: return "deadlock";
        case TAPI_SQL_ERR_SERIALIZATION: return "serialization";
        case TAPI_SQL_ERR_LOCK_TIMEOUT: return "lock timeout";
        case TAPI_SQL_ERR_TIMEOUT: return "timeout";
        case TAPI_SQL_ERR_CANCELLED: return "cancelled";
        case TAPI_SQL_ERR_CONN_LOST: return "connection lost";
        case TAPI_SQL_ERR_AUTH: return "authentication";
        case TAPI_SQL_ERR_TLS: return "tls";
        case TAPI_SQL_ERR_PERMISSION: return "permission";
        case TAPI_SQL_ERR_CONNECT: return "connect";
        default: return "other";
    }
}

/* See description in tapi_sql.h */
const char *
tapi_sql_isolation2str(tapi_sql_isolation isolation)
{
    switch (isolation)
    {
        case TAPI_SQL_ISO_DEFAULT: return "default";
        case TAPI_SQL_ISO_READ_UNCOMMITTED: return "read uncommitted";
        case TAPI_SQL_ISO_READ_COMMITTED: return "read committed";
        case TAPI_SQL_ISO_REPEATABLE_READ: return "repeatable read";
        case TAPI_SQL_ISO_SERIALIZABLE: return "serializable";
        default: return "unknown";
    }
}

/* ---- the value format --------------------------------------------- */

/* Append text with the wire's escapes. */
static void
escape(te_string *out, const char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        switch (data[i])
        {
            case '\\': te_string_append(out, "\\\\"); break;
            case '\t': te_string_append(out, "\\t"); break;
            case '\n': te_string_append(out, "\\n"); break;
            case '\r': te_string_append(out, "\\r"); break;
            case '\0': te_string_append(out, "\\0"); break;
            default: te_string_append_buf(out, &data[i], 1); break;
        }
    }
}

/* Encode a value as a cell. */
static void
cell_encode(te_string *out, const tapi_sql_value *v)
{
    size_t i;

    switch (v->type)
    {
        case TAPI_SQL_NULL:
            te_string_append(out, "N");
            break;
        case TAPI_SQL_BOOL:
            te_string_append(out, "B%d", v->i != 0);
            break;
        case TAPI_SQL_INT:
            te_string_append(out, "I%lld", (long long)v->i);
            break;
        case TAPI_SQL_UINT:
            te_string_append(out, "U%llu", (unsigned long long)v->u);
            break;
        case TAPI_SQL_FLOAT:
            if (isnan(v->f))
                te_string_append(out, "Fnan");
            else if (isinf(v->f))
                te_string_append(out, v->f > 0 ? "Finf" : "F-inf");
            else
                te_string_append(out, "F%.17g", v->f);
            break;
        case TAPI_SQL_BINARY:
            te_string_append(out, "X");
            for (i = 0; i < v->len; i++)
                te_string_append(out, "%02x", (unsigned char)v->str[i]);
            break;
        default:
            te_string_append(out, "%c", (char)v->type);
            if (v->str != NULL)
                escape(out, v->str, v->len);
            break;
    }
}

/* Decode one cell into a value that owns its string. */
static te_errno
cell_decode(const char *cell, size_t len, tapi_sql_value *v)
{
    char  *buf;
    size_t n = 0;
    size_t i;

    memset(v, 0, sizeof(*v));
    if (len == 0 || strchr("NBIUFDTJMX", cell[0]) == NULL)
        return TE_RC(TE_TAPI, TE_EBADMSG);

    v->type = (tapi_sql_type)cell[0];
    if (v->type == TAPI_SQL_NULL)
        return 0;

    buf = TE_ALLOC(len + 1);
    if (v->type == TAPI_SQL_BINARY)
    {
        for (i = 1; i + 1 < len; i += 2)
        {
            char         pair[3] = { cell[i], cell[i + 1], '\0' };
            unsigned int byte = 0;

            if (sscanf(pair, "%2x", &byte) != 1)
            {
                free(buf);
                return TE_RC(TE_TAPI, TE_EBADMSG);
            }
            buf[n++] = (char)byte;
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
                    case 't': buf[n++] = '\t'; break;
                    case 'n': buf[n++] = '\n'; break;
                    case 'r': buf[n++] = '\r'; break;
                    case '0': buf[n++] = '\0'; break;
                    default: buf[n++] = cell[i]; break;
                }
            }
            else
            {
                buf[n++] = cell[i];
            }
        }
    }
    buf[n] = '\0';
    v->str = buf;
    v->len = n;

    switch (v->type)
    {
        case TAPI_SQL_BOOL:
            v->i = buf[0] == '1';
            v->u = (uint64_t)v->i;
            v->f = (double)v->i;
            break;
        case TAPI_SQL_INT:
            v->i = strtoll(buf, NULL, 10);
            v->u = (uint64_t)v->i;
            v->f = (double)v->i;
            break;
        case TAPI_SQL_UINT:
            v->u = strtoull(buf, NULL, 10);
            v->i = (int64_t)v->u;
            v->f = (double)v->u;
            break;
        case TAPI_SQL_FLOAT:
        case TAPI_SQL_DECIMAL:
            v->f = strtod(buf, NULL);
            v->i = (int64_t)v->f;
            break;
        default:
            break;
    }
    return 0;
}

static void
value_free(tapi_sql_value *v)
{
    free((char *)(uintptr_t)v->str);
    memset(v, 0, sizeof(*v));
}

/* Free the rows of a result, keeping its columns. */
static void
result_drop_rows(tapi_sql_result *res)
{
    unsigned int i;

    for (i = 0; i < res->nrows * res->ncols; i++)
        value_free(&res->cells[i]);
    res->nrows = 0;
}

/* See description in tapi_sql.h */
void
tapi_sql_result_free(tapi_sql_result *res)
{
    unsigned int i;

    if (res == NULL)
        return;

    result_drop_rows(res);
    for (i = 0; i < res->ncols; i++)
        free(res->names != NULL ? res->names[i] : NULL);
    free(res->names);
    free(res->types);
    free(res->cells);
    memset(res, 0, sizeof(*res));
}

/* Give a result its columns (when it has none yet). */
static void
result_set_columns(tapi_sql_result *res, unsigned int ncols,
                   char *const *names, const tapi_sql_type *types)
{
    unsigned int i;

    if (res->ncols != 0 || ncols == 0)
        return;

    res->ncols = ncols;
    res->names = TE_ALLOC(ncols * sizeof(*res->names));
    res->types = TE_ALLOC(ncols * sizeof(*res->types));
    for (i = 0; i < ncols; i++)
    {
        res->names[i] = TE_STRDUP(names != NULL ? names[i] : "");
        res->types[i] = types != NULL ? types[i] : TAPI_SQL_TEXT;
    }
}

/*
 * Append the rows of a batch (as the agent wrote them) to a result.
 * A result without columns takes its width from the first row.
 */
static te_errno
result_append(tapi_sql_result *res, const char *text, unsigned int nrows)
{
    const char *line = text;
    te_errno    rc = 0;

    if (nrows == 0 || text == NULL)
        return 0;

    while (line != NULL && rc == 0)
    {
        const char  *eol = strchr(line, '\n');
        const char  *end = eol != NULL ? eol : line + strlen(line);
        const char  *p = line;
        unsigned int col = 0;

        if (res->ncols == 0)
        {
            unsigned int width = 1;
            const char  *q;

            for (q = line; q < end; q++)
                width += *q == '\t' ? 1 : 0;
            result_set_columns(res, width, NULL, NULL);
        }

        if (res->nrows == res->cap_rows)
        {
            res->cap_rows = res->cap_rows == 0 ? 16 : res->cap_rows * 2;
            res->cells = realloc(res->cells, (size_t)res->cap_rows *
                                 res->ncols * sizeof(*res->cells));
            if (res->cells == NULL)
                TE_FATAL_ERROR("Out of memory");
        }

        for (col = 0; col < res->ncols; col++)
        {
            tapi_sql_value *v = &res->cells[res->nrows * res->ncols + col];
            const char     *tab = memchr(p, '\t', (size_t)(end - p));
            const char     *cend = tab != NULL ? tab : end;

            rc = cell_decode(p, (size_t)(cend - p), v);
            if (rc != 0)
            {
                ERROR("Malformed cell %u in a row from the agent", col);
                memset(v, 0, sizeof(*v));
                v->type = TAPI_SQL_NULL;
            }
            p = tab != NULL ? tab + 1 : end;
        }
        res->nrows++;

        line = eol != NULL ? eol + 1 : NULL;
    }
    return rc;
}

/* See description in tapi_sql.h */
const tapi_sql_value *
tapi_sql_result_cell(const tapi_sql_result *res, unsigned int row,
                     unsigned int col)
{
    if (res == NULL || row >= res->nrows || col >= res->ncols)
        return NULL;
    return &res->cells[row * res->ncols + col];
}

/* See description in tapi_sql.h */
void
tapi_sql_value_print(const tapi_sql_value *value, te_string *dest)
{
    size_t i;

    if (value == NULL || value->type == TAPI_SQL_NULL)
    {
        te_string_append(dest, "NULL");
        return;
    }

    switch (value->type)
    {
        case TAPI_SQL_BOOL:
            te_string_append(dest, "%s", value->i != 0 ? "true" : "false");
            break;
        case TAPI_SQL_INT:
            te_string_append(dest, "%lld", (long long)value->i);
            break;
        case TAPI_SQL_UINT:
            te_string_append(dest, "%llu", (unsigned long long)value->u);
            break;
        case TAPI_SQL_FLOAT:
            te_string_append(dest, "%.17g", value->f);
            break;
        case TAPI_SQL_DECIMAL:
            te_string_append(dest, "%s", value->str != NULL ? value->str :
                                                              "");
            break;
        case TAPI_SQL_BINARY:
            te_string_append(dest, "x'");
            for (i = 0; i < value->len && i < 32; i++)
                te_string_append(dest, "%02x",
                                 (unsigned char)value->str[i]);
            te_string_append(dest, "%s'", value->len > 32 ? "..." : "");
            break;
        default:
            te_string_append(dest, "'%.*s%s'",
                             (int)(value->len > 80 ? 80 : value->len),
                             value->str != NULL ? value->str : "",
                             value->len > 80 ? "..." : "");
            break;
    }
}

/* See description in tapi_sql.h */
void
tapi_sql_result_log(const tapi_sql_result *res, const char *title,
                    unsigned int max_rows)
{
    te_string    out = TE_STRING_INIT;
    unsigned int row;
    unsigned int col;

    if (res == NULL)
        return;

    for (col = 0; col < res->ncols; col++)
        te_string_append(&out, "%s%s", col > 0 ? " | " : "",
                         res->names[col]);
    te_string_append(&out, "\n");
    for (row = 0; row < res->nrows && row < max_rows; row++)
    {
        for (col = 0; col < res->ncols; col++)
        {
            if (col > 0)
                te_string_append(&out, " | ");
            tapi_sql_value_print(tapi_sql_result_cell(res, row, col), &out);
        }
        te_string_append(&out, "\n");
    }
    if (res->nrows > max_rows)
        te_string_append(&out, "... %u more row(s)\n", res->nrows - max_rows);

    RING("%s: %u row(s), %u column(s)\n%s", title != NULL ? title : "result",
         res->nrows, res->ncols, out.ptr != NULL ? out.ptr : "");
    te_string_free(&out);
}

/* ---- drivers ------------------------------------------------------ */

/* See description in tapi_sql.h */
bool
tapi_sql_available(rcf_rpc_server *rpcs)
{
    return rpc_sql_available(rpcs, NULL, NULL) == 0;
}

/* See description in tapi_sql.h */
bool
tapi_sql_supports(rcf_rpc_server *rpcs, tapi_sql_engine engine)
{
    unsigned int engines = 0;

    if (rpc_sql_available(rpcs, &engines, NULL) != 0)
        return false;
    return (engines & (unsigned int)engine) != 0;
}

/* ---- connection management ---------------------------------------- */

static char *
dup_or_null(const char *s)
{
    return s != NULL ? TE_STRDUP(s) : NULL;
}

static void
cfg_copy(tapi_sql_config *dst, const tapi_sql_config *src)
{
    *dst = *src;
    dst->host = dup_or_null(src->host);
    dst->dbname = dup_or_null(src->dbname);
    dst->user = dup_or_null(src->user);
    dst->password = dup_or_null(src->password);
    dst->tls_ca = dup_or_null(src->tls_ca);
    dst->tls_cert = dup_or_null(src->tls_cert);
    dst->tls_key = dup_or_null(src->tls_key);
}

static void
cfg_free(tapi_sql_config *cfg)
{
    char *secret = (char *)(uintptr_t)cfg->password;

    if (secret != NULL)
        memset(secret, 0, strlen(secret));
    free((char *)(uintptr_t)cfg->host);
    free((char *)(uintptr_t)cfg->dbname);
    free((char *)(uintptr_t)cfg->user);
    free(secret);
    free((char *)(uintptr_t)cfg->tls_ca);
    free((char *)(uintptr_t)cfg->tls_cert);
    free((char *)(uintptr_t)cfg->tls_key);
    memset(cfg, 0, sizeof(*cfg));
}

static void
err_from_rpc(tapi_sql_error *dst, const rpc_sql_errinfo *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->cls = (tapi_sql_err_class)src->cls;
    dst->native = src->native;
    te_strlcpy(dst->sqlstate, src->sqlstate, sizeof(dst->sqlstate));
    te_strlcpy(dst->message, src->message, sizeof(dst->message));
}

/* Open the agent-side session of a connection object. */
static te_errno
conn_open(tapi_sql_conn *conn, tapi_sql_error *err)
{
    rpc_sql_connect_args args = {
        .engine = (int)conn->cfg.engine,
        .host = conn->cfg.host,
        .port = conn->cfg.port,
        .dbname = conn->cfg.dbname,
        .user = conn->cfg.user,
        .password = conn->cfg.password,
        .tls_mode = (int)conn->cfg.tls_mode,
        .tls_ca = conn->cfg.tls_ca,
        .tls_cert = conn->cfg.tls_cert,
        .tls_key = conn->cfg.tls_key,
        .timeout_ms = conn->cfg.timeout_ms,
    };
    rpc_sql_errinfo rerr;
    te_errno        rc;

    memset(&rerr, 0, sizeof(rerr));
    conn->handle = -1;
    conn->caps_known = false;
    rc = rpc_sql_connect(conn->rpcs, &args, &conn->handle, &conn->elapsed_us,
                         &rerr);
    err_from_rpc(&conn->err, &rerr);
    if (err != NULL)
        *err = conn->err;
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_connect_err(rcf_rpc_server *rpcs, const tapi_sql_config *cfg,
                     tapi_sql_conn **conn, tapi_sql_error *err)
{
    tapi_sql_conn *c;
    te_errno       rc;

    if (rpcs == NULL || cfg == NULL || conn == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    c = TE_ALLOC(sizeof(*c));
    c->rpcs = rpcs;
    cfg_copy(&c->cfg, cfg);

    rc = conn_open(c, err);
    if (rc != 0)
    {
        cfg_free(&c->cfg);
        free(c);
        return rc;
    }

    *conn = c;
    return 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_connect(rcf_rpc_server *rpcs, const tapi_sql_config *cfg,
                 tapi_sql_conn **conn)
{
    tapi_sql_error err;
    te_errno       rc;

    memset(&err, 0, sizeof(err));
    rc = tapi_sql_connect_err(rpcs, cfg, conn, &err);
    if (rc != 0)
        ERROR("Cannot connect to the %s database: %s (%s)",
              cfg != NULL ? tapi_sql_engine2str(cfg->engine) : "?",
              err.message, tapi_sql_err_class2str(err.cls));
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_disconnect(tapi_sql_conn *conn)
{
    te_errno rc = 0;

    if (conn == NULL)
        return 0;

    if (conn->handle >= 0)
        rc = rpc_sql_disconnect(conn->rpcs, conn->handle);
    cfg_free(&conn->cfg);
    free(conn);
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_reconnect(tapi_sql_conn *conn)
{
    if (conn == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    if (conn->handle >= 0)
        rpc_sql_disconnect(conn->rpcs, conn->handle);
    return conn_open(conn, NULL);
}

/* Fetch the reason of a failure into the connection object. */
static te_errno
conn_failed(tapi_sql_conn *conn, te_errno rc)
{
    rpc_sql_errinfo rerr;

    memset(&rerr, 0, sizeof(rerr));
    if (rc != 0 && conn->handle >= 0 &&
        rpc_sql_error(conn->rpcs, conn->handle, &rerr) == 0)
        err_from_rpc(&conn->err, &rerr);
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_ping(tapi_sql_conn *conn)
{
    return conn_failed(conn, rpc_sql_ping(conn->rpcs, conn->handle,
                                          &conn->elapsed_us));
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_capabilities(tapi_sql_conn *conn, tapi_sql_caps *caps)
{
    te_string    server = TE_STRING_INIT;
    unsigned int default_iso = 0;
    int          tls = 0;
    te_errno     rc;

    if (!conn->caps_known)
    {
        memset(&conn->caps, 0, sizeof(conn->caps));
        rc = rpc_sql_capabilities(conn->rpcs, conn->handle, &conn->caps.caps,
                                  &conn->caps.isolation, &default_iso, &tls,
                                  &server);
        if (rc != 0)
        {
            te_string_free(&server);
            return rc;
        }
        conn->caps.default_iso = (tapi_sql_isolation)default_iso;
        conn->caps.tls = tls != 0;
        te_strlcpy(conn->caps.server, server.ptr != NULL ? server.ptr : "",
                   sizeof(conn->caps.server));
        te_string_free(&server);
        conn->caps_known = true;
    }
    if (caps != NULL)
        *caps = conn->caps;
    return 0;
}

/* See description in tapi_sql.h */
bool
tapi_sql_has_cap(tapi_sql_conn *conn, tapi_sql_cap cap)
{
    tapi_sql_caps caps;

    if (tapi_sql_capabilities(conn, &caps) != 0)
        return false;
    return (caps.caps & (unsigned int)cap) != 0;
}

/* See description in tapi_sql.h */
tapi_sql_engine
tapi_sql_conn_engine(const tapi_sql_conn *conn)
{
    return conn->cfg.engine;
}

/* See description in tapi_sql.h */
rcf_rpc_server *
tapi_sql_conn_rpcs(const tapi_sql_conn *conn)
{
    return conn->rpcs;
}

/* See description in tapi_sql.h */
const tapi_sql_config *
tapi_sql_conn_config(const tapi_sql_conn *conn)
{
    return &conn->cfg;
}

/* See description in tapi_sql.h */
const tapi_sql_error *
tapi_sql_last_error(const tapi_sql_conn *conn)
{
    return &conn->err;
}

/* See description in tapi_sql.h */
uint64_t
tapi_sql_last_elapsed_us(const tapi_sql_conn *conn)
{
    return conn->elapsed_us;
}

/* See description in tapi_sql.h */
void
tapi_sql_conn_describe(const tapi_sql_conn *conn, te_string *dest)
{
    static const char *const tls[] = {
        "disable", "prefer", "require", "verify-ca", "verify-full",
    };
    const tapi_sql_config   *cfg = &conn->cfg;
    unsigned int             mode = (unsigned int)cfg->tls_mode;

    te_string_append(dest, "%s", tapi_sql_engine2str(cfg->engine));
    if (cfg->engine == TAPI_SQL_ENGINE_SQLITE)
    {
        te_string_append(dest, " file=%s",
                         cfg->dbname != NULL ? cfg->dbname : "");
        return;
    }
    te_string_append(dest, " %s:%d db=%s user=%s tls=%s",
                     cfg->host != NULL ? cfg->host : "", cfg->port,
                     cfg->dbname != NULL ? cfg->dbname : "",
                     cfg->user != NULL ? cfg->user : "",
                     mode < TE_ARRAY_LEN(tls) ? tls[mode] : "?");
}

/* ---- query execution ---------------------------------------------- */

/* See description in tapi_sql.h */
te_errno
tapi_sql_execute(tapi_sql_conn *conn, const char *sql, int64_t *affected)
{
    return conn_failed(conn, rpc_sql_execute(conn->rpcs, conn->handle, sql,
                                             affected, &conn->elapsed_us));
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_execute_fmt(tapi_sql_conn *conn, int64_t *affected, const char *fmt,
                     ...)
{
    te_string sql = TE_STRING_INIT;
    va_list   ap;
    te_errno  rc;

    va_start(ap, fmt);
    te_string_append_va(&sql, fmt, ap);
    va_end(ap);

    rc = tapi_sql_execute(conn, sql.ptr, affected);
    te_string_free(&sql);
    return rc;
}

/* Parse the "name \t letter" lines of an executed statement. */
static void
stmt_set_columns(tapi_sql_stmt *stmt, unsigned int ncols, const char *text)
{
    const char  *line = text;
    unsigned int i;

    for (i = 0; i < stmt->ncols; i++)
        free(stmt->names[i]);
    free(stmt->names);
    free(stmt->types);
    stmt->names = NULL;
    stmt->types = NULL;
    stmt->ncols = ncols;
    if (ncols == 0)
        return;

    stmt->names = TE_ALLOC(ncols * sizeof(*stmt->names));
    stmt->types = TE_ALLOC(ncols * sizeof(*stmt->types));
    for (i = 0; i < ncols; i++)
    {
        const char    *eol = line != NULL ? strchr(line, '\n') : NULL;
        const char    *end = line == NULL ? NULL :
                             eol != NULL ? eol : line + strlen(line);
        const char    *tab = line != NULL ?
                             memchr(line, '\t', (size_t)(end - line)) : NULL;
        te_string      cell = TE_STRING_INIT;
        tapi_sql_value v;

        stmt->types[i] = TAPI_SQL_TEXT;
        if (tab == NULL)
        {
            stmt->names[i] = TE_STRDUP("");
        }
        else
        {
            /* A column name is escaped like a text cell without a tag. */
            te_string_append(&cell, "T%.*s", (int)(tab - line), line);
            if (cell_decode(cell.ptr, cell.len, &v) == 0)
            {
                stmt->names[i] = TE_STRDUP(v.str != NULL ? v.str : "");
                value_free(&v);
            }
            else
            {
                stmt->names[i] = TE_STRDUP("");
            }
            if (tab + 1 < end && strchr("NBIUFDTJMX", tab[1]) != NULL)
                stmt->types[i] = (tapi_sql_type)tab[1];
            te_string_free(&cell);
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_prepare(tapi_sql_conn *conn, const char *sql, tapi_sql_stmt **stmt)
{
    tapi_sql_stmt *s;
    int            handle = -1;
    int            nparams = 0;
    te_errno       rc;

    rc = rpc_sql_prepare(conn->rpcs, conn->handle, sql, &handle, &nparams);
    if (rc != 0)
        return conn_failed(conn, rc);

    s = TE_ALLOC(sizeof(*s));
    s->conn = conn;
    s->handle = handle;
    s->nparams = (unsigned int)nparams;
    *stmt = s;
    return 0;
}

/* See description in tapi_sql.h */
unsigned int
tapi_sql_stmt_nparams(const tapi_sql_stmt *stmt)
{
    return stmt->nparams;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_bind(tapi_sql_stmt *stmt, unsigned int index, tapi_sql_value value)
{
    te_string cell = TE_STRING_INIT;
    te_errno  rc;

    cell_encode(&cell, &value);
    rc = rpc_sql_bind(stmt->conn->rpcs, stmt->handle, (int)index, cell.ptr);
    te_string_free(&cell);
    return conn_failed(stmt->conn, rc);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_stmt_execute(tapi_sql_stmt *stmt, int64_t *affected)
{
    te_string columns = TE_STRING_INIT;
    int       ncols = 0;
    te_errno  rc;

    rc = rpc_sql_stmt_execute(stmt->conn->rpcs, stmt->handle, &ncols,
                              &columns, affected,
                              &stmt->conn->elapsed_us);
    if (rc == 0)
        stmt_set_columns(stmt, (unsigned int)ncols, columns.ptr);
    te_string_free(&columns);
    return conn_failed(stmt->conn, rc);
}

/* Fetch a batch and append it to a result. */
static te_errno
stmt_fetch_append(tapi_sql_stmt *stmt, unsigned int max_rows,
                  unsigned int max_bytes, tapi_sql_result *res, bool *eof)
{
    te_string rows = TE_STRING_INIT;
    int       nrows = 0;
    bool      end = false;
    te_errno  rc;

    rc = rpc_sql_fetch(stmt->conn->rpcs, stmt->handle, (int)max_rows,
                       (int)max_bytes, &rows, &nrows, &end);
    if (rc != 0)
    {
        te_string_free(&rows);
        return conn_failed(stmt->conn, rc);
    }

    result_set_columns(res, stmt->ncols, stmt->names, stmt->types);
    rc = result_append(res, rows.ptr, (unsigned int)nrows);
    te_string_free(&rows);
    if (eof != NULL)
        *eof = end;
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_fetch_bounded(tapi_sql_stmt *stmt, unsigned int max_rows,
                       unsigned int max_bytes, tapi_sql_result *res,
                       bool *eof)
{
    if (res == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    result_drop_rows(res);
    return stmt_fetch_append(stmt, max_rows, max_bytes, res, eof);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_fetch(tapi_sql_stmt *stmt, unsigned int max_rows,
               tapi_sql_result *res, bool *eof)
{
    return tapi_sql_fetch_bounded(stmt, max_rows, 0, res, eof);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_statement_free(tapi_sql_stmt *stmt)
{
    unsigned int i;
    te_errno     rc;

    if (stmt == NULL)
        return 0;

    rc = rpc_sql_stmt_free(stmt->conn->rpcs, stmt->handle);
    /* Closing the connection first has already released it there. */
    if (TE_RC_GET_ERROR(rc) == TE_EBADF)
        rc = 0;

    for (i = 0; i < stmt->ncols; i++)
        free(stmt->names[i]);
    free(stmt->names);
    free(stmt->types);
    free(stmt);
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_query_params(tapi_sql_conn *conn, const char *sql,
                      const tapi_sql_value *params, unsigned int nparams,
                      tapi_sql_result *res, int64_t *affected)
{
    tapi_sql_stmt *stmt = NULL;
    bool           eof = false;
    unsigned int   i;
    te_errno       rc;

    rc = tapi_sql_prepare(conn, sql, &stmt);
    if (rc != 0)
        return rc;

    for (i = 0; i < nparams && rc == 0; i++)
        rc = tapi_sql_bind(stmt, i + 1, params[i]);
    if (rc == 0)
        rc = tapi_sql_stmt_execute(stmt, affected);

    if (rc == 0 && res != NULL)
    {
        tapi_sql_result_free(res);
        result_set_columns(res, stmt->ncols, stmt->names, stmt->types);
        while (rc == 0 && !eof && stmt->ncols > 0)
            rc = stmt_fetch_append(stmt, QUERY_BATCH_ROWS, 0, res, &eof);
    }

    tapi_sql_statement_free(stmt);
    return rc;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_query(tapi_sql_conn *conn, const char *sql, tapi_sql_result *res)
{
    return tapi_sql_query_params(conn, sql, NULL, 0, res, NULL);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_query_int(tapi_sql_conn *conn, const char *sql, int64_t *value)
{
    tapi_sql_result       res = TAPI_SQL_RESULT_INIT;
    const tapi_sql_value *v;
    te_errno              rc;

    rc = tapi_sql_query(conn, sql, &res);
    if (rc != 0)
        return rc;

    v = tapi_sql_result_cell(&res, 0, 0);
    if (v == NULL)
    {
        rc = TE_RC(TE_TAPI, TE_ENODATA);
    }
    else
    {
        switch (v->type)
        {
            case TAPI_SQL_BOOL:
            case TAPI_SQL_INT:
            case TAPI_SQL_UINT:
            case TAPI_SQL_FLOAT:
            case TAPI_SQL_DECIMAL:
                *value = v->i;
                break;
            case TAPI_SQL_TEXT:
            {
                char *end = NULL;

                *value = strtoll(v->str, &end, 10);
                if (end == v->str)
                    rc = TE_RC(TE_TAPI, TE_ENODATA);
                break;
            }
            default:
                rc = TE_RC(TE_TAPI, TE_ENODATA);
                break;
        }
    }
    tapi_sql_result_free(&res);
    return rc;
}

/* ---- transaction management --------------------------------------- */

static te_errno
tx(tapi_sql_conn *conn, int op, unsigned int isolation, const char *name)
{
    return conn_failed(conn, rpc_sql_tx(conn->rpcs, conn->handle, op,
                                        isolation, name));
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_begin(tapi_sql_conn *conn)
{
    return tx(conn, TX_BEGIN, TAPI_SQL_ISO_DEFAULT, NULL);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_begin_iso(tapi_sql_conn *conn, tapi_sql_isolation isolation)
{
    return tx(conn, TX_BEGIN, (unsigned int)isolation, NULL);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_commit(tapi_sql_conn *conn)
{
    return tx(conn, TX_COMMIT, 0, NULL);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_rollback(tapi_sql_conn *conn)
{
    return tx(conn, TX_ROLLBACK, 0, NULL);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_savepoint(tapi_sql_conn *conn, const char *name)
{
    return tx(conn, TX_SAVEPOINT, 0, name);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_rollback_to(tapi_sql_conn *conn, const char *name)
{
    return tx(conn, TX_ROLLBACK_TO, 0, name);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_release(tapi_sql_conn *conn, const char *name)
{
    return tx(conn, TX_RELEASE, 0, name);
}

/* ---- schema introspection ----------------------------------------- */

/* Ask the agent and parse the answer into a plain grid of cells. */
static te_errno
meta(tapi_sql_conn *conn, int kind, const char *table, unsigned int width,
     tapi_sql_result *res)
{
    te_string rows = TE_STRING_INIT;
    te_errno  rc;

    rc = rpc_sql_meta(conn->rpcs, conn->handle, kind, table, &rows);
    if (rc != 0)
    {
        te_string_free(&rows);
        return conn_failed(conn, rc);
    }

    result_set_columns(res, width, NULL, NULL);
    if (rows.len > 0)
        rc = result_append(res, rows.ptr, 1);
    te_string_free(&rows);
    return rc;
}

/* A copy of a text cell, or NULL for SQL NULL. */
static char *
meta_str(const tapi_sql_result *res, unsigned int row, unsigned int col)
{
    const tapi_sql_value *v = tapi_sql_result_cell(res, row, col);

    if (v == NULL || v->type == TAPI_SQL_NULL || v->str == NULL)
        return NULL;
    return TE_STRDUP(v->str);
}

/* A copy of a text cell, "" for SQL NULL. */
static char *
meta_text(const tapi_sql_result *res, unsigned int row, unsigned int col)
{
    char *s = meta_str(res, row, col);

    return s != NULL ? s : TE_STRDUP("");
}

static bool
meta_flag(const tapi_sql_result *res, unsigned int row, unsigned int col)
{
    const tapi_sql_value *v = tapi_sql_result_cell(res, row, col);

    return v != NULL && v->str != NULL && strcmp(v->str, "1") == 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_tables(tapi_sql_conn *conn, tapi_sql_table **tables,
                unsigned int *n)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    tapi_sql_table *out;
    unsigned int    i;
    te_errno        rc;

    rc = meta(conn, META_TABLES, NULL, 2, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        return rc;
    }

    out = TE_ALLOC((res.nrows + 1) * sizeof(*out));
    for (i = 0; i < res.nrows; i++)
    {
        char *kind = meta_text(&res, i, 1);

        out[i].name = meta_text(&res, i, 0);
        out[i].is_view = strcmp(kind, "view") == 0;
        free(kind);
    }
    *tables = out;
    *n = res.nrows;
    tapi_sql_result_free(&res);
    return 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_columns(tapi_sql_conn *conn, const char *table,
                 tapi_sql_column **columns, unsigned int *n)
{
    tapi_sql_result  res = TAPI_SQL_RESULT_INIT;
    tapi_sql_column *out;
    unsigned int     i;
    te_errno         rc;

    rc = meta(conn, META_COLUMNS, table, 7, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        return rc;
    }

    out = TE_ALLOC((res.nrows + 1) * sizeof(*out));
    for (i = 0; i < res.nrows; i++)
    {
        char *pos = meta_text(&res, i, 1);
        char *letter = meta_text(&res, i, 2);

        out[i].name = meta_text(&res, i, 0);
        out[i].position = (unsigned int)strtoul(pos, NULL, 10);
        out[i].type = letter[0] != '\0' ? (tapi_sql_type)letter[0] :
                                          TAPI_SQL_TEXT;
        out[i].native_type = meta_text(&res, i, 3);
        out[i].nullable = meta_flag(&res, i, 4);
        out[i].default_value = meta_str(&res, i, 5);
        out[i].primary_key = meta_flag(&res, i, 6);
        free(pos);
        free(letter);
    }
    *columns = out;
    *n = res.nrows;
    tapi_sql_result_free(&res);
    return 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_indexes(tapi_sql_conn *conn, const char *table,
                 tapi_sql_index **indexes, unsigned int *n)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    tapi_sql_index *out;
    unsigned int    i;
    te_errno        rc;

    rc = meta(conn, META_INDEXES, table, 4, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        return rc;
    }

    out = TE_ALLOC((res.nrows + 1) * sizeof(*out));
    for (i = 0; i < res.nrows; i++)
    {
        out[i].name = meta_text(&res, i, 0);
        out[i].unique = meta_flag(&res, i, 1);
        out[i].primary = meta_flag(&res, i, 2);
        out[i].columns = meta_text(&res, i, 3);
    }
    *indexes = out;
    *n = res.nrows;
    tapi_sql_result_free(&res);
    return 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_constraints(tapi_sql_conn *conn, const char *table,
                     tapi_sql_constraint **constraints, unsigned int *n)
{
    tapi_sql_result      res = TAPI_SQL_RESULT_INIT;
    tapi_sql_constraint *out;
    unsigned int         i;
    te_errno             rc;

    rc = meta(conn, META_CONSTRAINTS, table, 3, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        return rc;
    }

    out = TE_ALLOC((res.nrows + 1) * sizeof(*out));
    for (i = 0; i < res.nrows; i++)
    {
        char *kind = meta_text(&res, i, 1);

        out[i].name = meta_text(&res, i, 0);
        out[i].kind = strcmp(kind, "primary") == 0 ?
                          TAPI_SQL_CONSTRAINT_PRIMARY :
                      strcmp(kind, "unique") == 0 ?
                          TAPI_SQL_CONSTRAINT_UNIQUE :
                      strcmp(kind, "foreign") == 0 ?
                          TAPI_SQL_CONSTRAINT_FOREIGN :
                          TAPI_SQL_CONSTRAINT_CHECK;
        out[i].columns = meta_text(&res, i, 2);
        free(kind);
    }
    *constraints = out;
    *n = res.nrows;
    tapi_sql_result_free(&res);
    return 0;
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_foreign_keys(tapi_sql_conn *conn, const char *table,
                      tapi_sql_foreign_key **keys, unsigned int *n)
{
    tapi_sql_result       res = TAPI_SQL_RESULT_INIT;
    tapi_sql_foreign_key *out;
    unsigned int          i;
    te_errno              rc;

    rc = meta(conn, META_FOREIGN_KEYS, table, 6, &res);
    if (rc != 0)
    {
        tapi_sql_result_free(&res);
        return rc;
    }

    out = TE_ALLOC((res.nrows + 1) * sizeof(*out));
    for (i = 0; i < res.nrows; i++)
    {
        out[i].name = meta_text(&res, i, 0);
        out[i].columns = meta_text(&res, i, 1);
        out[i].ref_table = meta_text(&res, i, 2);
        out[i].ref_columns = meta_text(&res, i, 3);
        out[i].on_delete = meta_text(&res, i, 4);
        out[i].on_update = meta_text(&res, i, 5);
    }
    *keys = out;
    *n = res.nrows;
    tapi_sql_result_free(&res);
    return 0;
}

/* See description in tapi_sql.h */
void
tapi_sql_tables_free(tapi_sql_table *tables, unsigned int n)
{
    unsigned int i;

    for (i = 0; tables != NULL && i < n; i++)
        free(tables[i].name);
    free(tables);
}

/* See description in tapi_sql.h */
void
tapi_sql_columns_free(tapi_sql_column *columns, unsigned int n)
{
    unsigned int i;

    for (i = 0; columns != NULL && i < n; i++)
    {
        free(columns[i].name);
        free(columns[i].native_type);
        free(columns[i].default_value);
    }
    free(columns);
}

/* See description in tapi_sql.h */
void
tapi_sql_indexes_free(tapi_sql_index *indexes, unsigned int n)
{
    unsigned int i;

    for (i = 0; indexes != NULL && i < n; i++)
    {
        free(indexes[i].name);
        free(indexes[i].columns);
    }
    free(indexes);
}

/* See description in tapi_sql.h */
void
tapi_sql_constraints_free(tapi_sql_constraint *constraints, unsigned int n)
{
    unsigned int i;

    for (i = 0; constraints != NULL && i < n; i++)
    {
        free(constraints[i].name);
        free(constraints[i].columns);
    }
    free(constraints);
}

/* See description in tapi_sql.h */
void
tapi_sql_foreign_keys_free(tapi_sql_foreign_key *keys, unsigned int n)
{
    unsigned int i;

    for (i = 0; keys != NULL && i < n; i++)
    {
        free(keys[i].name);
        free(keys[i].columns);
        free(keys[i].ref_table);
        free(keys[i].ref_columns);
        free(keys[i].on_delete);
        free(keys[i].on_update);
    }
    free(keys);
}

/* ---- deadlines, synchronisation points, cancellation -------------- */

/* See description in tapi_sql.h */
te_errno
tapi_sql_set_timeout(tapi_sql_conn *conn, tapi_sql_timeout kind,
                     unsigned int ms)
{
    return conn_failed(conn, rpc_sql_set_timeout(conn->rpcs, conn->handle,
                                                 (int)kind, (int)ms));
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_execute_start(tapi_sql_conn *conn, const char *sql)
{
    return rpc_sql_async_start(conn->rpcs, conn->handle, sql);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_execute_wait(tapi_sql_conn *conn, unsigned int timeout_ms,
                      bool *done, int64_t *affected)
{
    bool     finished = false;
    te_errno rc;

    /* The RPC itself must outlive the wait it carries. */
    conn->rpcs->timeout = timeout_ms + RCF_RPC_DEFAULT_TIMEOUT;
    rc = rpc_sql_async_wait(conn->rpcs, conn->handle, (int)timeout_ms,
                            &finished, affected, &conn->elapsed_us);
    if (done != NULL)
        *done = finished;
    if (!finished)
        return rc;
    return conn_failed(conn, rc);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_cancel(tapi_sql_conn *conn)
{
    return rpc_sql_cancel(conn->rpcs, conn->handle);
}

/* See description in tapi_sql.h */
te_errno
tapi_sql_stats(rcf_rpc_server *rpcs, unsigned int *conns, unsigned int *stmts,
               uint64_t *max_batch)
{
    int      c = 0;
    int      s = 0;
    te_errno rc;

    rc = rpc_sql_stats(rpcs, &c, &s, max_batch);
    if (rc == 0)
    {
        if (conns != NULL)
            *conns = (unsigned int)c;
        if (stmts != NULL)
            *stmts = (unsigned int)s;
    }
    return rc;
}
