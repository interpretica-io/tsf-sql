/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL databases from a test
 *
 * @defgroup tapi_sql SQL databases (tapi_sql)
 * @{
 *
 * Connections, statements and transactions on PostgreSQL, SQLite and
 * MySQL / MariaDB, through one API. The database is reached from a
 * Test Agent: the connection lives in the agent's RPC server, over the
 * engine's own client library, and the test holds an opaque
 * #tapi_sql_conn.
 *
 * - tapi_sql_connect() / tapi_sql_disconnect() / tapi_sql_ping() /
 *   tapi_sql_capabilities() manage a connection;
 * - tapi_sql_execute() runs a statement, tapi_sql_query() runs one and
 *   brings its rows back;
 * - tapi_sql_prepare() / tapi_sql_bind() / tapi_sql_stmt_execute() /
 *   tapi_sql_fetch() / tapi_sql_statement_free() are the prepared
 *   statement, with typed parameters and a result read in bounded
 *   batches;
 * - tapi_sql_begin() / tapi_sql_commit() / tapi_sql_rollback() /
 *   tapi_sql_savepoint() / tapi_sql_rollback_to() control transactions;
 * - tapi_sql_tables() / tapi_sql_columns() / tapi_sql_indexes() /
 *   tapi_sql_constraints() / tapi_sql_foreign_keys() read the schema
 *   into the same structures whatever the engine;
 * - tapi_sql_execute_start() / tapi_sql_execute_wait() /
 *   tapi_sql_cancel() / tapi_sql_set_timeout() are what a concurrency
 *   test is built from;
 * - @ref tapi_sql_assert (tapi_sql_assert.h) checks database state,
 *   @ref tapi_sql_perf (tapi_sql_perf.h) measures,
 *   @ref tapi_sql_audit (tapi_sql_audit.h) reads a server's security
 *   posture.
 *
 * Parameters are written @c ? in a statement on every engine. That and
 * nothing else is translated: the SQL a test writes is the SQL the
 * database gets.
 *
 * The secret of a connection is given in #tapi_sql_config and goes to
 * the agent inside the connect call. It is never written to a log, a
 * command line or an error message.
 */

#ifndef __TAPI_SQL_H__
#define __TAPI_SQL_H__

#include <stdint.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Database engines. */
typedef enum tapi_sql_engine {
    TAPI_SQL_ENGINE_SQLITE = 1,     /**< SQLite. */
    TAPI_SQL_ENGINE_POSTGRES = 2,   /**< PostgreSQL. */
    TAPI_SQL_ENGINE_MYSQL = 4,      /**< MySQL / MariaDB. */
} tapi_sql_engine;

/** TLS modes of a connection. */
typedef enum tapi_sql_tls {
    TAPI_SQL_TLS_DISABLE = 0,       /**< Plain text. */
    TAPI_SQL_TLS_PREFER = 1,        /**< Encrypt when the server offers. */
    TAPI_SQL_TLS_REQUIRE = 2,       /**< Encrypt; check nothing. */
    TAPI_SQL_TLS_VERIFY_CA = 3,     /**< Encrypt; the CA must be trusted. */
    TAPI_SQL_TLS_VERIFY_FULL = 4,   /**< ... and the host name match. */
} tapi_sql_tls;

/** What a driver and its engine can do (a bitmask). */
typedef enum tapi_sql_cap {
    TAPI_SQL_CAP_TX = 0x0001,           /**< Transactions. */
    TAPI_SQL_CAP_SAVEPOINT = 0x0002,    /**< Savepoints. */
    TAPI_SQL_CAP_MULTI_CURSOR = 0x0004, /**< Several open results on one
                                             connection. */
    TAPI_SQL_CAP_TLS = 0x0008,          /**< TLS to the server. */
    TAPI_SQL_CAP_UINT64 = 0x0010,       /**< Full unsigned 64-bit. */
    TAPI_SQL_CAP_BOOL = 0x0020,         /**< A boolean type of its own. */
    TAPI_SQL_CAP_JSON = 0x0040,         /**< A JSON type of its own. */
    TAPI_SQL_CAP_DECIMAL = 0x0080,      /**< Exact decimals. */
    TAPI_SQL_CAP_STMT_TIMEOUT = 0x0100, /**< Statement deadline. */
    TAPI_SQL_CAP_LOCK_TIMEOUT = 0x0200, /**< Lock-wait deadline. */
    TAPI_SQL_CAP_CANCEL = 0x0400,       /**< Cancel a running statement. */
    TAPI_SQL_CAP_ROW_LOCKS = 0x0800,    /**< Writers wait for each other
                                             row by row. */
    TAPI_SQL_CAP_DEADLOCK = 0x1000,     /**< Deadlocks are detected. */
    TAPI_SQL_CAP_NETWORK = 0x2000,      /**< A server over the network. */
    TAPI_SQL_CAP_AUTH = 0x4000,         /**< Accounts and privileges. */
    TAPI_SQL_CAP_CHECK_META = 0x8000,   /**< CHECK constraints appear in
                                             introspection. */
} tapi_sql_cap;

/** Transaction isolation levels (one bit each). */
typedef enum tapi_sql_isolation {
    TAPI_SQL_ISO_DEFAULT = 0,           /**< Whatever the engine gives. */
    TAPI_SQL_ISO_READ_UNCOMMITTED = 1,
    TAPI_SQL_ISO_READ_COMMITTED = 2,
    TAPI_SQL_ISO_REPEATABLE_READ = 4,
    TAPI_SQL_ISO_SERIALIZABLE = 8,
} tapi_sql_isolation;

/** What went wrong, whatever the engine calls it. */
typedef enum tapi_sql_err_class {
    TAPI_SQL_ERR_NONE = 0,
    TAPI_SQL_ERR_OTHER = 1,         /**< Anything not named below. */
    TAPI_SQL_ERR_SYNTAX = 2,        /**< The statement does not parse. */
    TAPI_SQL_ERR_NOT_FOUND = 3,     /**< No such table / column. */
    TAPI_SQL_ERR_CONSTRAINT = 4,    /**< A constraint was violated. */
    TAPI_SQL_ERR_DEADLOCK = 5,      /**< Chosen as a deadlock victim. */
    TAPI_SQL_ERR_SERIALIZATION = 6, /**< Could not serialize access. */
    TAPI_SQL_ERR_LOCK_TIMEOUT = 7,  /**< Gave up waiting for a lock. */
    TAPI_SQL_ERR_TIMEOUT = 8,       /**< Statement deadline passed. */
    TAPI_SQL_ERR_CANCELLED = 9,     /**< Cancelled on request. */
    TAPI_SQL_ERR_CONN_LOST = 10,    /**< The connection is gone. */
    TAPI_SQL_ERR_AUTH = 11,         /**< Credentials refused. */
    TAPI_SQL_ERR_TLS = 12,          /**< TLS could not be set up. */
    TAPI_SQL_ERR_PERMISSION = 13,   /**< The account may not do this. */
    TAPI_SQL_ERR_CONNECT = 14,      /**< The server cannot be reached. */
} tapi_sql_err_class;

/** Kinds of deadline. */
typedef enum tapi_sql_timeout {
    TAPI_SQL_TIMEOUT_STATEMENT = 1, /**< A statement's run time. */
    TAPI_SQL_TIMEOUT_LOCK = 2,      /**< Waiting for a lock. */
} tapi_sql_timeout;

/** Types of a value. The enumerators are the letters of the wire. */
typedef enum tapi_sql_type {
    TAPI_SQL_NULL = 'N',        /**< SQL NULL. */
    TAPI_SQL_BOOL = 'B',        /**< Boolean. */
    TAPI_SQL_INT = 'I',         /**< Signed integer. */
    TAPI_SQL_UINT = 'U',        /**< Unsigned integer. */
    TAPI_SQL_FLOAT = 'F',       /**< Floating point. */
    TAPI_SQL_DECIMAL = 'D',     /**< Exact decimal, as its digits. */
    TAPI_SQL_TEXT = 'T',        /**< Text. */
    TAPI_SQL_JSON = 'J',        /**< JSON, as text. */
    TAPI_SQL_DATETIME = 'M',    /**< Date / time, as the engine prints. */
    TAPI_SQL_BINARY = 'X',      /**< Bytes. */
} tapi_sql_type;

/**
 * A value: a parameter going in or a cell coming out.
 *
 * Numbers are in @c i / @c u / @c f. Text, decimals, dates, JSON and
 * binary are in @c str / @c len (NUL-terminated as well, for
 * convenience). A cell read from a database also has @c str set to the
 * digits of a number, so any cell can be printed.
 */
typedef struct tapi_sql_value {
    tapi_sql_type type;     /**< What it is. */
    int64_t       i;        /**< BOOL (0/1) and INT. */
    uint64_t      u;        /**< UINT. */
    double        f;        /**< FLOAT. */
    const char   *str;      /**< Text or bytes; see above. */
    size_t        len;      /**< Length of @c str. */
} tapi_sql_value;

/** @name Parameter constructors
 * The value refers to the caller's string; nothing is copied.
 * @{ */
static inline tapi_sql_value
tapi_sql_null(void)
{
    tapi_sql_value v = { .type = TAPI_SQL_NULL };

    return v;
}

static inline tapi_sql_value
tapi_sql_bool(bool value)
{
    tapi_sql_value v = { .type = TAPI_SQL_BOOL, .i = value ? 1 : 0 };

    return v;
}

static inline tapi_sql_value
tapi_sql_int(int64_t value)
{
    tapi_sql_value v = { .type = TAPI_SQL_INT, .i = value };

    return v;
}

static inline tapi_sql_value
tapi_sql_uint(uint64_t value)
{
    tapi_sql_value v = { .type = TAPI_SQL_UINT, .u = value };

    return v;
}

static inline tapi_sql_value
tapi_sql_float(double value)
{
    tapi_sql_value v = { .type = TAPI_SQL_FLOAT, .f = value };

    return v;
}

static inline tapi_sql_value
tapi_sql_str(tapi_sql_type type, const char *value)
{
    tapi_sql_value v = { .type = type, .str = value,
                         .len = value != NULL ? strlen(value) : 0 };

    return v;
}

static inline tapi_sql_value
tapi_sql_text(const char *value)
{
    return tapi_sql_str(TAPI_SQL_TEXT, value);
}

static inline tapi_sql_value
tapi_sql_decimal(const char *digits)
{
    return tapi_sql_str(TAPI_SQL_DECIMAL, digits);
}

static inline tapi_sql_value
tapi_sql_datetime(const char *value)
{
    return tapi_sql_str(TAPI_SQL_DATETIME, value);
}

static inline tapi_sql_value
tapi_sql_json(const char *value)
{
    return tapi_sql_str(TAPI_SQL_JSON, value);
}

static inline tapi_sql_value
tapi_sql_binary(const void *data, size_t len)
{
    tapi_sql_value v = { .type = TAPI_SQL_BINARY,
                         .str = (const char *)data, .len = len };

    return v;
}
/** @} */

/** The rows a query returned (or one fetched batch of them). */
typedef struct tapi_sql_result {
    unsigned int    ncols;      /**< Columns. */
    char          **names;      /**< Column names. */
    tapi_sql_type  *types;      /**< Column types, as declared. */
    unsigned int    nrows;      /**< Rows. */
    tapi_sql_value *cells;      /**< @c nrows x @c ncols, row by row. */
    unsigned int    cap_rows;   /**< Rows allocated. */
} tapi_sql_result;

/** An empty result. */
#define TAPI_SQL_RESULT_INIT { 0, NULL, NULL, 0, NULL, 0 }

/** Parameters of a connection. Unused strings are @c NULL. */
typedef struct tapi_sql_config {
    tapi_sql_engine engine;     /**< Database engine. */
    const char     *host;       /**< Server host. */
    int             port;       /**< Server port; 0 = default. */
    const char     *dbname;     /**< Database, or the SQLite file path
                                     on the agent. */
    const char     *user;       /**< Account. */
    const char     *password;   /**< Its secret. Never logged. */
    tapi_sql_tls    tls_mode;   /**< TLS mode. */
    const char     *tls_ca;     /**< CA bundle path on the agent. */
    const char     *tls_cert;   /**< Client certificate path. */
    const char     *tls_key;    /**< Client key path. */
    int             timeout_ms; /**< Connect deadline; <=0 = default. */
} tapi_sql_config;

/** A database failure, normalised. */
typedef struct tapi_sql_error {
    tapi_sql_err_class cls;             /**< What went wrong. */
    int                native;          /**< The driver's own code. */
    char               sqlstate[8];     /**< SQLSTATE, or "". */
    char               message[1024];   /**< The driver's message. */
} tapi_sql_error;

/** What a connection can do. */
typedef struct tapi_sql_caps {
    unsigned int       caps;            /**< Mask of #tapi_sql_cap. */
    unsigned int       isolation;       /**< Mask of the isolation levels
                                             the engine accepts. */
    tapi_sql_isolation default_iso;     /**< The level of a plain begin. */
    bool               tls;             /**< This connection is
                                             encrypted. */
    char               server[160];     /**< Server version. */
} tapi_sql_caps;

/** A connection (opaque). */
typedef struct tapi_sql_conn tapi_sql_conn;

/** A prepared statement (opaque). */
typedef struct tapi_sql_stmt tapi_sql_stmt;

/* ---- drivers ------------------------------------------------------ */

/**
 * Is any database driver compiled into the agent?
 *
 * @param rpcs  RPC server.
 *
 * @return @c true when there is one.
 */
extern bool tapi_sql_available(rcf_rpc_server *rpcs);

/**
 * Does the agent have the driver of an engine? A test of an engine the
 * agent lacks has nothing to run on and skips.
 *
 * @param rpcs    RPC server.
 * @param engine  Engine.
 *
 * @return @c true when the driver is there.
 */
extern bool tapi_sql_supports(rcf_rpc_server *rpcs, tapi_sql_engine engine);

/** Spell an engine out ("sqlite", "postgres", "mysql"). */
extern const char *tapi_sql_engine2str(tapi_sql_engine engine);

/**
 * Find an engine by name: "sqlite", "postgres" (or "postgresql", "pg"),
 * "mysql" (or "mariadb").
 *
 * @return 0, or TE_EINVAL for an unknown name.
 */
extern te_errno tapi_sql_engine_from_str(const char *name,
                                         tapi_sql_engine *engine);

/** Spell an error class out ("constraint", "deadlock", ...). */
extern const char *tapi_sql_err_class2str(tapi_sql_err_class cls);

/** Spell an isolation level out ("read committed", ...). */
extern const char *tapi_sql_isolation2str(tapi_sql_isolation isolation);

/* ---- connection management ---------------------------------------- */

/**
 * Establish a database connection.
 *
 * @param[in]  rpcs  RPC server on the agent that reaches the database.
 * @param[in]  cfg   Connection parameters (copied).
 * @param[out] conn  The connection.
 *
 * @return Status code; on failure tapi_sql_connect_error() says why.
 */
extern te_errno tapi_sql_connect(rcf_rpc_server *rpcs,
                                 const tapi_sql_config *cfg,
                                 tapi_sql_conn **conn);

/**
 * Try to connect when failing is a possible outcome: the same as
 * tapi_sql_connect(), with the reason of a failure handed back.
 *
 * @param[in]  rpcs  RPC server.
 * @param[in]  cfg   Connection parameters.
 * @param[out] conn  The connection, when it was established.
 * @param[out] err   The failure, when it was not (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_sql_connect_err(rcf_rpc_server *rpcs,
                                     const tapi_sql_config *cfg,
                                     tapi_sql_conn **conn,
                                     tapi_sql_error *err);

/**
 * Close a connection. Statements prepared on it are released on the
 * agent; their #tapi_sql_stmt objects must still be freed with
 * tapi_sql_statement_free(), which is all that may be done with them
 * from then on.
 *
 * @param conn  Connection, or @c NULL.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_disconnect(tapi_sql_conn *conn);

/**
 * Close the session and open a new one with the same parameters. The
 * object stays; statements prepared on the old session are gone.
 *
 * @param conn  Connection.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_reconnect(tapi_sql_conn *conn);

/**
 * Verify connectivity: one round trip to the server.
 *
 * @param conn  Connection.
 *
 * @return 0 when the server answered.
 */
extern te_errno tapi_sql_ping(tapi_sql_conn *conn);

/**
 * Enumerate what the driver, the engine and this connection can do.
 *
 * @param[in]  conn  Connection.
 * @param[out] caps  Capabilities.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_capabilities(tapi_sql_conn *conn,
                                      tapi_sql_caps *caps);

/**
 * Does the connection have a capability? A shortcut over
 * tapi_sql_capabilities() for the skip at the top of a test.
 */
extern bool tapi_sql_has_cap(tapi_sql_conn *conn, tapi_sql_cap cap);

/** The engine of a connection. */
extern tapi_sql_engine tapi_sql_conn_engine(const tapi_sql_conn *conn);

/** The RPC server of a connection. */
extern rcf_rpc_server *tapi_sql_conn_rpcs(const tapi_sql_conn *conn);

/** The parameters a connection was opened with. */
extern const tapi_sql_config *tapi_sql_conn_config(
                                            const tapi_sql_conn *conn);

/**
 * The last database failure on a connection. Valid until the next call
 * on it.
 */
extern const tapi_sql_error *tapi_sql_last_error(const tapi_sql_conn *conn);

/**
 * How long the last statement (or the connect, or the ping) took, as
 * measured on the agent around the driver call, in microseconds.
 */
extern uint64_t tapi_sql_last_elapsed_us(const tapi_sql_conn *conn);

/**
 * Describe a connection for a log: engine, address, database, account,
 * TLS mode. The secret is not part of it.
 *
 * @param[in]  conn  Connection.
 * @param[out] dest  The description is appended here.
 */
extern void tapi_sql_conn_describe(const tapi_sql_conn *conn,
                                   te_string *dest);

/* ---- query execution ---------------------------------------------- */

/**
 * Execute a statement without returning rows.
 *
 * The call returns when the statement has finished. One that may run
 * longer than an RPC is allowed to (the RPC server's @c timeout, ten
 * seconds unless raised) is started with tapi_sql_execute_start()
 * instead.
 *
 * @param[in]  conn      Connection.
 * @param[in]  sql       The statement.
 * @param[out] affected  Rows changed, -1 when unknown (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_sql_execute(tapi_sql_conn *conn, const char *sql,
                                 int64_t *affected);

/** tapi_sql_execute() with the statement built printf-style. */
extern te_errno tapi_sql_execute_fmt(tapi_sql_conn *conn, int64_t *affected,
                                     const char *fmt, ...)
                                     TE_LIKE_PRINTF(3, 4);

/**
 * Execute a statement and retrieve its whole result. For a result that
 * may be large use tapi_sql_prepare() and tapi_sql_fetch().
 *
 * @param[in]  conn  Connection.
 * @param[in]  sql   The statement.
 * @param[out] res   The rows; release with tapi_sql_result_free().
 *
 * @return Status code.
 */
extern te_errno tapi_sql_query(tapi_sql_conn *conn, const char *sql,
                               tapi_sql_result *res);

/**
 * Execute a parameterised statement in one call.
 *
 * @param[in]  conn     Connection.
 * @param[in]  sql      The statement, parameters written @c ?.
 * @param[in]  params   Parameter values.
 * @param[in]  nparams  How many.
 * @param[out] res      The rows (@c NULL when none are wanted).
 * @param[out] affected Rows changed (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_sql_query_params(tapi_sql_conn *conn, const char *sql,
                                      const tapi_sql_value *params,
                                      unsigned int nparams,
                                      tapi_sql_result *res,
                                      int64_t *affected);

/**
 * Execute a query that yields one number (a count, a sum).
 *
 * @param[in]  conn   Connection.
 * @param[in]  sql    The query.
 * @param[out] value  The first cell of the first row.
 *
 * @return Status code; TE_ENODATA when there is no row or the cell is
 *         NULL or not a number.
 */
extern te_errno tapi_sql_query_int(tapi_sql_conn *conn, const char *sql,
                                   int64_t *value);

/**
 * Prepare a parameterised statement.
 *
 * @param[in]  conn  Connection.
 * @param[in]  sql   The statement, parameters written @c ?.
 * @param[out] stmt  The statement.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_prepare(tapi_sql_conn *conn, const char *sql,
                                 tapi_sql_stmt **stmt);

/** How many parameters a prepared statement takes. */
extern unsigned int tapi_sql_stmt_nparams(const tapi_sql_stmt *stmt);

/**
 * Bind a typed parameter. A binding stays until it is bound again, so a
 * statement can be executed many times, rebinding only what changes.
 *
 * @param stmt   Statement.
 * @param index  Parameter number, from 1.
 * @param value  The value.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_bind(tapi_sql_stmt *stmt, unsigned int index,
                              tapi_sql_value value);

/**
 * Execute a prepared statement with its current bindings. The rows, if
 * any, stay with the database until tapi_sql_fetch() asks for them.
 *
 * @param[in]  stmt      Statement.
 * @param[out] affected  Rows changed (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_sql_stmt_execute(tapi_sql_stmt *stmt,
                                      int64_t *affected);

/**
 * Retrieve the next batch of rows of an executed statement. Only this
 * batch is ever in memory, on the agent and here.
 *
 * @param[in]  stmt      Statement.
 * @param[in]  max_rows  Largest batch wanted; 0 = 1000.
 * @param[out] res       The batch (what it held before is released).
 * @param[out] eof       @c true when the result is exhausted.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_fetch(tapi_sql_stmt *stmt, unsigned int max_rows,
                               tapi_sql_result *res, bool *eof);

/**
 * tapi_sql_fetch() with a byte bound as well: once a row is in, no row
 * that would take the batch past @p max_bytes of encoded data.
 */
extern te_errno tapi_sql_fetch_bounded(tapi_sql_stmt *stmt,
                                       unsigned int max_rows,
                                       unsigned int max_bytes,
                                       tapi_sql_result *res, bool *eof);

/**
 * Release a statement, on the agent and here.
 *
 * @param stmt  Statement, or @c NULL.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_statement_free(tapi_sql_stmt *stmt);

/* ---- results ------------------------------------------------------ */

/** Release what a result holds and make it empty. */
extern void tapi_sql_result_free(tapi_sql_result *res);

/**
 * A cell of a result.
 *
 * @return The cell, or @c NULL when @p row / @p col are out of range.
 */
extern const tapi_sql_value *tapi_sql_result_cell(
                                    const tapi_sql_result *res,
                                    unsigned int row, unsigned int col);

/**
 * Print a value for a log or a diagnostic: @c NULL, the digits of a
 * number, text in quotes, binary as @c x'..'.
 */
extern void tapi_sql_value_print(const tapi_sql_value *value,
                                 te_string *dest);

/** Log a result as a table (RING level), at most @p max_rows rows. */
extern void tapi_sql_result_log(const tapi_sql_result *res,
                                const char *title, unsigned int max_rows);

/* ---- transaction management --------------------------------------- */

/** Begin a transaction at the engine's default isolation level. */
extern te_errno tapi_sql_begin(tapi_sql_conn *conn);

/**
 * Begin a transaction at a given isolation level.
 *
 * @return Status code; TE_EOPNOTSUPP when the engine does not have the
 *         level (see tapi_sql_capabilities()).
 */
extern te_errno tapi_sql_begin_iso(tapi_sql_conn *conn,
                                   tapi_sql_isolation isolation);

/** Commit the transaction. */
extern te_errno tapi_sql_commit(tapi_sql_conn *conn);

/** Roll the transaction back. */
extern te_errno tapi_sql_rollback(tapi_sql_conn *conn);

/** Set a savepoint; @p name is letters, digits and underscores. */
extern te_errno tapi_sql_savepoint(tapi_sql_conn *conn, const char *name);

/** Roll back to a savepoint, keeping the transaction open. */
extern te_errno tapi_sql_rollback_to(tapi_sql_conn *conn, const char *name);

/** Release a savepoint. */
extern te_errno tapi_sql_release(tapi_sql_conn *conn, const char *name);

/* ---- schema introspection ----------------------------------------- */

/** A table or a view. */
typedef struct tapi_sql_table {
    char *name;         /**< Name. */
    bool  is_view;      /**< A view rather than a table. */
} tapi_sql_table;

/** A column. */
typedef struct tapi_sql_column {
    char         *name;         /**< Name. */
    unsigned int  position;     /**< Position in the table, from 1. */
    tapi_sql_type type;         /**< Normalised type. */
    char         *native_type;  /**< The type as the engine names it. */
    bool          nullable;     /**< NULL is allowed. */
    char         *default_value; /**< Default as the engine spells it,
                                      or @c NULL. */
    bool          primary_key;  /**< Part of the primary key. */
} tapi_sql_column;

/** An index. */
typedef struct tapi_sql_index {
    char *name;         /**< Name. */
    bool  unique;       /**< Enforces uniqueness. */
    bool  primary;      /**< Backs the primary key. */
    char *columns;      /**< Columns, comma-separated, in index order. */
} tapi_sql_index;

/** Kinds of constraint. */
typedef enum tapi_sql_constraint_kind {
    TAPI_SQL_CONSTRAINT_PRIMARY = 1,
    TAPI_SQL_CONSTRAINT_UNIQUE,
    TAPI_SQL_CONSTRAINT_FOREIGN,
    TAPI_SQL_CONSTRAINT_CHECK,
} tapi_sql_constraint_kind;

/** A constraint. */
typedef struct tapi_sql_constraint {
    char                    *name;      /**< Name. */
    tapi_sql_constraint_kind kind;      /**< Kind. */
    char                    *columns;   /**< Columns, comma-separated. */
} tapi_sql_constraint;

/** A foreign key. */
typedef struct tapi_sql_foreign_key {
    char *name;         /**< Name (SQLite has none: "fk_<n>"). */
    char *columns;      /**< Referencing columns, comma-separated. */
    char *ref_table;    /**< Referenced table. */
    char *ref_columns;  /**< Referenced columns. */
    char *on_delete;    /**< "cascade", "restrict", "no action", ... */
    char *on_update;    /**< The same, for updates. */
} tapi_sql_foreign_key;

/**
 * List the tables and views of the connection's current schema.
 *
 * @param[in]  conn    Connection.
 * @param[out] tables  The tables; release with tapi_sql_tables_free().
 * @param[out] n       How many.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_tables(tapi_sql_conn *conn, tapi_sql_table **tables,
                                unsigned int *n);

/** List the columns of a table, in table order. */
extern te_errno tapi_sql_columns(tapi_sql_conn *conn, const char *table,
                                 tapi_sql_column **columns, unsigned int *n);

/** List the indexes of a table. */
extern te_errno tapi_sql_indexes(tapi_sql_conn *conn, const char *table,
                                 tapi_sql_index **indexes, unsigned int *n);

/** List the constraints of a table. */
extern te_errno tapi_sql_constraints(tapi_sql_conn *conn, const char *table,
                                     tapi_sql_constraint **constraints,
                                     unsigned int *n);

/** List the foreign keys of a table. */
extern te_errno tapi_sql_foreign_keys(tapi_sql_conn *conn, const char *table,
                                      tapi_sql_foreign_key **keys,
                                      unsigned int *n);

/** @name Releasing introspection results
 * @{ */
extern void tapi_sql_tables_free(tapi_sql_table *tables, unsigned int n);
extern void tapi_sql_columns_free(tapi_sql_column *columns, unsigned int n);
extern void tapi_sql_indexes_free(tapi_sql_index *indexes, unsigned int n);
extern void tapi_sql_constraints_free(tapi_sql_constraint *constraints,
                                      unsigned int n);
extern void tapi_sql_foreign_keys_free(tapi_sql_foreign_key *keys,
                                       unsigned int n);
/** @} */

/* ---- deadlines, synchronisation points, cancellation -------------- */

/**
 * Set a deadline for what follows on the connection. A statement past
 * its deadline fails with TE_ETIMEDOUT and the error class
 * #TAPI_SQL_ERR_TIMEOUT or #TAPI_SQL_ERR_LOCK_TIMEOUT.
 *
 * @param conn  Connection.
 * @param kind  A statement's run time, or the wait for a lock.
 * @param ms    The deadline; 0 switches it off.
 *
 * @return Status code; TE_EOPNOTSUPP when the engine has no such
 *         deadline.
 */
extern te_errno tapi_sql_set_timeout(tapi_sql_conn *conn,
                                     tapi_sql_timeout kind, unsigned int ms);

/**
 * Start a statement and return at once. The agent runs it on a thread
 * of its own; until tapi_sql_execute_wait() collects it the connection
 * takes nothing else (TE_EBUSY) but tapi_sql_cancel().
 *
 * This is the synchronisation point of a concurrency test: start the
 * statement that must block, check with a short wait that it has not
 * finished, release what it waits for from another session, collect.
 *
 * @param conn  Connection.
 * @param sql   The statement.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_execute_start(tapi_sql_conn *conn, const char *sql);

/**
 * Wait for the statement tapi_sql_execute_start() started.
 *
 * @param[in]  conn        Connection.
 * @param[in]  timeout_ms  How long to wait; 0 only looks.
 * @param[out] done        @c true when the statement has finished.
 * @param[out] affected    Rows it changed (may be @c NULL).
 *
 * @return The statement's own status once it has finished;
 *         TE_EINPROGRESS (and @p done @c false) while it still runs.
 */
extern te_errno tapi_sql_execute_wait(tapi_sql_conn *conn,
                                      unsigned int timeout_ms, bool *done,
                                      int64_t *affected);

/**
 * Ask the server to stop what the connection is running. The stopped
 * statement fails with TE_ECANCELED.
 */
extern te_errno tapi_sql_cancel(tapi_sql_conn *conn);

/**
 * What the agent's RPC server still holds: the oracle of a cleanup
 * test.
 *
 * @param[in]  rpcs       RPC server.
 * @param[out] conns      Open connections (may be @c NULL).
 * @param[out] stmts      Open statements (may be @c NULL).
 * @param[out] max_batch  The largest fetch batch the agent built so
 *                        far, in bytes (may be @c NULL).
 *
 * @return Status code.
 */
extern te_errno tapi_sql_stats(rcf_rpc_server *rpcs, unsigned int *conns,
                               unsigned int *stmts, uint64_t *max_batch);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_H__ */

/** @} */
