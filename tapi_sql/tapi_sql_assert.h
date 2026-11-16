/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: assertions on database state
 *
 * @defgroup tapi_sql_assert Database assertions (tapi_sql_assert)
 * @ingroup tapi_sql
 * @{
 *
 * Checks of what a database holds: row counts, key uniqueness,
 * referential integrity, NULLs, schema shape, equivalence of two
 * results, a number within a tolerance, atomicity of a transaction and
 * persistence across a reconnect.
 *
 * Every tapi_sql_assert_*() returns 0 when the property holds. When it
 * does not, the function logs what it found - which rows, which cells,
 * expected against actual, one line each - emits a TE verdict whose
 * text is stable (it names the property and the object, never a value
 * that changes between runs, so TRC can match it), and returns
 * TE_EFAIL. Any other status means the check itself could not be made.
 *
 * tapi_sql_result_equal() is the comparison underneath, without the
 * verdict, for a test that wants the answer rather than a failure.
 */

#ifndef __TAPI_SQL_ASSERT_H__
#define __TAPI_SQL_ASSERT_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"

#include "tapi_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How two results are compared. */
typedef struct tapi_sql_cmp_opts {
    /**
     * Rows are compared position by position. When @c false the two
     * results are compared as bags of rows: the same rows the same
     * number of times, in any order.
     */
    bool ordered;
    /**
     * Two NULLs are equal (what a test usually means). When @c false
     * NULL equals nothing, itself included, as in SQL.
     */
    bool null_equals_null;
    /**
     * Types must match as well as values. When @c false a number is a
     * number: integer 2, float 2.0 and decimal "2.00" are equal, which
     * is what comparing results of different engines needs.
     */
    bool strict_types;
    /** Absolute tolerance of a numeric comparison. */
    double abs_tol;
    /** Relative tolerance of a numeric comparison. */
    double rel_tol;
    /** Differences described in the diagnostics; 0 = 10. */
    unsigned int max_diffs;
} tapi_sql_cmp_opts;

/** Ordered, NULL equals NULL, numbers by value, no tolerance. */
#define TAPI_SQL_CMP_OPTS_DEFAULT \
    { .ordered = true, .null_equals_null = true, .strict_types = false, \
      .abs_tol = 0.0, .rel_tol = 0.0, .max_diffs = 10 }

/**
 * Compare two values.
 *
 * @param a     A value.
 * @param b     Another.
 * @param opts  How to compare (@c NULL = the defaults).
 *
 * @return @c true when they are equal under @p opts.
 */
extern bool tapi_sql_value_equal(const tapi_sql_value *a,
                                 const tapi_sql_value *b,
                                 const tapi_sql_cmp_opts *opts);

/**
 * Compare two results.
 *
 * @param[in]  got       The result under test.
 * @param[in]  expected  What it should be.
 * @param[in]  opts      How to compare (@c NULL = the defaults).
 * @param[out] diag      One line per difference is appended here (may
 *                       be @c NULL).
 *
 * @return @c true when the results are equal under @p opts.
 *
 * @note The unordered comparison pairs rows off one by one; it is meant
 *       for results of test size, not for millions of rows.
 */
extern bool tapi_sql_result_equal(const tapi_sql_result *got,
                                  const tapi_sql_result *expected,
                                  const tapi_sql_cmp_opts *opts,
                                  te_string *diag);

/**
 * Assert two results are equal; @p what names the result in the
 * verdict.
 */
extern te_errno tapi_sql_assert_result(const tapi_sql_result *got,
                                       const tapi_sql_result *expected,
                                       const tapi_sql_cmp_opts *opts,
                                       const char *what);

/**
 * Assert the number of rows of a table (or of a table with a
 * condition).
 *
 * @param conn      Connection.
 * @param from      What follows FROM: a table name, optionally with a
 *                  WHERE clause (@c "orders WHERE paid = 1").
 * @param expected  The count.
 */
extern te_errno tapi_sql_assert_row_count(tapi_sql_conn *conn,
                                          const char *from,
                                          int64_t expected);

/**
 * Assert that a key is unique and complete: no two rows share the
 * values of @p columns and none has a NULL in them.
 *
 * @param conn     Connection.
 * @param table    Table.
 * @param columns  Key columns, comma-separated.
 */
extern te_errno tapi_sql_assert_pk_unique(tapi_sql_conn *conn,
                                          const char *table,
                                          const char *columns);

/**
 * Assert referential integrity: every non-NULL reference in the child
 * table has its row in the parent table. This looks at the data, so it
 * also finds what a missing or disabled constraint let in.
 *
 * @param conn         Connection.
 * @param child        Referencing table.
 * @param child_cols   Referencing columns, comma-separated.
 * @param parent       Referenced table.
 * @param parent_cols  Referenced columns, in the same order.
 */
extern te_errno tapi_sql_assert_referential(tapi_sql_conn *conn,
                                            const char *child,
                                            const char *child_cols,
                                            const char *parent,
                                            const char *parent_cols);

/**
 * Assert that no row has a NULL in any of the columns.
 *
 * @param conn     Connection.
 * @param table    Table.
 * @param columns  Columns, comma-separated.
 */
extern te_errno tapi_sql_assert_not_null(tapi_sql_conn *conn,
                                         const char *table,
                                         const char *columns);

/** Yes, no, or do not care - for a column property. */
typedef enum tapi_sql_tristate {
    TAPI_SQL_ANY = 0,
    TAPI_SQL_YES,
    TAPI_SQL_NO,
} tapi_sql_tristate;

/** What a column of a table is expected to be. */
typedef struct tapi_sql_column_spec {
    const char       *name;         /**< Column name. */
    tapi_sql_type     type;         /**< Normalised type; TAPI_SQL_NULL
                                         accepts any. */
    tapi_sql_tristate nullable;     /**< NULL allowed? */
    tapi_sql_tristate primary_key;  /**< Part of the primary key? */
} tapi_sql_column_spec;

/**
 * Assert schema compatibility: the table has every expected column,
 * with the expected normalised type, nullability and key membership.
 * Columns the table has beyond the expected ones are fine unless
 * @p exact is set.
 *
 * @param conn      Connection.
 * @param table     Table.
 * @param expected  Expected columns.
 * @param n         How many.
 * @param exact     The table must have no other column.
 */
extern te_errno tapi_sql_assert_schema(tapi_sql_conn *conn,
                                       const char *table,
                                       const tapi_sql_column_spec *expected,
                                       unsigned int n, bool exact);

/**
 * Assert query result equivalence: two queries, possibly on two
 * connections (two engines, two replicas, before and after a
 * migration), return the same rows.
 *
 * @param conn_a  Connection of the query under test.
 * @param sql_a   The query under test.
 * @param conn_b  Connection of the reference query.
 * @param sql_b   The reference query.
 * @param opts    How to compare (@c NULL = the defaults).
 */
extern te_errno tapi_sql_assert_query_equal(tapi_sql_conn *conn_a,
                                            const char *sql_a,
                                            tapi_sql_conn *conn_b,
                                            const char *sql_b,
                                            const tapi_sql_cmp_opts *opts);

/**
 * Assert a number within a tolerance: the query's first cell is
 * @p expected give or take max(@p abs_tol, @p rel_tol x magnitude).
 *
 * @param conn      Connection.
 * @param sql       A query yielding one number.
 * @param expected  The expected value.
 * @param abs_tol   Absolute tolerance.
 * @param rel_tol   Relative tolerance.
 */
extern te_errno tapi_sql_assert_number(tapi_sql_conn *conn, const char *sql,
                                       double expected, double abs_tol,
                                       double rel_tol);

/**
 * Assert transaction atomicity: a transaction that does not commit
 * leaves no trace. The probe query is run, a transaction is begun, the
 * statements are executed until one fails (the last is normally the
 * one meant to fail), the transaction is rolled back and the probe is
 * run again; the two probe results must be equal.
 *
 * @param conn          Connection (no transaction open).
 * @param stmts         Statements of the transaction.
 * @param n             How many.
 * @param must_fail     One of the statements has to fail; if all
 *                      succeed the scenario did not happen, which is a
 *                      failure of its own.
 * @param probe         A query that shows the state the transaction
 *                      touches, with a stable order.
 */
extern te_errno tapi_sql_assert_atomic(tapi_sql_conn *conn,
                                       const char *const *stmts,
                                       unsigned int n, bool must_fail,
                                       const char *probe);

/**
 * Assert persistent state after reconnection: the probe query returns
 * the same rows before and after the session is closed and opened
 * again. What was committed is still there; what belonged to the
 * session is not what the probe should look at.
 *
 * @param conn   Connection; it is reconnected.
 * @param probe  A query with a stable order.
 */
extern te_errno tapi_sql_assert_persistent(tapi_sql_conn *conn,
                                           const char *probe);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_ASSERT_H__ */

/** @} */
