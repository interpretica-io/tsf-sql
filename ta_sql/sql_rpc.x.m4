/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief RPC for SQL database sessions
 *
 * The RPCs of rpcs_sql, a thin layer over ta_sql, which keeps database
 * connections, prepared statements and transaction state in the agent's
 * RPC server process and talks to the databases over their own client
 * libraries (libpq, the SQLite C API, MariaDB Connector/C). Add this
 * file to the rpcxdr definitions of the engine platform and of the
 * agent platform:
 *
 *   TE_LIB_PARMS([rpcxdr], [<platform>], [],
 *                [--with-rpcdefs=tarpc_job.x.m4,../ta_sql/sql_rpc.x.m4])
 *
 * A connection and a statement are STATEFUL: sql_connect and
 * sql_prepare return an integer handle and the object lives on the
 * agent between calls, keyed by that handle. The engine never sees a
 * driver object.
 *
 * Values cross the wire as text. A row is a line; cells are separated
 * by a tab; a cell is one type letter followed by its payload:
 *
 *   N           SQL NULL (no payload)
 *   B0 / B1     boolean
 *   I<digits>   signed integer        U<digits>   unsigned integer
 *   F<repr>     floating point (C "%.17g", or inf / -inf / nan)
 *   D<digits>   decimal, exactly as the database prints it
 *   T<text>     text                  J<text>     JSON
 *   M<text>     date / time / timestamp, as the database prints it
 *   X<hex>      binary, two lower-case hex digits a byte
 *
 * In a text payload a backslash, a tab, a newline and a carriage
 * return are written as \\ \t \n \r; nothing else is escaped. A
 * parameter (sql_bind) is one cell in the same form. No float crosses
 * XDR as a float, and a binary value with a NUL in it is carried
 * faithfully.
 *
 * The secret of a connection (sql_connect.in.password) is carried in
 * the call and nowhere else: the agent never logs it, never puts it on
 * a command line, and replaces it with "***" in any driver message it
 * hands back.
 */

/*
 * Wire constants (documentary; te_rpcgen takes no anonymous enums, so
 * these live as named C constants in ta_sql.h on the agent and in
 * tapi_sql.h on the engine, and are kept in step with the values here):
 *
 *   engine (sql_connect.in.engine, a bit of sql_available.out.engines):
 *     1 = SQLite, 2 = PostgreSQL, 4 = MySQL / MariaDB
 *
 *   tls_mode: 0 disable, 1 prefer, 2 require (encrypt, no checks),
 *     3 verify the CA, 4 verify the CA and the host name
 *
 *   capabilities (bitmask): see TA_SQL_CAP_* in ta_sql.h
 *   isolation levels (bitmask, one bit to request): 1 read uncommitted,
 *     2 read committed, 4 repeatable read, 8 serializable; 0 = default
 *
 *   sql_tx.in.op: 1 begin, 2 commit, 3 rollback, 4 savepoint,
 *     5 rollback to savepoint, 6 release savepoint
 *
 *   sql_meta.in.kind: 1 tables, 2 columns, 3 indexes, 4 constraints,
 *     5 foreign keys
 *
 *   sql_set_timeout.in.kind: 1 statement, 2 lock wait
 *
 *   error class (sql_error.out.cls): see TA_SQL_ERR_* in ta_sql.h
 */

/*
 * sql_available(): which drivers are compiled into this agent? retval
 * is 0 when at least one is. engines is the bitmask; versions names the
 * client libraries ("sqlite 3.40.1; libpq 15.6; mariadb 3.3.8").
 */
struct tarpc_sql_available_in {
    struct tarpc_in_arg common;
};

struct tarpc_sql_available_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_uint      engines;
    string          versions<>;   /* "version" is a reserved RPC keyword */
};

/*
 * sql_connect(): open a connection.
 *
 *   engine      one engine bit
 *   host,port   server address (port 0 = the engine's default); unused
 *               by SQLite
 *   dbname      database name, or the file path for SQLite
 *   user        account name
 *   password    the secret; never logged, never echoed
 *   tls_*       TLS mode, CA bundle, client certificate and key paths
 *               on the agent ("" when unused)
 *   timeout_ms  connection establishment deadline (<=0 = driver default)
 *   handle      the connection handle, >= 0 on success
 *   elapsed_us  how long establishing the connection took
 *   err_*       on failure: error class, SQLSTATE, the driver's own
 *               code and its message (the secret redacted)
 */
