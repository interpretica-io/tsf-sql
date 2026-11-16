/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Contract test of ta_sql against real databases
 *
 * Runs the same checks over every engine it is given, through the
 * public ta_sql API (handles, value cells, bounded fetch, transactions,
 * introspection, error classes, deadlines, asynchronous statements) -
 * what the RPC layer calls, without the RPC layer. Built against the
 * stand-in TE headers of ci/stubs, with the real client libraries.
 *
 * Engines come from the environment:
 *
 *   TSF_SQL_SQLITE    a database file path (created, then removed)
 *   TSF_SQL_PG        "host port user password database"
 *   TSF_SQL_MY        "host port user password database"
 *
 * An engine whose variable is unset is skipped; with TSF_SQL_REQUIRE_ALL
 * set, that is a failure.
 */

#include "te_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"

#include "ta_sql.h"

static int         failures;
static const char *engine_name = "";

#define CHECK(_cond, ...) \
    do {                                                            \
        if (!(_cond))                                               \
        {                                                           \
            failures++;                                             \
            fprintf(stderr, "FAIL [%s] %s:%d: ", engine_name,       \
                    __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                           \
            fprintf(stderr, "\n");                                  \
        }                                                           \
    } while (0)

/* The class of the last error on a connection. */
static int
err_cls(int h)
{
    ta_sql_errinfo err;

    memset(&err, 0, sizeof(err));
    ta_sql_error(h, &err);
    return err.cls;
}

static const char *
err_msg(int h)
{
    static ta_sql_errinfo err;

    memset(&err, 0, sizeof(err));
    ta_sql_error(h, &err);
    return err.message;
}

/* Run a statement that must succeed. */
static void
must(int h, const char *sql)
{
    te_errno rc = ta_sql_execute(h, sql, NULL, NULL);

    CHECK(rc == 0, "'%s' failed: rc=%d %s", sql, rc, err_msg(h));
}

/* Run a query and return its whole result as rows text (static). */
static const char *
rows_of(int h, const char *sql)
{
    static te_string out = TE_STRING_INIT;
    int              st = -1;
    int              ncols = 0;
    int              n = 0;
    int              eof = 0;
    te_errno         rc;

    te_string_reset(&out);
    te_string_append(&out, "%s", "");

    rc = ta_sql_prepare(h, sql, &st, NULL);
    CHECK(rc == 0, "prepare '%s': rc=%d %s", sql, rc, err_msg(h));
    if (rc != 0)
        return out.ptr;
    rc = ta_sql_stmt_execute(st, &ncols, NULL, NULL, NULL);
    CHECK(rc == 0, "execute '%s': rc=%d %s", sql, rc, err_msg(h));
    while (rc == 0 && !eof)
    {
        if (out.len > 0)
            te_string_append(&out, "\n");
        rc = ta_sql_fetch(st, 0, 0, &out, &n, &eof);
        CHECK(rc == 0, "fetch '%s': rc=%d %s", sql, rc, err_msg(h));
    }
    ta_sql_stmt_free(st);
    return out.ptr;
}

/* Prepare, bind the given cells, execute; returns the status. */
static te_errno
run_params(int h, const char *sql, const char *const *cells, int n)
{
    int      st = -1;
    te_errno rc;
    int      i;

    rc = ta_sql_prepare(h, sql, &st, NULL);
    if (rc != 0)
        return rc;
    for (i = 0; i < n && rc == 0; i++)
        rc = ta_sql_bind(st, i + 1, cells[i]);
    if (rc == 0)
        rc = ta_sql_stmt_execute(st, NULL, NULL, NULL, NULL);
    ta_sql_stmt_free(st);
    return rc;
}

/* ---- the checks --------------------------------------------------- */

static void
check_values(int h, const ta_sql_config *cfg)
{
    static const char *const row[] = {
        "I1", "Thello\\tworld\\n\\\\", "F2.5", "D12.50",
        "X00ff10206e756c00", "M2024-02-29", "B1",
    };
    static const char *const nulls[] = {
        "I2", "Tsecond", "N", "N", "N", "N", "N",
    };
    const char *got;
    te_errno    rc;

    must(h, "DROP TABLE IF EXISTS child");
    must(h, "DROP TABLE IF EXISTS parent");
    must(h, cfg->engine == TA_SQL_ENGINE_POSTGRES ?
         "CREATE TABLE parent (id INTEGER PRIMARY KEY, "
         "name VARCHAR(40) NOT NULL UNIQUE, score DOUBLE PRECISION, "
         "amount DECIMAL(10,2), data BYTEA, born DATE, flag BOOLEAN)" :
         "CREATE TABLE parent (id INTEGER PRIMARY KEY, "
         "name VARCHAR(40) NOT NULL UNIQUE, score DOUBLE PRECISION, "
         "amount DECIMAL(10,2), data BLOB, born DATE, flag BOOLEAN)");
    must(h, "CREATE TABLE child (id INTEGER PRIMARY KEY, "
            "parent_id INTEGER NOT NULL, note VARCHAR(40), "
            "CONSTRAINT child_parent FOREIGN KEY (parent_id) "
            "REFERENCES parent (id) ON DELETE CASCADE)");
    must(h, "CREATE INDEX child_note ON child (note)");

    rc = run_params(h, "INSERT INTO parent VALUES (?, ?, ?, ?, ?, ?, ?)",
                    row, 7);
    CHECK(rc == 0, "typed insert: rc=%d %s", rc, err_msg(h));
    rc = run_params(h, "INSERT INTO parent VALUES (?, ?, ?, ?, ?, ?, ?)",
                    nulls, 7);
    CHECK(rc == 0, "null insert: rc=%d %s", rc, err_msg(h));

    got = rows_of(h, "SELECT id, name, data, born, flag FROM parent "
                     "ORDER BY id");
    CHECK(strcmp(got, "I1\tThello\\tworld\\n\\\\\tX00ff10206e756c00\t"
                      "M2024-02-29\tB1\n"
                      "I2\tTsecond\tN\tN\tN") == 0,
          "values read back wrong:\n%s", got);

    got = rows_of(h, "SELECT score FROM parent WHERE id = 1");
    CHECK(strcmp(got, "F2.5") == 0, "double read back as '%s'", got);

    got = rows_of(h, "SELECT amount FROM parent WHERE id = 1");
    CHECK(got[0] != '\0' && strtod(got + 1, NULL) == 12.5,
          "decimal read back as '%s'", got);
    if (cfg->engine != TA_SQL_ENGINE_SQLITE)
        CHECK(strcmp(got, "D12.50") == 0, "decimal lost its scale: '%s'",
              got);

    /* A parameter in a WHERE clause, and a text with a quote in it. */
    {
        static const char *const quote[] = { "I3", "To'brien; DROP" };
        static const char *const key[] = { "To'brien; DROP" };
        int                      st = -1;
        te_string                out = TE_STRING_INIT;
        int                      n = 0;
        int                      eof = 0;

        rc = run_params(h, "INSERT INTO parent (id, name) VALUES (?, ?)",
                        quote, 2);
        CHECK(rc == 0, "quote insert: rc=%d %s", rc, err_msg(h));

        rc = ta_sql_prepare(h, "SELECT id FROM parent WHERE name = ?", &st,
                            &n);
        CHECK(rc == 0 && n == 1, "prepare: rc=%d nparams=%d", rc, n);
        rc = ta_sql_bind(st, 1, key[0]);
        CHECK(rc == 0, "bind: rc=%d", rc);
        rc = ta_sql_stmt_execute(st, NULL, NULL, NULL, NULL);
        CHECK(rc == 0, "execute: rc=%d %s", rc, err_msg(h));
        rc = ta_sql_fetch(st, 0, 0, &out, &n, &eof);
        CHECK(rc == 0 && n == 1 && out.ptr != NULL &&
              strcmp(out.ptr, "I3") == 0,
              "parameterised lookup: rc=%d n=%d '%s'", rc, n,
              out.ptr != NULL ? out.ptr : "");
        te_string_free(&out);
        ta_sql_stmt_free(st);
    }
}

static void
check_fetch(int h)
{
    te_string batch = TE_STRING_INIT;
    int       st = -1;
    int       ncols = 0;
    int       total = 0;
    int       batches = 0;
    int       n = 0;
    int       eof = 0;
    te_errno  rc;

    must(h, "DROP TABLE IF EXISTS big");
    must(h, "CREATE TABLE big (n INTEGER PRIMARY KEY, pad VARCHAR(40))");
    /* Ten digits, crossed, make any number of rows on every engine. */
    must(h, "DROP TABLE IF EXISTS digits");
    must(h, "CREATE TABLE digits (d INTEGER PRIMARY KEY)");
    must(h, "INSERT INTO digits VALUES (0), (1), (2), (3), (4), (5), (6), "
            "(7), (8), (9)");
    must(h, "INSERT INTO big (n, pad) "
            "SELECT a.d + 10 * b.d + 100 * c.d + 1000 * e.d + 1, "
            "'padding-padding-padding' "
            "FROM digits a, digits b, digits c, digits e WHERE e.d < 5");

    rc = ta_sql_prepare(h, "SELECT n, pad FROM big ORDER BY n", &st, NULL);
    CHECK(rc == 0, "prepare: rc=%d %s", rc, err_msg(h));
    rc = ta_sql_stmt_execute(st, &ncols, NULL, NULL, NULL);
    CHECK(rc == 0 && ncols == 2, "execute: rc=%d ncols=%d", rc, ncols);

    while (rc == 0 && !eof)
    {
        te_string_reset(&batch);
        rc = ta_sql_fetch(st, 100, 0, &batch, &n, &eof);
        CHECK(rc == 0 && n <= 100, "fetch: rc=%d n=%d", rc, n);
        total += n;
        batches += n > 0 ? 1 : 0;
    }
    CHECK(total == 5000 && batches == 50, "5000 rows in 50 batches, got "
          "%d rows in %d batches", total, batches);

    /* The byte bound: 64 bytes hold two ~27-byte rows, not three. */
    rc = ta_sql_stmt_execute(st, &ncols, NULL, NULL, NULL);
    CHECK(rc == 0, "re-execute: rc=%d %s", rc, err_msg(h));
    te_string_reset(&batch);
    rc = ta_sql_fetch(st, 100, 64, &batch, &n, &eof);
    CHECK(rc == 0 && n == 2 && batch.len <= 64,
          "byte-bounded fetch: rc=%d n=%d len=%zu", rc, n, batch.len);

    /* A command while the result is still open must not break either. */
    must(h, "UPDATE big SET pad = 'x' WHERE n = 1");

    te_string_free(&batch);
    ta_sql_stmt_free(st);
}

static void
check_tx(int h)
{
    const char *got;
    te_errno    rc;

    rc = ta_sql_tx(h, TA_SQL_TX_BEGIN, 0, NULL);
    CHECK(rc == 0, "begin: rc=%d %s", rc, err_msg(h));
    must(h, "INSERT INTO child VALUES (1, 1, 'rolled back')");
    rc = ta_sql_tx(h, TA_SQL_TX_ROLLBACK, 0, NULL);
    CHECK(rc == 0, "rollback: rc=%d", rc);
    got = rows_of(h, "SELECT count(*) FROM child");
    CHECK(strcmp(got, "I0") == 0, "rollback left '%s' rows", got);

    rc = ta_sql_tx(h, TA_SQL_TX_BEGIN, 0, NULL);
    CHECK(rc == 0, "begin: rc=%d", rc);
    must(h, "INSERT INTO child VALUES (1, 1, 'kept')");
    rc = ta_sql_tx(h, TA_SQL_TX_SAVEPOINT, 0, "sp1");
    CHECK(rc == 0, "savepoint: rc=%d %s", rc, err_msg(h));
    must(h, "INSERT INTO child VALUES (2, 1, 'dropped')");
    rc = ta_sql_tx(h, TA_SQL_TX_ROLLBACK_TO, 0, "sp1");
    CHECK(rc == 0, "rollback to: rc=%d %s", rc, err_msg(h));
    rc = ta_sql_tx(h, TA_SQL_TX_COMMIT, 0, NULL);
    CHECK(rc == 0, "commit: rc=%d", rc);
    got = rows_of(h, "SELECT note FROM child ORDER BY id");
    CHECK(strcmp(got, "Tkept") == 0, "savepoint rollback left '%s'", got);

    rc = ta_sql_tx(h, TA_SQL_TX_SAVEPOINT, 0, "bad name; DROP");
    CHECK(rc == TE_EINVAL, "a savepoint name is not validated: rc=%d", rc);
}

static void
check_errors(int h)
{
    te_errno rc;

    rc = ta_sql_execute(h, "SELEKT 1", NULL, NULL);
    CHECK(rc != 0 && err_cls(h) == TA_SQL_ERR_SYNTAX,
          "syntax error: rc=%d cls=%d %s", rc, err_cls(h), err_msg(h));

    rc = ta_sql_execute(h, "SELECT * FROM no_such_table", NULL, NULL);
    CHECK(rc != 0 && err_cls(h) == TA_SQL_ERR_NOT_FOUND,
          "missing table: rc=%d cls=%d %s", rc, err_cls(h), err_msg(h));

    rc = ta_sql_execute(h, "INSERT INTO parent (id, name) VALUES (1, 'dup')",
                        NULL, NULL);
    CHECK(rc != 0 && err_cls(h) == TA_SQL_ERR_CONSTRAINT,
          "duplicate key: rc=%d cls=%d %s", rc, err_cls(h), err_msg(h));

    rc = ta_sql_execute(h, "INSERT INTO child VALUES (9, 999, 'orphan')",
                        NULL, NULL);
    CHECK(rc != 0 && err_cls(h) == TA_SQL_ERR_CONSTRAINT,
          "foreign key: rc=%d cls=%d %s", rc, err_cls(h), err_msg(h));

    rc = ta_sql_execute(h, "INSERT INTO parent (id, name) VALUES (7, NULL)",
                        NULL, NULL);
    CHECK(rc != 0 && err_cls(h) == TA_SQL_ERR_CONSTRAINT,
          "not null: rc=%d cls=%d %s", rc, err_cls(h), err_msg(h));

    /* The connection is still usable after each of these. */
    CHECK(ta_sql_ping(h, NULL) == 0, "ping after errors failed");
}

static void
check_meta(int h)
{
    te_string out = TE_STRING_INIT;
    te_errno  rc;

    rc = ta_sql_meta(h, TA_SQL_META_TABLES, NULL, &out);
    CHECK(rc == 0 && strstr(out.ptr, "Tchild\tTtable") != NULL &&
          strstr(out.ptr, "Tparent\tTtable") != NULL,
          "tables: rc=%d\n%s", rc, out.ptr != NULL ? out.ptr : "");

    te_string_reset(&out);
    rc = ta_sql_meta(h, TA_SQL_META_COLUMNS, "parent", &out);
    CHECK(rc == 0 && out.ptr != NULL &&
          strncmp(out.ptr, "Tid\tT1\tTI\t", 10) == 0 &&
          strstr(out.ptr, "\nTname\tT2\tTT\t") != NULL &&
          strstr(out.ptr, "\nTscore\tT3\tTF\t") != NULL &&
          strstr(out.ptr, "\nTamount\tT4\tTD\t") != NULL &&
          strstr(out.ptr, "\nTdata\tT5\tTX\t") != NULL &&
          strstr(out.ptr, "\nTborn\tT6\tTM\t") != NULL &&
          strstr(out.ptr, "\nTflag\tT7\tTB\t") != NULL,
          "columns: rc=%d %s\n%s", rc, err_msg(h),
          out.ptr != NULL ? out.ptr : "");

    te_string_reset(&out);
    rc = ta_sql_meta(h, TA_SQL_META_INDEXES, "child", &out);
    CHECK(rc == 0 && out.ptr != NULL &&
          strstr(out.ptr, "Tchild_note\tT0\tT0\tTnote") != NULL,
          "indexes: rc=%d %s\n%s", rc, err_msg(h),
          out.ptr != NULL ? out.ptr : "");

    te_string_reset(&out);
    rc = ta_sql_meta(h, TA_SQL_META_CONSTRAINTS, "child", &out);
    CHECK(rc == 0 && out.ptr != NULL &&
          strstr(out.ptr, "\tTprimary\tTid") != NULL &&
          strstr(out.ptr, "\tTforeign\tTparent_id") != NULL,
          "constraints: rc=%d %s\n%s", rc, err_msg(h),
          out.ptr != NULL ? out.ptr : "");

    te_string_reset(&out);
    rc = ta_sql_meta(h, TA_SQL_META_FOREIGN_KEYS, "child", &out);
    CHECK(rc == 0 && out.ptr != NULL &&
          strstr(out.ptr, "\tTparent_id\tTparent\tTid\tTcascade\t") != NULL,
          "foreign keys: rc=%d %s\n%s", rc, err_msg(h),
          out.ptr != NULL ? out.ptr : "");

    te_string_free(&out);
}

/* A thousand million rows to count: slow on every engine. */
#define SLOW_QUERY \
    "SELECT count(*) FROM digits a, digits b, digits c, digits e, "    \
    "digits f, digits g, digits h, digits i, digits j"

static void
check_deadlines(int h, unsigned int caps)
{
    uint64_t us = 0;
    te_errno rc;
    int      done = 0;

    rc = ta_sql_set_timeout(h, TA_SQL_TIMEOUT_STATEMENT, 300);
    CHECK(rc == 0, "set statement timeout: rc=%d %s", rc, err_msg(h));
    rc = ta_sql_execute(h, SLOW_QUERY, NULL, &us);
    CHECK(rc == TE_ETIMEDOUT && err_cls(h) == TA_SQL_ERR_TIMEOUT,
          "statement deadline: rc=%d cls=%d %s", rc, err_cls(h),
          err_msg(h));
    CHECK(us < 5000000, "the deadline took %llu us",
          (unsigned long long)us);
    rc = ta_sql_set_timeout(h, TA_SQL_TIMEOUT_STATEMENT, 0);
    CHECK(rc == 0, "clear statement timeout: rc=%d", rc);
    CHECK(ta_sql_ping(h, NULL) == 0, "ping after a deadline failed");

    if ((caps & TA_SQL_CAP_CANCEL) == 0)
        return;

    rc = ta_sql_async_start(h, SLOW_QUERY);
    CHECK(rc == 0, "async start: rc=%d", rc);
    rc = ta_sql_async_wait(h, 300, &done, NULL, NULL);
    CHECK(rc == TE_EINPROGRESS && !done, "slow query finished early: rc=%d",
          rc);
    rc = ta_sql_execute(h, "SELECT 1", NULL, NULL);
    CHECK(rc == TE_EBUSY, "a busy connection took a statement: rc=%d", rc);
    rc = ta_sql_cancel(h);
    CHECK(rc == 0, "cancel: rc=%d", rc);
    rc = ta_sql_async_wait(h, 10000, &done, NULL, NULL);
    CHECK(done && rc == TE_ECANCELED && err_cls(h) == TA_SQL_ERR_CANCELLED,
          "cancelled statement: done=%d rc=%d cls=%d %s", done, rc,
          err_cls(h), err_msg(h));
    CHECK(ta_sql_ping(h, NULL) == 0, "ping after a cancel failed");
}

static void
check_locks(const ta_sql_config *cfg, int h1, unsigned int caps)
{
    ta_sql_errinfo err;
    int            h2 = -1;
    int            done = 0;
    int64_t        affected = 0;
    te_errno       rc;
    te_errno       rc2;

    rc = ta_sql_connect(cfg, &h2, NULL, &err);
    CHECK(rc == 0, "second session: rc=%d %s", rc, err.message);
    if (rc != 0)
        return;

    must(h1, "DELETE FROM big WHERE n > 2");

    if ((caps & TA_SQL_CAP_ROW_LOCKS) == 0)
    {
        /* One writer at a time: the second gets "busy". */
        ta_sql_tx(h1, TA_SQL_TX_BEGIN, 0, NULL);
        must(h1, "UPDATE big SET pad = 'one' WHERE n = 1");
        rc = ta_sql_execute(h2, "UPDATE big SET pad = 'two' WHERE n = 2",
                            NULL, NULL);
        CHECK(rc == TE_ETIMEDOUT && err_cls(h2) == TA_SQL_ERR_LOCK_TIMEOUT,
              "second writer: rc=%d cls=%d %s", rc, err_cls(h2),
              err_msg(h2));
        ta_sql_tx(h1, TA_SQL_TX_COMMIT, 0, NULL);
        ta_sql_disconnect(h2);
        return;
    }

    /* A blocked writer proceeds when the lock holder commits. */
    ta_sql_tx(h1, TA_SQL_TX_BEGIN, 0, NULL);
    must(h1, "UPDATE big SET pad = 'one' WHERE n = 1");
    rc = ta_sql_async_start(h2, "UPDATE big SET pad = 'two' WHERE n = 1");
    CHECK(rc == 0, "async start: rc=%d", rc);
    rc = ta_sql_async_wait(h2, 500, &done, NULL, NULL);
    CHECK(rc == TE_EINPROGRESS && !done,
          "the second writer did not wait: rc=%d", rc);
    ta_sql_tx(h1, TA_SQL_TX_COMMIT, 0, NULL);
    rc = ta_sql_async_wait(h2, 10000, &done, &affected, NULL);
    CHECK(rc == 0 && done && affected == 1,
          "released writer: rc=%d done=%d affected=%lld %s", rc, done,
          (long long)affected, err_msg(h2));

    /* A lock-wait deadline. */
    rc = ta_sql_set_timeout(h2, TA_SQL_TIMEOUT_LOCK, 1000);
    CHECK(rc == 0, "set lock timeout: rc=%d %s", rc, err_msg(h2));
    ta_sql_tx(h1, TA_SQL_TX_BEGIN, 0, NULL);
    must(h1, "UPDATE big SET pad = 'one' WHERE n = 1");
    rc = ta_sql_execute(h2, "UPDATE big SET pad = 'two' WHERE n = 1", NULL,
                        NULL);
    CHECK(rc == TE_ETIMEDOUT && err_cls(h2) == TA_SQL_ERR_LOCK_TIMEOUT,
          "lock deadline: rc=%d cls=%d %s", rc, err_cls(h2), err_msg(h2));
    ta_sql_tx(h1, TA_SQL_TX_ROLLBACK, 0, NULL);
    ta_sql_set_timeout(h2, TA_SQL_TIMEOUT_LOCK, 0);

    /* A deadlock: each session holds a row the other one wants. */
    ta_sql_tx(h1, TA_SQL_TX_BEGIN, 0, NULL);
    ta_sql_tx(h2, TA_SQL_TX_BEGIN, 0, NULL);
    must(h1, "UPDATE big SET pad = 'a' WHERE n = 1");
    must(h2, "UPDATE big SET pad = 'b' WHERE n = 2");
    rc = ta_sql_async_start(h2, "UPDATE big SET pad = 'b' WHERE n = 1");
    CHECK(rc == 0, "async start: rc=%d", rc);
    ta_sql_async_wait(h2, 300, &done, NULL, NULL);
    rc = ta_sql_execute(h1, "UPDATE big SET pad = 'a' WHERE n = 2", NULL,
                        NULL);
    rc2 = ta_sql_async_wait(h2, 10000, &done, NULL, NULL);
    CHECK(done, "the deadlocked statement never returned");
    CHECK((rc == TE_EDEADLK) != (rc2 == TE_EDEADLK),
          "exactly one deadlock victim expected: rc=%d rc2=%d", rc, rc2);
    ta_sql_tx(h1, TA_SQL_TX_ROLLBACK, 0, NULL);
    ta_sql_tx(h2, TA_SQL_TX_ROLLBACK, 0, NULL);

    /* A lost connection: the first session kills the second. */
    {
        const char *id;
        char        sql[96];

        id = rows_of(h2, cfg->engine == TA_SQL_ENGINE_POSTGRES ?
                         "SELECT pg_backend_pid()" :
                         "SELECT CONNECTION_ID()");
        snprintf(sql, sizeof(sql),
                 cfg->engine == TA_SQL_ENGINE_POSTGRES ?
                 "SELECT pg_terminate_backend(%s)" : "KILL %s", id + 1);
        must(h1, sql);
        usleep(200000);
        rc = ta_sql_execute(h2, "SELECT 1", NULL, NULL);
        CHECK(rc == TE_ECONNRESET && err_cls(h2) == TA_SQL_ERR_CONN_LOST,
              "killed session: rc=%d cls=%d %s", rc, err_cls(h2),
              err_msg(h2));
        rc = ta_sql_ping(h2, NULL);
        CHECK(rc == TE_ECONNRESET, "ping on a dead session: rc=%d", rc);
    }

    ta_sql_disconnect(h2);
}

static void
check_auth(const ta_sql_config *cfg)
{
    ta_sql_config  bad = *cfg;
    ta_sql_errinfo err;
    int            h = -1;
    te_errno       rc;

    bad.password = "wrong-secret-0123456789";
    rc = ta_sql_connect(&bad, &h, NULL, &err);
    CHECK(rc == TE_EACCES && err.cls == TA_SQL_ERR_AUTH,
          "wrong password: rc=%d cls=%d %s", rc, err.cls, err.message);
    CHECK(strstr(err.message, bad.password) == NULL,
          "the secret is in the error message: %s", err.message);

    bad = *cfg;
    bad.port = 1;
    bad.timeout_ms = 3000;
    rc = ta_sql_connect(&bad, &h, NULL, &err);
    CHECK(rc == TE_ECONNREFUSED && err.cls == TA_SQL_ERR_CONNECT,
          "closed port: rc=%d cls=%d %s", rc, err.cls, err.message);
}

static void
run_engine(const char *name, const ta_sql_config *cfg)
{
    ta_sql_errinfo err;
    te_string      server = TE_STRING_INIT;
    unsigned int   caps = 0;
    unsigned int   iso = 0;
    unsigned int   def_iso = 0;
    int            tls = 0;
    int            h = -1;
    int            conns = -1;
    int            stmts = -1;
    uint64_t       us = 0;
    te_errno       rc;

    engine_name = name;

    rc = ta_sql_connect(cfg, &h, &us, &err);
    CHECK(rc == 0, "connect: rc=%d cls=%d %s", rc, err.cls, err.message);
    if (rc != 0)
        return;

    rc = ta_sql_capabilities(h, &caps, &iso, &def_iso, &tls, &server);
    CHECK(rc == 0 && (caps & TA_SQL_CAP_TX) != 0 && (iso & def_iso) != 0,
          "capabilities: rc=%d caps=0x%x iso=0x%x", rc, caps, iso);
    printf("[%s] %s, connected in %llu us, caps 0x%x, isolation 0x%x\n",
           name, server.ptr, (unsigned long long)us, caps, iso);
    te_string_free(&server);

    CHECK(ta_sql_ping(h, &us) == 0, "ping failed");

    check_values(h, cfg);
    check_fetch(h);
    check_tx(h);
    check_errors(h);
    check_meta(h);
    check_deadlines(h, caps);
    check_locks(cfg, h, caps);
    if ((caps & TA_SQL_CAP_AUTH) != 0)
        check_auth(cfg);

    ta_sql_stats(&conns, &stmts, NULL);
    CHECK(conns == 1 && stmts == 0,
          "leftovers before disconnect: %d connections, %d statements",
          conns, stmts);

    /* Disconnect closes what is still prepared on the connection. */
    {
        int st = -1;

        rc = ta_sql_prepare(h, "SELECT 1", &st, NULL);
        CHECK(rc == 0, "prepare: rc=%d", rc);
    }
    rc = ta_sql_disconnect(h);
    CHECK(rc == 0, "disconnect: rc=%d", rc);
    ta_sql_stats(&conns, &stmts, NULL);
    CHECK(conns == 0 && stmts == 0,
          "leftovers after disconnect: %d connections, %d statements",
          conns, stmts);
    CHECK(ta_sql_ping(h, NULL) == TE_EBADF, "a closed handle still works");
}

/* Parse "host port user password database". */
static bool
parse_server(const char *spec, int engine, ta_sql_config *cfg)
{
    static char buf[2][512];
    char       *copy = buf[engine == TA_SQL_ENGINE_POSTGRES ? 0 : 1];
    char       *save = NULL;
    char       *port;

    snprintf(copy, sizeof(buf[0]), "%s", spec);
    memset(cfg, 0, sizeof(*cfg));
    cfg->engine = engine;
    cfg->host = strtok_r(copy, " ", &save);
    port = strtok_r(NULL, " ", &save);
    cfg->user = strtok_r(NULL, " ", &save);
    cfg->password = strtok_r(NULL, " ", &save);
    cfg->dbname = strtok_r(NULL, " ", &save);
    if (cfg->dbname == NULL)
        return false;
    cfg->port = atoi(port);
    cfg->timeout_ms = 10000;
    return true;
}

int
main(void)
{
    te_string     versions = TE_STRING_INIT;
    unsigned int  engines = 0;
    ta_sql_config cfg;
    const char   *spec;
    bool          require_all = getenv("TSF_SQL_REQUIRE_ALL") != NULL;
    int           ran = 0;

    ta_sql_available(&engines, &versions);
    printf("drivers: 0x%x (%s)\n", engines,
           versions.ptr != NULL ? versions.ptr : "none");
    te_string_free(&versions);

    spec = getenv("TSF_SQL_SQLITE");
    if (spec != NULL && (engines & TA_SQL_ENGINE_SQLITE) != 0)
    {
        memset(&cfg, 0, sizeof(cfg));
        cfg.engine = TA_SQL_ENGINE_SQLITE;
        cfg.dbname = spec;
        unlink(spec);
        run_engine("sqlite", &cfg);
        unlink(spec);
        ran++;
    }
    else if (require_all)
    {
        fprintf(stderr, "FAIL: SQLite was not tested\n");
        failures++;
    }

    spec = getenv("TSF_SQL_PG");
    if (spec != NULL && (engines & TA_SQL_ENGINE_POSTGRES) != 0 &&
        parse_server(spec, TA_SQL_ENGINE_POSTGRES, &cfg))
    {
        run_engine("postgres", &cfg);
        ran++;
    }
    else if (require_all)
    {
        fprintf(stderr, "FAIL: PostgreSQL was not tested\n");
        failures++;
    }

    spec = getenv("TSF_SQL_MY");
    if (spec != NULL && (engines & TA_SQL_ENGINE_MYSQL) != 0 &&
        parse_server(spec, TA_SQL_ENGINE_MYSQL, &cfg))
    {
        run_engine("mysql", &cfg);
        ran++;
    }
    else if (require_all)
    {
        fprintf(stderr, "FAIL: MySQL / MariaDB was not tested\n");
        failures++;
    }

    if (ran == 0)
    {
        fprintf(stderr, "FAIL: no engine was tested\n");
        failures++;
    }
    printf("%d engine(s) tested, %d failure(s)\n", ran, failures);
    return failures == 0 ? 0 : 1;
}
