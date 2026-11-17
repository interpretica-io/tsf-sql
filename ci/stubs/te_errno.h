/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief CI stand-in for the Test Environment's te_errno.h
 *
 * The status codes ta_sql returns, as the host's errno values; the
 * module identifier of TE_RC() is dropped.
 */
#ifndef CI_STUB_TE_ERRNO_H
#define CI_STUB_TE_ERRNO_H

#include <errno.h>

typedef int te_errno;

#define TE_TA_UNIX          0
#define TE_RC(_mod, _err)   (_err)
#define TE_RC_GET_ERROR(_rc) (_rc)

#define TE_EINVAL           EINVAL
#define TE_ENOENT           ENOENT
#define TE_EEXIST           EEXIST
#define TE_EDEADLK          EDEADLK
#define TE_EAGAIN           EAGAIN
#define TE_ETIMEDOUT        ETIMEDOUT
#define TE_ECANCELED        ECANCELED
#define TE_ECONNRESET       ECONNRESET
#define TE_EACCES           EACCES
#define TE_EPROTO           EPROTO
#define TE_EPERM            EPERM
#define TE_ECONNREFUSED     ECONNREFUSED
#define TE_EBADF            EBADF
#define TE_EBUSY            EBUSY
#define TE_EMFILE           EMFILE
#define TE_ENOMEM           ENOMEM
#define TE_EOPNOTSUPP       EOPNOTSUPP
#define TE_EINPROGRESS      EINPROGRESS
#define TE_EROFS            EROFS
#define TE_EFAIL            10001

#endif /* !CI_STUB_TE_ERRNO_H */
