/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: assertions on database state
 */

#define TE_LGR_USER     "TAPI SQL ASSERT"

#include "te_config.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "logger_api.h"
#include "tapi_test_log.h"

#include "tapi_sql.h"
#include "tapi_sql_assert.h"

static const tapi_sql_cmp_opts default_opts = TAPI_SQL_CMP_OPTS_DEFAULT;

/* ---- comparing ---------------------------------------------------- */

static bool
is_number(tapi_sql_type type)
{
    return type == TAPI_SQL_BOOL || type == TAPI_SQL_INT ||
           type == TAPI_SQL_UINT || type == TAPI_SQL_FLOAT ||
           type == TAPI_SQL_DECIMAL;
}

static bool
is_integer(tapi_sql_type type)
{
    return type == TAPI_SQL_BOOL || type == TAPI_SQL_INT ||
           type == TAPI_SQL_UINT;
}

/* The numeric reading of a value: its own, or a text that is a number. */
static bool
as_number(const tapi_sql_value *v, double *out)
{
    char *end = NULL;

    switch (v->type)
    {
        case TAPI_SQL_BOOL:
        case TAPI_SQL_INT:
            *out = (double)v->i;
            return true;
        case TAPI_SQL_UINT:
            *out = (double)v->u;
            return true;
        case TAPI_SQL_FLOAT:
            *out = v->f;
            return true;
        case TAPI_SQL_DECIMAL:
        case TAPI_SQL_TEXT:
            /* A decimal is its digits, whoever made the value. */
            break;
        default:
            return false;
    }
    if (v->str == NULL || v->len == 0)
        return false;

    *out = strtod(v->str, &end);
    return end != v->str && *end == '\0';
}

static bool
close_enough(double a, double b, const tapi_sql_cmp_opts *opts)
{
    double magnitude = fmax(fabs(a), fabs(b));
    double tolerance = fmax(opts->abs_tol, opts->rel_tol * magnitude);

    if (isnan(a) || isnan(b))
        return isnan(a) && isnan(b);
    if (a == b)
        return true;
    return fabs(a - b) <= tolerance;
}

/* See description in tapi_sql_assert.h */
bool
tapi_sql_value_equal(const tapi_sql_value *a, const tapi_sql_value *b,
                     const tapi_sql_cmp_opts *opts)
{
    double x = 0.0;
    double y = 0.0;

    if (opts == NULL)
        opts = &default_opts;

    if (a->type == TAPI_SQL_NULL || b->type == TAPI_SQL_NULL)
        return a->type == b->type && opts->null_equals_null;

    if (opts->strict_types && a->type != b->type)
        return false;

    /* Two integers are compared exactly, beyond a double's 53 bits. */
    if (is_integer(a->type) && is_integer(b->type) &&
        opts->abs_tol == 0.0 && opts->rel_tol == 0.0)
    {
        if (a->type == TAPI_SQL_UINT || b->type == TAPI_SQL_UINT)
        {
            if ((a->type != TAPI_SQL_UINT && a->i < 0) ||
                (b->type != TAPI_SQL_UINT && b->i < 0))
                return false;
            return a->u == b->u;
        }
        return a->i == b->i;
    }

    if ((is_number(a->type) || is_number(b->type)) &&
        as_number(a, &x) && as_number(b, &y))
        return close_enough(x, y, opts);

    if (is_number(a->type) != is_number(b->type))
        return false;

    /* Text, JSON, date/time, binary: the bytes. */
    if ((a->type == TAPI_SQL_BINARY) != (b->type == TAPI_SQL_BINARY))
        return false;
    return a->len == b->len &&
           (a->len == 0 || memcmp(a->str, b->str, a->len) == 0);
}

static bool
row_equal(const tapi_sql_result *a, unsigned int ra,
          const tapi_sql_result *b, unsigned int rb,
          const tapi_sql_cmp_opts *opts)
{
    unsigned int col;

    for (col = 0; col < a->ncols; col++)
    {
        if (!tapi_sql_value_equal(tapi_sql_result_cell(a, ra, col),
                                  tapi_sql_result_cell(b, rb, col), opts))
            return false;
    }
    return true;
}

static void
row_print(const tapi_sql_result *res, unsigned int row, te_string *dest)
{
    unsigned int col;

    te_string_append(dest, "(");
    for (col = 0; col < res->ncols; col++)
    {
        if (col > 0)
            te_string_append(dest, ", ");
        tapi_sql_value_print(tapi_sql_result_cell(res, row, col), dest);
    }
    te_string_append(dest, ")");
}

