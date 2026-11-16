/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief SQL TAPI: security posture of a database
 *
 * @defgroup tapi_sql_audit Security checks (tapi_sql_audit)
 * @ingroup tapi_sql
 * @{
 *
 * What a database server looks like to someone who should not be
 * trusted with it: an unencrypted or unverified connection, an account
 * with more privilege than it needs, a server listening on every
 * address, defaults left unsafe, credentials that should be refused and
 * are not, privilege separation that does not separate.
 *
 * The checks are read-only: tapi_sql_audit() only reads server
 * settings and catalogue views. The two checks that try something -
 * tapi_sql_audit_auth() (wrong credentials) and
 * tapi_sql_audit_denied() (a statement the account must not be allowed)
 * - act only on what the test hands them, and the latter runs inside a
 * transaction it rolls back. A test that passes a mutating statement
 * to tapi_sql_audit_denied() has opted into that mutation, controlled
 * by the rollback.
 *
 * SQL injection is a property of an application, not of a database
 * driver, and is tested where the application takes input:
 * tapi_sql_injection_payloads() gives the inputs and
 * tapi_sql_audit_injection() feeds them to a callback of the test that
 * drives the application and says whether it misbehaved.
 *
 * Findings carry a stable check identifier, a subject and a severity,
 * the model of tsf-cybersec; tapi_sql_cybersec.h moves a report into a
 * tapi_cybersec_report for a suite that builds tsf-cybersec. tapi_sql
 * itself does not depend on it.
 *
 * A check that could not look (no privilege to read a catalogue view)
 * says so with a @c *.not-assessed finding rather than passing.
 *
 * Authorized use only: run these against servers you own or are
 * engaged to test.
 */

#ifndef __TAPI_SQL_AUDIT_H__
#define __TAPI_SQL_AUDIT_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#include "tapi_sql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How much a finding matters (the scale of tsf-cybersec). */
typedef enum tapi_sql_severity {
    TAPI_SQL_SEV_INFO = 0,  /**< Worth recording, not acting on. */
    TAPI_SQL_SEV_LOW,       /**< Needs something else to go wrong. */
    TAPI_SQL_SEV_MEDIUM,    /**< Makes an attack easier. */
    TAPI_SQL_SEV_HIGH,      /**< A missing defence. */
    TAPI_SQL_SEV_CRITICAL,  /**< Something is wrong right now. */
} tapi_sql_severity;

/** One thing a check found. */
typedef struct tapi_sql_finding {
    tapi_sql_severity severity;     /**< How much it matters. */
    char             *check;        /**< Stable identifier, e.g.
                                         @c "sql.tls.off". */
    char             *subject;      /**< What it is about; stable. */
    char             *detail;       /**< Free text for the log. */
} tapi_sql_finding;

/** Everything the checks found. */
typedef struct tapi_sql_report {
    tapi_sql_finding *items;    /**< Findings, in the order found. */
    unsigned int      n;        /**< How many. */
} tapi_sql_report;

/** An empty report. */
#define TAPI_SQL_REPORT_INIT { NULL, 0 }

/** Add a finding. */
extern void tapi_sql_report_add(tapi_sql_report *report,
                                tapi_sql_severity severity,
                                const char *check, const char *subject,
                                const char *detail_fmt, ...)
                                TE_LIKE_PRINTF(5, 6);

/** Count the findings of at least a given severity. */
extern unsigned int tapi_sql_report_count(const tapi_sql_report *report,
                                          tapi_sql_severity min_severity);

/** Does the report have a finding of this check? */
extern bool tapi_sql_report_has(const tapi_sql_report *report,
                                const char *check);

/** Log every finding (RING), most severe first. */
extern void tapi_sql_report_log(const tapi_sql_report *report);

/**
 * Build the verdict of a report: the checks that found something at or
 * above @p min_severity, in a stable order.
 *
 * @return @c true when there is such a finding (and @p dest is set).
 */
extern bool tapi_sql_report_verdict(const tapi_sql_report *report,
                                    tapi_sql_severity min_severity,
                                    te_string *dest);

/** Release a report. */
extern void tapi_sql_report_free(tapi_sql_report *report);

/** Spell a severity out. */
extern const char *tapi_sql_severity2str(tapi_sql_severity severity);

/**
 * Read the security posture of the server behind a connection. Only
 * settings and catalogue views are read; nothing is changed.
 *
 * Checks (identifier - what it means):
 * - @c sql.tls.off - this connection is not encrypted (HIGH);
 * - @c sql.tls.unverified - encrypted, but the server's certificate
 *   was not verified (MEDIUM);
 * - @c sql.tls.server-off - the server has TLS switched off (MEDIUM);
 * - @c sql.account.superuser - the account is a superuser / has every
 *   privilege (HIGH);
 * - @c sql.account.excessive - the account can create roles or
 *   databases, read files or grant its privileges on (MEDIUM);
 * - @c sql.net.listen-all - the server listens on every address
 *   (MEDIUM);
 * - @c sql.auth.weak-hash - passwords are stored with a weak hash
 *   (MEDIUM);
 * - @c sql.auth.trust - host-based rules let someone in without a
 *   password (CRITICAL);
 * - @c sql.account.anonymous / @c sql.account.empty-password - an
 *   account anyone can use (HIGH / CRITICAL);
 * - @c sql.file.local-infile / @c sql.file.unrestricted - the server
 *   reads client or server files on request (MEDIUM);
 * - @c sql.log.connections-off - connections are not logged (LOW);
 * - @c sql.sqlite.file - a file database: its protection is the file's
 *   permissions (INFO);
 * - @c *.not-assessed - the account may not read what the check needs
 *   (INFO).
 *
 * @param[in]  conn    Connection.
 * @param[out] report  Findings are added here.
 *
 * @return Status code (0 even when there are findings).
 */
extern te_errno tapi_sql_audit(tapi_sql_conn *conn, tapi_sql_report *report);

/**
 * Authentication failures: credentials that must be refused. Each
 * password is tried for the account of @p cfg; one that is accepted is
 * @c sql.auth.accepted (CRITICAL). A try that failed for a reason other
 * than authentication is @c sql.auth.not-assessed.
 *
 * @param[in]  rpcs       RPC server.
 * @param[in]  cfg        Connection parameters (its password is not
 *                        used).
 * @param[in]  passwords  Passwords that must not work ("" is a fair
 *                        one to try).
 * @param[in]  n          How many.
 * @param[out] report     Findings are added here.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_audit_auth(rcf_rpc_server *rpcs,
                                    const tapi_sql_config *cfg,
                                    const char *const *passwords,
                                    unsigned int n, tapi_sql_report *report);

/**
 * Privilege separation: a statement this account must not be allowed.
 * It is run inside a transaction that is rolled back whatever happens.
 * Allowed is @c sql.privsep.allowed (HIGH); refused for lack of
 * privilege is the expected outcome; failing for another reason is
 * @c sql.privsep.not-assessed.
 *
 * @param[in]  conn    Connection of the restricted account.
 * @param[in]  sql     The statement.
 * @param[in]  what    What it stands for, for the finding's subject
 *                     (e.g. @c "read the salaries table").
 * @param[out] report  Findings are added here.
 *
 * @return Status code.
 */
extern te_errno tapi_sql_audit_denied(tapi_sql_conn *conn, const char *sql,
                                      const char *what,
                                      tapi_sql_report *report);

/**
 * Inputs that break out of a carelessly built statement: quote
 * terminators, tautologies, comment tails, UNION and stacked
 * statements. For feeding to an application's input.
 *
 * @param[out] n  How many.
 *
 * @return The payloads (static).
 */
extern const char *const *tapi_sql_injection_payloads(unsigned int *n);

/**
 * What a test tells about one input: feed @p input to the application
 * and return @c true when the application did what an injected
 * statement would make it do (returned rows it should not have, changed
 * what it should not have, leaked an SQL error).
 */
typedef bool (*tapi_sql_injection_probe)(const char *input, void *opaque);

/**
 * SQL injection through an explicitly supplied application input: every
 * payload is handed to @p probe; each one the application falls for is
 * @c sql.injection (HIGH) with the input's name as subject.
 *
 * @param[in]  input_name  Name of the application input under test.
 * @param[in]  probe       The test's callback.
 * @param[in]  opaque      Its argument.
 * @param[out] report      Findings are added here.
 *
 * @return How many payloads worked.
 */
extern unsigned int tapi_sql_audit_injection(const char *input_name,
                                             tapi_sql_injection_probe probe,
                                             void *opaque,
                                             tapi_sql_report *report);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_SQL_AUDIT_H__ */

/** @} */
