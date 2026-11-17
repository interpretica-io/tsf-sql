/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL database sessions on an agent
 *
 * ta_sql keeps database connections, prepared statements and
 * transaction state in the process that links it (the agent's RPC
 * server) and talks to the databases over their client libraries. A
 * connection and a statement are named by small integers; the values
 * travel as text cells (see sql_rpc.x.m4 for the format).
 *
 * Every function returns a TE status. A failure of the database itself
 * is also kept, normalised, as the connection's last error
 * (ta_sql_error()).
 */

#ifndef __TA_SQL_H__
#define __TA_SQL_H__

#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Database engines; one bit each, so a set of them is a mask. */
enum {
    TA_SQL_ENGINE_SQLITE = 1,
    TA_SQL_ENGINE_POSTGRES = 2,
    TA_SQL_ENGINE_MYSQL = 4,
};

/** TLS modes of a connection. */
enum {
    TA_SQL_TLS_DISABLE = 0,     /**< Plain text. */
    TA_SQL_TLS_PREFER = 1,      /**< Encrypt when the server offers. */
    TA_SQL_TLS_REQUIRE = 2,     /**< Encrypt; check nothing. */
    TA_SQL_TLS_VERIFY_CA = 3,   /**< Encrypt; the CA must be trusted. */
    TA_SQL_TLS_VERIFY_FULL = 4, /**< ... and the host name must match. */
};

/** What a driver and its engine can do. */
enum {
    TA_SQL_CAP_TX = 0x0001,             /**< Transactions. */
    TA_SQL_CAP_SAVEPOINT = 0x0002,      /**< Savepoints. */
    TA_SQL_CAP_MULTI_CURSOR = 0x0004,   /**< Several open results on one
                                             connection at a time. */
    TA_SQL_CAP_TLS = 0x0008,            /**< TLS to the server. */
    TA_SQL_CAP_UINT64 = 0x0010,         /**< Full unsigned 64-bit. */
    TA_SQL_CAP_BOOL = 0x0020,           /**< A boolean type of its own. */
    TA_SQL_CAP_JSON = 0x0040,           /**< A JSON type of its own. */
    TA_SQL_CAP_DECIMAL = 0x0080,        /**< Exact decimals. */
    TA_SQL_CAP_STMT_TIMEOUT = 0x0100,   /**< Statement deadline. */
    TA_SQL_CAP_LOCK_TIMEOUT = 0x0200,   /**< Lock-wait deadline. */
    TA_SQL_CAP_CANCEL = 0x0400,         /**< Cancel a running statement. */
    TA_SQL_CAP_ROW_LOCKS = 0x0800,      /**< Writers wait for each other
                                             row by row (not the whole
                                             database). */
    TA_SQL_CAP_DEADLOCK = 0x1000,       /**< Deadlocks are detected. */
    TA_SQL_CAP_NETWORK = 0x2000,        /**< A server reached over the
                                             network. */
    TA_SQL_CAP_AUTH = 0x4000,           /**< Accounts and privileges. */
    TA_SQL_CAP_CHECK_META = 0x8000,     /**< CHECK constraints show up
                                             in introspection. */
};

/** Transaction isolation levels; one bit each. */
enum {
    TA_SQL_ISO_DEFAULT = 0,
    TA_SQL_ISO_READ_UNCOMMITTED = 1,
    TA_SQL_ISO_READ_COMMITTED = 2,
    TA_SQL_ISO_REPEATABLE_READ = 4,
    TA_SQL_ISO_SERIALIZABLE = 8,
};

/** Transaction operations of ta_sql_tx(). */
enum {
    TA_SQL_TX_BEGIN = 1,
    TA_SQL_TX_COMMIT = 2,
    TA_SQL_TX_ROLLBACK = 3,
    TA_SQL_TX_SAVEPOINT = 4,
    TA_SQL_TX_ROLLBACK_TO = 5,
    TA_SQL_TX_RELEASE = 6,
};

/** Kinds of schema objects ta_sql_meta() lists. */
enum {
    TA_SQL_META_TABLES = 1,
    TA_SQL_META_COLUMNS = 2,
    TA_SQL_META_INDEXES = 3,
    TA_SQL_META_CONSTRAINTS = 4,
    TA_SQL_META_FOREIGN_KEYS = 5,
};

/** Kinds of deadline ta_sql_set_timeout() sets. */
enum {
    TA_SQL_TIMEOUT_STATEMENT = 1,
    TA_SQL_TIMEOUT_LOCK = 2,
};

/** Error classes: what went wrong, whatever the engine calls it. */
enum {
    TA_SQL_ERR_NONE = 0,
    TA_SQL_ERR_OTHER = 1,           /**< Anything not named below. */
    TA_SQL_ERR_SYNTAX = 2,          /**< The statement does not parse. */
    TA_SQL_ERR_NOT_FOUND = 3,       /**< No such table / column. */
    TA_SQL_ERR_CONSTRAINT = 4,      /**< A constraint was violated. */
    TA_SQL_ERR_DEADLOCK = 5,        /**< Chosen as a deadlock victim. */
    TA_SQL_ERR_SERIALIZATION = 6,   /**< Could not serialize access. */
    TA_SQL_ERR_LOCK_TIMEOUT = 7,    /**< Gave up waiting for a lock. */
    TA_SQL_ERR_TIMEOUT = 8,         /**< Statement deadline passed. */
    TA_SQL_ERR_CANCELLED = 9,       /**< Cancelled on request. */
    TA_SQL_ERR_CONN_LOST = 10,      /**< The connection is gone. */
    TA_SQL_ERR_AUTH = 11,           /**< Credentials refused. */
    TA_SQL_ERR_TLS = 12,            /**< TLS could not be set up. */
    TA_SQL_ERR_PERMISSION = 13,     /**< The account may not do this. */
    TA_SQL_ERR_CONNECT = 14,        /**< The server cannot be reached. */
    TA_SQL_ERR_READ_ONLY = 15,      /**< The database takes no writes
                                         here: a replica, a read-only
                                         transaction or file. */
};

