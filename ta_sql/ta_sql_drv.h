/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief The seam between ta_sql and a database driver
 *
 * ta_sql.c owns the handles, the value format, transactions,
 * introspection and the asynchronous statements; a driver is one
 * translation unit that maps this table of operations onto a client
 * library. A driver is compiled in only when its library is found, and
 * nothing outside its unit names that library.
 *
 * An operation returns 0 on success and -1 on failure, with the failure
 * described in @c err. A driver never logs and never sees a handle.
 */

#ifndef __TA_SQL_DRV_H__
#define __TA_SQL_DRV_H__

#include <stddef.h>
#include <stdint.h>

#include "te_string.h"

#include "ta_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A database driver: the engine's properties and its operations. */
typedef struct ta_sql_driver {
    int          engine;        /**< TA_SQL_ENGINE_* bit. */
    const char  *name;          /**< "sqlite", "postgres", "mysql". */
    unsigned int caps;          /**< TA_SQL_CAP_* of the engine. */
    unsigned int isolation;     /**< TA_SQL_ISO_* it accepts. */
    unsigned int default_iso;   /**< The level of a plain BEGIN. */

    /** Append the client library version ("libpq 15.6"). */
    void (*version)(te_string *out);

    /** Open a connection; @p dc receives the driver's object. */
    int (*connect)(const ta_sql_config *cfg, void **dc,
                   ta_sql_errinfo *err);
    /** Close a connection. Its statements are already closed. */
    void (*disconnect)(void *dc);
    /** Round trip to the server. */
    int (*ping)(void *dc, ta_sql_errinfo *err);
    /** Append the server version. */
    void (*server_version)(void *dc, te_string *out);
    /** Is the connection encrypted? */
    int (*tls_in_use)(void *dc);

    /** Run a statement, discard rows. */
    int (*exec)(void *dc, const char *sql, int64_t *affected,
                ta_sql_errinfo *err);

    /** Prepare; @p ds receives the driver's statement. */
    int (*prepare)(void *dc, const char *sql, void **ds, int *nparams,
                   ta_sql_errinfo *err);
    /**
     * Bind parameter @p index (0-based). @p tag is the cell's type
     * letter; @p data / @p len its decoded payload (raw bytes for X,
     * unescaped text for the rest, NUL-terminated either way).
     */
    int (*bind)(void *ds, int index, char tag, const char *data,
                size_t len, ta_sql_errinfo *err);
    /** Execute with the current bindings. */
    int (*execute)(void *ds, int *ncols, int64_t *affected,
                   ta_sql_errinfo *err);
    /** Name of result column @p col. */
    const char *(*col_name)(void *ds, int col);
    /** Type letter of result column @p col ('T' when unknown). */
    char (*col_type)(void *ds, int col);
    /** Advance: 1 a row is current, 0 no more rows, -1 failure. */
    int (*step)(void *ds, ta_sql_errinfo *err);
    /** Append the current row's cell @p col, encoded. */
    void (*cell)(void *ds, int col, te_string *out);
    /** Drop the open result, if any; the statement can run again. */
    void (*reset)(void *ds);
    /** Close the statement. */
    void (*finish)(void *ds);

    /** Stop what the connection runs; callable from another thread. */
    int (*cancel)(void *dc);
    /** Set a deadline, see TA_SQL_TIMEOUT_*. */
    int (*set_timeout)(void *dc, int kind, int ms, ta_sql_errinfo *err);

    /** Per-thread set-up and tear-down of the client library. */
    void (*thread_init)(void);
    void (*thread_fini)(void);
} ta_sql_driver;

#ifdef TA_SQL_HAVE_SQLITE
extern const ta_sql_driver ta_sql_sqlite_driver;
#endif
#ifdef TA_SQL_HAVE_POSTGRES
extern const ta_sql_driver ta_sql_pg_driver;
#endif
#ifdef TA_SQL_HAVE_MYSQL
extern const ta_sql_driver ta_sql_mysql_driver;
#endif

/* ---- helpers ta_sql.c gives the drivers --------------------------- */

/** Fill an error. @p sqlstate may be @c NULL. */
extern void ta_sql_err_set(ta_sql_errinfo *err, int cls, int native,
                           const char *sqlstate, const char *fmt, ...)
                           __attribute__((format(printf, 5, 6)));

/** Append a NULL cell. */
extern void ta_sql_cell_null(te_string *out);

/** Append a cell of type @p tag with a text payload, escaped. */
extern void ta_sql_cell_text(te_string *out, char tag, const char *data,
                             size_t len);

/** Append a binary cell. */
extern void ta_sql_cell_bin(te_string *out, const void *data, size_t len);

/** Append a floating-point cell. */
extern void ta_sql_cell_double(te_string *out, double value);

/**
 * The type letter a declared SQL type name maps to: "varchar(20)" is
 * 'T', "bigint unsigned" is 'U', "numeric(10,2)" is 'D' and so on.
 */
extern char ta_sql_type_letter(const char *native);

/** Case-insensitive substring search. */
extern bool ta_sql_has(const char *haystack, const char *needle);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TA_SQL_DRV_H__ */