/* See description in tapi_sql_assert.h */
bool
tapi_sql_result_equal(const tapi_sql_result *got,
                      const tapi_sql_result *expected,
                      const tapi_sql_cmp_opts *opts, te_string *diag)
{
    te_string    scratch = TE_STRING_INIT;
    unsigned int max_diffs;
    unsigned int diffs = 0;
    unsigned int row;
    unsigned int col;
    bool         equal = true;

    if (opts == NULL)
        opts = &default_opts;
    if (diag == NULL)
        diag = &scratch;
    max_diffs = opts->max_diffs != 0 ? opts->max_diffs : 10;

    if (got->ncols != expected->ncols &&
        (got->nrows != 0 || expected->nrows != 0))
    {
        te_string_append(diag, "columns: expected %u, got %u\n",
                         expected->ncols, got->ncols);
        te_string_free(&scratch);
        return false;
    }
    if (got->nrows != expected->nrows)
    {
        te_string_append(diag, "rows: expected %u, got %u\n",
                         expected->nrows, got->nrows);
        equal = false;
    }

    if (opts->ordered)
    {
        unsigned int rows = got->nrows < expected->nrows ? got->nrows :
                                                           expected->nrows;

        for (row = 0; row < rows; row++)
        {
            for (col = 0; col < got->ncols; col++)
            {
                const tapi_sql_value *g = tapi_sql_result_cell(got, row,
                                                               col);
                const tapi_sql_value *e = tapi_sql_result_cell(expected,
                                                               row, col);

                if (tapi_sql_value_equal(g, e, opts))
                    continue;
                equal = false;
                if (diffs++ >= max_diffs)
                    continue;
                te_string_append(diag, "row %u column %u (%s): expected ",
                                 row, col,
                                 got->names != NULL ? got->names[col] : "");
                tapi_sql_value_print(e, diag);
                te_string_append(diag, ", got ");
                tapi_sql_value_print(g, diag);
                te_string_append(diag, "\n");
            }
        }
    }
    else
    {
        bool *used = TE_ALLOC((got->nrows + 1) * sizeof(*used));

        /* Pair every expected row off with a row not yet taken. */
        for (row = 0; row < expected->nrows; row++)
        {
            unsigned int cand;

            for (cand = 0; cand < got->nrows; cand++)
            {
                if (!used[cand] && row_equal(got, cand, expected, row, opts))
                {
                    used[cand] = true;
                    break;
                }
            }
            if (cand < got->nrows)
                continue;
            equal = false;
            if (diffs++ >= max_diffs)
                continue;
            te_string_append(diag, "missing row: ");
            row_print(expected, row, diag);
            te_string_append(diag, "\n");
        }
        for (row = 0; row < got->nrows; row++)
        {
            if (used[row])
                continue;
            equal = false;
            if (diffs++ >= max_diffs)
                continue;
            te_string_append(diag, "unexpected row: ");
            row_print(got, row, diag);
            te_string_append(diag, "\n");
        }
        free(used);
    }

    if (diffs > max_diffs)
        te_string_append(diag, "... and %u more difference(s)\n",
                         diffs - max_diffs);
    te_string_free(&scratch);
    return equal;
}

/* ---- failing ------------------------------------------------------ */

/* Log the details, emit the verdict, return the status of a failure. */
static te_errno
failed(const char *details, const char *verdict_fmt, ...)
    TE_LIKE_PRINTF(2, 3);

static te_errno
failed(const char *details, const char *verdict_fmt, ...)
{
    te_string verdict = TE_STRING_INIT;
    va_list   ap;

    va_start(ap, verdict_fmt);
    te_string_append_va(&verdict, verdict_fmt, ap);
    va_end(ap);

    ERROR("SQL assertion failed: %s\n%s", verdict.ptr,
          details != NULL ? details : "");
    ERROR_VERDICT("%s", verdict.ptr);
    te_string_free(&verdict);
    return TE_RC(TE_TAPI, TE_EFAIL);
}

/* Run a query for the rows that break a property; none must come. */
static te_errno
no_rows(tapi_sql_conn *conn, const char *sql, const char *verdict_fmt, ...)
    TE_LIKE_PRINTF(3, 4);