/** Parameters of a connection. Strings may be @c NULL or empty. */
typedef struct ta_sql_config {
    int         engine;         /**< One TA_SQL_ENGINE_* bit. */
    const char *host;           /**< Server host. */
    int         port;           /**< Server port; 0 = default. */
    const char *dbname;         /**< Database, or the SQLite file. */
    const char *user;           /**< Account. */
    const char *password;       /**< Its secret. */
    int         tls_mode;       /**< TA_SQL_TLS_*. */
    const char *tls_ca;         /**< CA bundle path. */
    const char *tls_cert;       /**< Client certificate path. */
    const char *tls_key;        /**< Client key path. */
    int         timeout_ms;     /**< Connect deadline; <=0 = default. */
} ta_sql_config;

/** A normalised database error. */
typedef struct ta_sql_errinfo {
    int  cls;               /**< TA_SQL_ERR_*. */
    int  native;            /**< The driver's own code. */
    char sqlstate[8];       /**< SQLSTATE or "". */
    char message[1024];     /**< The driver's message. */
} ta_sql_errinfo;

/**
 * Which drivers are compiled in.
 *
 * @param[out] engines   Mask of TA_SQL_ENGINE_* (may be @c NULL).
 * @param[out] versions  Client library versions appended here (may be
 *                       @c NULL).
 *
 * @return @c true when at least one driver is present.
 */
extern bool ta_sql_available(unsigned int *engines, te_string *versions);

/**
 * Open a connection.
 *
 * @param[in]  cfg         Connection parameters.
 * @param[out] handle      Connection handle.
 * @param[out] elapsed_us  Time the connection took to establish.
 * @param[out] err         The failure, when there is one (the secret
 *                         already redacted).
 *
 * @return Status code.
 */
extern te_errno ta_sql_connect(const ta_sql_config *cfg, int *handle,
                               uint64_t *elapsed_us, ta_sql_errinfo *err);

/** Close a connection and every statement prepared on it. */
extern te_errno ta_sql_disconnect(int handle);

/** Check the server still answers. */
extern te_errno ta_sql_ping(int handle, uint64_t *elapsed_us);

/** Report capabilities, isolation levels, TLS state, server version. */
extern te_errno ta_sql_capabilities(int handle, unsigned int *caps,
                                    unsigned int *isolation,
                                    unsigned int *default_iso, int *tls,
                                    te_string *server);

/** Run one statement, discarding rows. */
extern te_errno ta_sql_execute(int handle, const char *sql,
                               int64_t *affected, uint64_t *elapsed_us);

/** Prepare a statement with "?" parameters. */
extern te_errno ta_sql_prepare(int handle, const char *sql, int *stmt,
                               int *nparams);

/** Bind parameter @p index (1-based) to an encoded cell. */
extern te_errno ta_sql_bind(int stmt, int index, const char *cell);

/** Run a prepared statement; describe its result columns. */
extern te_errno ta_sql_stmt_execute(int stmt, int *ncols, te_string *columns,
                                    int64_t *affected,
                                    uint64_t *elapsed_us);

/** Fetch the next bounded batch of rows. */
extern te_errno ta_sql_fetch(int stmt, int max_rows, int max_bytes,
                             te_string *rows, int *nrows, int *eof);

/** Release a statement. */
extern te_errno ta_sql_stmt_free(int stmt);

/** Transaction control, see TA_SQL_TX_*. */
extern te_errno ta_sql_tx(int handle, int op, unsigned int isolation,
                          const char *name);

/** Normalised schema introspection, see TA_SQL_META_*. */
extern te_errno ta_sql_meta(int handle, int kind, const char *table,
                            te_string *rows);

/** Set a statement or lock-wait deadline; 0 switches it off. */
extern te_errno ta_sql_set_timeout(int handle, int kind, int ms);

/** Start a statement on a thread and return. */
extern te_errno ta_sql_async_start(int handle, const char *sql);

/**
 * Wait for the statement ta_sql_async_start() started.
 *
 * @return The statement's own status once it has finished,
 *         TE_EINPROGRESS while it still runs.
 */
extern te_errno ta_sql_async_wait(int handle, int timeout_ms, int *done,
                                  int64_t *affected, uint64_t *elapsed_us);

/** Ask the server to stop what the connection is running. */
extern te_errno ta_sql_cancel(int handle);

/** The last database failure on a connection. */
extern te_errno ta_sql_error(int handle, ta_sql_errinfo *err);

/** Open connections, open statements, largest fetch batch built. */
extern void ta_sql_stats(int *conns, int *stmts, uint64_t *max_batch);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TA_SQL_H__ */