struct tarpc_sql_connect_in {
    struct tarpc_in_arg common;

    tarpc_int       engine;
    string          host<>;
    tarpc_int       port;
    string          dbname<>;
    string          user<>;
    string          password<>;
    tarpc_int       tls_mode;
    string          tls_ca<>;
    string          tls_cert<>;
    string          tls_key<>;
    tarpc_int       timeout_ms;
};

struct tarpc_sql_connect_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       handle;
    uint64_t        elapsed_us;
    tarpc_int       err_cls;
    tarpc_int       err_native;
    string          err_sqlstate<>;
    string          err_message<>;
};

/* sql_disconnect(): close a connection and every statement on it. */
struct tarpc_sql_disconnect_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_sql_disconnect_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/* sql_ping(): does the server still answer on this connection? */
struct tarpc_sql_ping_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_sql_ping_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    uint64_t        elapsed_us;
};

/*
 * sql_capabilities(): what this driver and this connection can do.
 *   caps            TA_SQL_CAP_* bitmask
 *   isolation       isolation levels the engine accepts (bitmask)
 *   default_iso     the level a transaction gets when none is asked
 *   tls             1 when this connection is encrypted
 *   server          the server's version string
 */
struct tarpc_sql_capabilities_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_sql_capabilities_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_uint      caps;
    tarpc_uint      isolation;
    tarpc_uint      default_iso;
    tarpc_int       tls;
    string          server<>;
};

/*
 * sql_execute(): run one statement, discarding any rows.
 *   affected    rows changed, or -1 when the driver does not say
 */
struct tarpc_sql_execute_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    string          sql<>;
};

struct tarpc_sql_execute_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    int64_t         affected;
    uint64_t        elapsed_us;
};

/*
 * sql_prepare(): prepare a statement. Parameters are written as "?";
 * the PostgreSQL driver renumbers them to $1.. itself.
 *   stmt        the statement handle, >= 0 on success
 *   nparams     how many parameters it takes
 */
struct tarpc_sql_prepare_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    string          sql<>;
};

struct tarpc_sql_prepare_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       stmt;
    tarpc_int       nparams;
};

/*
 * sql_bind(): bind parameter @c index (1-based) to one cell (see the
 * value format above). A binding stays until it is bound again.
 */
struct tarpc_sql_bind_in {
    struct tarpc_in_arg common;

    tarpc_int       stmt;
    tarpc_int       index;
    string          cell<>;
};

struct tarpc_sql_bind_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_stmt_execute(): run a prepared statement with its bindings.
 *   ncols       result columns (0 for a statement with no rows)
 *   columns     one "name \t type-letter" line per column
 *   affected    rows changed, or -1
 * The rows stay on the server / in the driver until sql_fetch asks.
 */
struct tarpc_sql_stmt_execute_in {
    struct tarpc_in_arg common;

    tarpc_int       stmt;
};

struct tarpc_sql_stmt_execute_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       ncols;
    string          columns<>;
    int64_t         affected;
    uint64_t        elapsed_us;
};

/*
 * sql_fetch(): the next batch of rows, bounded twice: at most
 * @c max_rows rows (<=0 = 1000) and, once a row is in, no row that
 * would take the batch past @c max_bytes (<=0 = 1 MiB).
 *   rows        the batch (see the value format above)
 *   nrows       rows in it
 *   eof         1 when the result is exhausted
 */
struct tarpc_sql_fetch_in {
    struct tarpc_in_arg common;

    tarpc_int       stmt;
    tarpc_int       max_rows;
    tarpc_int       max_bytes;
};

struct tarpc_sql_fetch_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    string          rows<>;
    tarpc_int       nrows;
    tarpc_int       eof;
};

/* sql_stmt_free(): release a statement and whatever it still holds. */
struct tarpc_sql_stmt_free_in {
    struct tarpc_in_arg common;

    tarpc_int       stmt;
};

struct tarpc_sql_stmt_free_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_tx(): transaction control.
 *   op          see the constants above
 *   isolation   for begin: one isolation bit, or 0 for the default
 *   name        for the savepoint operations: [A-Za-z0-9_]+
 */