static te_errno
no_rows(tapi_sql_conn *conn, const char *sql, const char *verdict_fmt, ...)
{
    tapi_sql_result res = TAPI_SQL_RESULT_INIT;
    te_string       details = TE_STRING_INIT;
    te_string       verdict = TE_STRING_INIT;
    unsigned int    row;
    va_list         ap;
    te_errno        rc;

    rc = tapi_sql_query(conn, sql, &res);
    if (rc != 0)
    {
        ERROR("The check query failed: %s",
              tapi_sql_last_error(conn)->message);
        tapi_sql_result_free(&res);
        return rc;
    }
    if (res.nrows == 0)
    {
        tapi_sql_result_free(&res);
        return 0;
    }

    te_string_append(&details, "%u offending row(s):\n", res.nrows);
    for (row = 0; row < res.nrows && row < 10; row++)
    {
        row_print(&res, row, &details);
        te_string_append(&details, "\n");
    }
    va_start(ap, verdict_fmt);
    te_string_append_va(&verdict, verdict_fmt, ap);
    va_end(ap);

    rc = failed(details.ptr, "%s", verdict.ptr);
    te_string_free(&details);
    te_string_free(&verdict);
    tapi_sql_result_free(&res);
    return rc;
}

/* Split a comma-separated list (spaces trimmed) into owned items. */
static unsigned int
split(const char *list, char ***items)
{
    char        *copy = TE_STRDUP(list != NULL ? list : "");
    char        *save = NULL;
    char        *tok;
    char       **out = NULL;
    unsigned int n = 0;

    for (tok = strtok_r(copy, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
    {
        char *end;

        while (*tok == ' ')
            tok++;
        end = tok + strlen(tok);
        while (end > tok && end[-1] == ' ')
            *--end = '\0';
        if (*tok == '\0')
            continue;
        out = realloc(out, (n + 1) * sizeof(*out));
        if (out == NULL)
            TE_FATAL_ERROR("Out of memory");
        out[n++] = TE_STRDUP(tok);
    }
    free(copy);
    *items = out;
    return n;
}

static void
split_free(char **items, unsigned int n)
{
    unsigned int i;

    for (i = 0; i < n; i++)
        free(items[i]);
    free(items);
}

/* ---- the assertions ----------------------------------------------- */

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_result(const tapi_sql_result *got,
                       const tapi_sql_result *expected,
                       const tapi_sql_cmp_opts *opts, const char *what)
{
    te_string diag = TE_STRING_INIT;
    te_errno  rc = 0;

    if (!tapi_sql_result_equal(got, expected, opts, &diag))
        rc = failed(diag.ptr, "%s differs from what is expected",
                    what != NULL ? what : "the result");
    te_string_free(&diag);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_row_count(tapi_sql_conn *conn, const char *from,
                          int64_t expected)
{
    te_string sql = TE_STRING_INIT;
    te_string details = TE_STRING_INIT;
    int64_t   got = 0;
    te_errno  rc;

    te_string_append(&sql, "SELECT count(*) FROM %s", from);
    rc = tapi_sql_query_int(conn, sql.ptr, &got);
    te_string_free(&sql);
    if (rc != 0)
        return rc;

    if (got != expected)
    {
        te_string_append(&details, "expected %lld row(s), got %lld",
                         (long long)expected, (long long)got);
        rc = failed(details.ptr, "unexpected number of rows in %s", from);
        te_string_free(&details);
    }
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_pk_unique(tapi_sql_conn *conn, const char *table,
                          const char *columns)
{
    te_string sql = TE_STRING_INIT;
    te_errno  rc;

    te_string_append(&sql, "SELECT %s, count(*) FROM %s GROUP BY %s "
                     "HAVING count(*) > 1", columns, table, columns);
    rc = no_rows(conn, sql.ptr, "key (%s) of %s is not unique", columns,
                 table);
    te_string_free(&sql);
    if (rc != 0)
        return rc;

    return tapi_sql_assert_not_null(conn, table, columns);
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_not_null(tapi_sql_conn *conn, const char *table,
                         const char *columns)
{
    te_string    sql = TE_STRING_INIT;
    char       **cols = NULL;
    unsigned int n = split(columns, &cols);
    unsigned int i;
    te_errno     rc;

    if (n == 0)
    {
        split_free(cols, n);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    te_string_append(&sql, "SELECT * FROM %s WHERE ", table);
    for (i = 0; i < n; i++)
        te_string_append(&sql, "%s%s IS NULL", i > 0 ? " OR " : "", cols[i]);
    rc = no_rows(conn, sql.ptr, "NULL in (%s) of %s", columns, table);
    te_string_free(&sql);
    split_free(cols, n);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_referential(tapi_sql_conn *conn, const char *child,
                            const char *child_cols, const char *parent,
                            const char *parent_cols)
{
    te_string    sql = TE_STRING_INIT;
    char       **ccols = NULL;
    char       **pcols = NULL;
    unsigned int nc = split(child_cols, &ccols);
    unsigned int np = split(parent_cols, &pcols);
    unsigned int i;
    te_errno     rc;

    if (nc == 0 || nc != np)
    {
        split_free(ccols, nc);
        split_free(pcols, np);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    te_string_append(&sql, "SELECT * FROM %s tsf_c WHERE ", child);
    for (i = 0; i < nc; i++)
        te_string_append(&sql, "tsf_c.%s IS NOT NULL AND ", ccols[i]);
    te_string_append(&sql, "NOT EXISTS (SELECT 1 FROM %s tsf_p WHERE ",
                     parent);
    for (i = 0; i < nc; i++)
        te_string_append(&sql, "%stsf_p.%s = tsf_c.%s", i > 0 ? " AND " : "",
                         pcols[i], ccols[i]);
    te_string_append(&sql, ")");

    rc = no_rows(conn, sql.ptr, "%s (%s) refers to rows missing from %s",
                 child, child_cols, parent);
    te_string_free(&sql);
    split_free(ccols, nc);
    split_free(pcols, np);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_schema(tapi_sql_conn *conn, const char *table,
                       const tapi_sql_column_spec *expected, unsigned int n,
                       bool exact)
{
    tapi_sql_column *cols = NULL;
    te_string        details = TE_STRING_INIT;
    unsigned int     ncols = 0;
    unsigned int     i;
    unsigned int     j;
    te_errno         rc;

    rc = tapi_sql_columns(conn, table, &cols, &ncols);
    if (rc != 0)
        return rc;

    if (ncols == 0)
        te_string_append(&details, "table %s does not exist\n", table);

    for (i = 0; i < n && ncols > 0; i++)
    {
        const tapi_sql_column_spec *e = &expected[i];
        const tapi_sql_column      *c = NULL;

        for (j = 0; j < ncols; j++)
        {
            if (strcasecmp(cols[j].name, e->name) == 0)
                c = &cols[j];
        }
        if (c == NULL)
        {
            te_string_append(&details, "column %s: missing\n", e->name);
            continue;
        }
        if (e->type != TAPI_SQL_NULL && e->type != c->type)
            te_string_append(&details,
                             "column %s: expected type %c, got %c (%s)\n",
                             e->name, (char)e->type, (char)c->type,
                             c->native_type);
        if (e->nullable != TAPI_SQL_ANY &&
            (e->nullable == TAPI_SQL_YES) != c->nullable)
            te_string_append(&details, "column %s: expected %s, got %s\n",
                             e->name,
                             e->nullable == TAPI_SQL_YES ? "nullable" :
                                                           "NOT NULL",
                             c->nullable ? "nullable" : "NOT NULL");
        if (e->primary_key != TAPI_SQL_ANY &&
            (e->primary_key == TAPI_SQL_YES) != c->primary_key)
            te_string_append(&details,
                             "column %s: expected %s the primary key\n",
                             e->name,
                             e->primary_key == TAPI_SQL_YES ? "in" :
                                                              "outside");
    }

    for (j = 0; exact && j < ncols; j++)
    {
        bool known = false;

        for (i = 0; i < n; i++)
            known = known || strcasecmp(cols[j].name, expected[i].name) == 0;
        if (!known)
            te_string_append(&details, "column %s: not expected\n",
                             cols[j].name);
    }

    if (details.len > 0)
        rc = failed(details.ptr, "schema of %s is not compatible", table);
    te_string_free(&details);
    tapi_sql_columns_free(cols, ncols);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_query_equal(tapi_sql_conn *conn_a, const char *sql_a,
                            tapi_sql_conn *conn_b, const char *sql_b,
                            const tapi_sql_cmp_opts *opts)
{
    tapi_sql_result a = TAPI_SQL_RESULT_INIT;
    tapi_sql_result b = TAPI_SQL_RESULT_INIT;
    te_errno        rc;

    rc = tapi_sql_query(conn_a, sql_a, &a);
    if (rc == 0)
        rc = tapi_sql_query(conn_b, sql_b, &b);
    if (rc == 0)
        rc = tapi_sql_assert_result(&a, &b, opts, "the query result");
    tapi_sql_result_free(&a);
    tapi_sql_result_free(&b);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_number(tapi_sql_conn *conn, const char *sql, double expected,
                       double abs_tol, double rel_tol)
{
    tapi_sql_cmp_opts     opts = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result       res = TAPI_SQL_RESULT_INIT;
    tapi_sql_value        want = tapi_sql_float(expected);
    const tapi_sql_value *got;
    te_string             details = TE_STRING_INIT;
    te_errno              rc;

    opts.abs_tol = abs_tol;
    opts.rel_tol = rel_tol;

    rc = tapi_sql_query(conn, sql, &res);
    if (rc != 0)
        return rc;

    got = tapi_sql_result_cell(&res, 0, 0);
    if (got == NULL || !tapi_sql_value_equal(got, &want, &opts))
    {
        te_string_append(&details, "expected %.17g (+/- %g abs, %g rel), "
                         "got ", expected, abs_tol, rel_tol);
        tapi_sql_value_print(got, &details);
        rc = failed(details.ptr, "a number is outside its tolerance");
        te_string_free(&details);
    }
    tapi_sql_result_free(&res);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_eventually(tapi_sql_conn *conn, const char *sql,
                           int64_t expected, unsigned int timeout_ms,
                           unsigned int *waited_ms)
{
    te_string       details = TE_STRING_INIT;
    struct timespec start;
    struct timespec now;
    unsigned int    elapsed = 0;
    int64_t         got = 0;
    te_errno        rc;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        /* A query that fails (the table is not there yet) is one more
         * way of not having arrived. */
        rc = tapi_sql_query_int(conn, sql, &got);
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (unsigned int)((now.tv_sec - start.tv_sec) * 1000 +
                                 (now.tv_nsec - start.tv_nsec) / 1000000);
        if ((rc == 0 && got == expected) || elapsed >= timeout_ms)
            break;
        usleep(20000);
    }
    if (waited_ms != NULL)
        *waited_ms = elapsed;
    if (rc == 0 && got == expected)
        return 0;

    if (rc != 0)
        te_string_append(&details, "after %u ms the query still fails: %s",
                         elapsed, tapi_sql_last_error(conn)->message);
    else
        te_string_append(&details, "after %u ms: expected %lld, got %lld",
                         elapsed, (long long)expected, (long long)got);
    rc = failed(details.ptr, "an expected value never arrived");
    te_string_free(&details);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_atomic(tapi_sql_conn *conn, const char *const *stmts,
                       unsigned int n, bool must_fail, const char *probe)
{
    tapi_sql_cmp_opts opts = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result   before = TAPI_SQL_RESULT_INIT;
    tapi_sql_result   after = TAPI_SQL_RESULT_INIT;
    te_string         diag = TE_STRING_INIT;
    bool              any_failed = false;
    unsigned int      i;
    te_errno          rc;

    opts.strict_types = true;

    rc = tapi_sql_query(conn, probe, &before);
    if (rc == 0)
        rc = tapi_sql_begin(conn);
    if (rc != 0)
    {
        tapi_sql_result_free(&before);
        return rc;
    }

    for (i = 0; i < n && !any_failed; i++)
    {
        if (tapi_sql_execute(conn, stmts[i], NULL) != 0)
        {
            any_failed = true;
            RING("Statement %u of the transaction failed (%s): %s", i,
                 tapi_sql_err_class2str(tapi_sql_last_error(conn)->cls),
                 tapi_sql_last_error(conn)->message);
        }
    }

    rc = tapi_sql_rollback(conn);
    if (rc == 0)
        rc = tapi_sql_query(conn, probe, &after);
    if (rc != 0)
    {
        tapi_sql_result_free(&before);
        tapi_sql_result_free(&after);
        return rc;
    }

    if (must_fail && !any_failed)
        rc = failed("every statement succeeded",
                    "the failing transaction did not fail");
    else if (!tapi_sql_result_equal(&after, &before, &opts, &diag))
        rc = failed(diag.ptr, "a rolled back transaction left a trace");

    te_string_free(&diag);
    tapi_sql_result_free(&before);
    tapi_sql_result_free(&after);
    return rc;
}

/* See description in tapi_sql_assert.h */
te_errno
tapi_sql_assert_persistent(tapi_sql_conn *conn, const char *probe)
{
    tapi_sql_cmp_opts opts = TAPI_SQL_CMP_OPTS_DEFAULT;
    tapi_sql_result   before = TAPI_SQL_RESULT_INIT;
    tapi_sql_result   after = TAPI_SQL_RESULT_INIT;
    te_string         diag = TE_STRING_INIT;
    te_errno          rc;

    opts.strict_types = true;

    rc = tapi_sql_query(conn, probe, &before);
    if (rc == 0)
        rc = tapi_sql_reconnect(conn);
    if (rc == 0)
        rc = tapi_sql_query(conn, probe, &after);
    if (rc == 0 && !tapi_sql_result_equal(&after, &before, &opts, &diag))
        rc = failed(diag.ptr, "state changed across a reconnection");

    te_string_free(&diag);
    tapi_sql_result_free(&before);
    tapi_sql_result_free(&after);
    return rc;
}
