/*****************************************************************************

Copyright (c) 2013, 2020, Alibaba and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
lzeusited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the zeusplied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/lizard0scn.h
 Lizard scn number implementation.

 Created 2020-03-23 by Jianwei.zhao
 *******************************************************/

#ifndef lizard0scn_h
#define lizard0scn_h

#include "lizard0scn0types.h"
#include "ut0mutex.h"

#ifdef UNIV_PFS_MUTEX
/* lizard scn mutex PFS key */
extern mysql_pfs_key_t lizard_scn_mutex_key;
#endif

/** The number gap of persist scn number into system tablespace */
#define LIZARD_SCN_NUMBER_MAGIN 1024

namespace lizard {

/** Invalid scn number was defined as the max value of ulint */
constexpr scn_t SCN_NULL = std::numeric_limits<scn_t>::max();

/** The max of scn number, crash direct if more than SCN_MAX */
constexpr scn_t SCN_MAX = std::numeric_limits<scn_t>::max() - 1;

/** The minimus and valid scn number */
constexpr scn_t SCN_FAKE = 1;

/** Invalid time 1970-01-01 00:00:00 +0000 (UTC) */
constexpr utc_t UTC_NULL = std::numeric_limits<utc_t>::min();

/** The max local time is less than 2038 year */
constexpr utc_t UTC_MAX = std::numeric_limits<std::int32_t>::max() * 1000000ULL;

/* The structure of scn number generation */
class SCN {
 public:
  SCN();
  virtual ~SCN();

  /** Assign the init value by reading from lizard tablespace */
  void init();

  /** Calculate a new scn number
  @return     scn */
  scn_t new_scn();

  /** Calculate a new scn number and consistent UTC time
  @return   <SCN, UTC> */
  commit_scn_t new_commit_scn();

  /** Get m_scn
  @return     m_scn */
  scn_t acquire_scn();

 private:
  /** Flush the scn number to system tablepace every LIZARD_SCN_NUMBER_MAGIN */
  void flush_scn();

  /** Disable the copy and assign function */
  SCN(const SCN &) = delete;
  SCN(const SCN &&) = delete;
  SCN &operator=(const SCN &) = delete;

 private:
  scn_t m_scn;
  bool m_inited;
  ib_mutex_t m_mutex;
};

/**
  Check the commit scn state

  @param[in]    scn       commit scn
  @return       scn state SCN_STATE_INITIAL, SCN_STATE_ALLOCATED or
                          SCN_STATE_INVALID
*/
enum scn_state_t zeus_commit_scn_state(const commit_scn_t &scn);

}  // namespace lizard

/** Commit scn initial value */
#define COMMIT_SCN_NULL \
  { lizard::SCN_NULL, lizard::UTC_NULL }

#endif  /* lizard0scn_h define */