struct tarpc_sql_tx_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       op;
    tarpc_uint      isolation;
    string          name<>;
};

struct tarpc_sql_tx_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_meta(): schema introspection, normalised. The answer is a row
 * set in the value format above; every cell is text or NULL:
 *
 *   tables        name, kind ("table" | "view")
 *   columns       name, position (1-based), type letter, native type,
 *                 nullable ("0"/"1"), default, primary-key ("0"/"1")
 *   indexes       name, unique ("0"/"1"), primary ("0"/"1"),
 *                 columns (comma-separated, in index order)
 *   constraints   name, kind ("primary" | "unique" | "foreign" |
 *                 "check"), columns
 *   foreign keys  name, columns, referenced table, referenced columns,
 *                 on delete, on update (lower case, e.g. "cascade")
 *
 * @c table is ignored for tables and required for the rest. The schema
 * looked at is the connection's current one.
 */
struct tarpc_sql_meta_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       kind;
    string          table<>;
};

struct tarpc_sql_meta_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    string          rows<>;
};

/*
 * sql_set_timeout(): a deadline for what follows on this connection.
 *   kind        1 = a statement's run time, 2 = waiting for a lock
 *   ms          the deadline; 0 switches it off
 * A statement past its deadline fails with TE_ETIMEDOUT.
 */
struct tarpc_sql_set_timeout_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       kind;
    tarpc_int       ms;
};

struct tarpc_sql_set_timeout_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_async_start(): start a statement on this connection and return at
 * once; a thread on the agent runs it. While it runs the connection
 * takes only sql_async_wait and sql_cancel (the rest say TE_EBUSY).
 * This is the synchronisation point of a concurrency test: start the
 * statement that must block, check that it has not finished, release
 * what it waits for, collect it.
 */
struct tarpc_sql_async_start_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    string          sql<>;
};

struct tarpc_sql_async_start_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_async_wait(): wait up to @c timeout_ms for the started statement.
 *   done        1 when it has finished (retval is then its own status)
 *   retval      TE_EINPROGRESS while it still runs
 */
struct tarpc_sql_async_wait_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
    tarpc_int       timeout_ms;
};

struct tarpc_sql_async_wait_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       done;
    int64_t         affected;
    uint64_t        elapsed_us;
};

/* sql_cancel(): ask the server to stop what this connection runs. */
struct tarpc_sql_cancel_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_sql_cancel_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
};

/*
 * sql_error(): the last failure on a connection, normalised.
 *   cls         TA_SQL_ERR_* class
 *   native      the driver's own error code
 *   sqlstate    the five-character SQLSTATE, or "" (SQLite has none)
 *   message     the driver's message, the secret redacted
 */
struct tarpc_sql_error_in {
    struct tarpc_in_arg common;

    tarpc_int       handle;
};

struct tarpc_sql_error_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       cls;
    tarpc_int       native;
    string          sqlstate<>;
    string          message<>;
};

/*
 * sql_stats(): what this RPC server still holds - the oracle of a
 * cleanup test.
 *   conns,stmts     open connections and statements
 *   max_batch       the largest sql_fetch batch built so far, in bytes
 */
struct tarpc_sql_stats_in {
    struct tarpc_in_arg common;
};

struct tarpc_sql_stats_out {
    struct tarpc_out_arg common;

    tarpc_int       retval;
    tarpc_int       conns;
    tarpc_int       stmts;
    uint64_t        max_batch;
};

program sql
{
    version ver0
    {
        RPC_DEF(sql_available)
        RPC_DEF(sql_connect)
        RPC_DEF(sql_disconnect)
        RPC_DEF(sql_ping)
        RPC_DEF(sql_capabilities)
        RPC_DEF(sql_execute)
        RPC_DEF(sql_prepare)
        RPC_DEF(sql_bind)
        RPC_DEF(sql_stmt_execute)
        RPC_DEF(sql_fetch)
        RPC_DEF(sql_stmt_free)
        RPC_DEF(sql_tx)
        RPC_DEF(sql_meta)
        RPC_DEF(sql_set_timeout)
        RPC_DEF(sql_async_start)
        RPC_DEF(sql_async_wait)
        RPC_DEF(sql_cancel)
        RPC_DEF(sql_error)
        RPC_DEF(sql_stats)
    } = 1;
} = 45;
