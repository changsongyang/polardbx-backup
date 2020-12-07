/*****************************************************************************

Copyright (c) 1996, 2020, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file trx/trx0sys.cc
 Transaction system

 Created 3/26/1996 Heikki Tuuri
 *******************************************************/

#include <sys/types.h>
#include <new>

#include "current_thd.h"
#include "ha_prototypes.h"
#include "mysql/plugin.h"
#include "sql_error.h"
#include "trx0sys.h"

#ifndef UNIV_HOTBACKUP
#include "fsp0fsp.h"
#include "fsp0sysspace.h"
#include "log0log.h"
#include "log0recv.h"
#include "mtr0log.h"
#include "os0file.h"
#include "read0read.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0undo.h"

#include "lizard0read0types.h"

/** The transaction system */
trx_sys_t *trx_sys = nullptr;

/** Check whether transaction id is valid.
@param[in]	id	transaction id to check
@param[in]	name	table name */
void lizard::Vision::check_trx_id_sanity(trx_id_t id,
                                         const table_name_t &name) const {
  if (&name == &dict_sys->dynamic_metadata->name) {
    /* The table mysql.innodb_dynamic_metadata uses a
    constant DB_TRX_ID=~0. */
    ut_ad(id == (1ULL << 48) - 1);
    return;
  }

  if (id >= trx_sys->max_trx_id) {
    ib::warn(ER_IB_MSG_1196)
        << "A transaction id"
        << " in a record of table " << name << " is newer than the"
        << " system-wide maximum.";
    ut_ad(0);
    THD *thd = current_thd;
    if (thd != nullptr) {
      char table_name[MAX_FULL_NAME_LEN + 1];

      innobase_format_name(table_name, sizeof(table_name), name.m_name);

      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SIGNAL_WARN,
                          "InnoDB: Transaction id"
                          " in a record of table"
                          " %s is newer than system-wide"
                          " maximum.",
                          table_name);
    }
  }
}

#ifdef UNIV_DEBUG
/* Flag to control TRX_RSEG_N_SLOTS behavior debugging. */
uint trx_rseg_n_slots_debug = 0;
#endif /* UNIV_DEBUG */

/** Writes the value of max_trx_id to the file based trx system header. */
void trx_sys_flush_max_trx_id(void) {
  mtr_t mtr;
  trx_sysf_t *sys_header;

  ut_ad(trx_sys_mutex_own());

  if (!srv_read_only_mode) {
    mtr_start(&mtr);

    sys_header = trx_sysf_get(&mtr);

    mlog_write_ull(sys_header + TRX_SYS_TRX_ID_STORE, trx_sys->max_trx_id,
                   &mtr);

    mtr_commit(&mtr);
  }
}

void trx_sys_persist_gtid_scn(scn_t gtid_trx_scn) {
  mtr_t mtr;
  mtr.start();
  auto sys_header = trx_sysf_get(&mtr);
  auto page = sys_header - TRX_SYS;
  /* Update GTID transaction number. All transactions with lower
  transaction number are no longer processed for GTID. */
  mlog_write_ull(page + TRX_SYS_TRX_SCN_GTID, gtid_trx_scn, &mtr);
  mtr.commit();
}

void trx_sys_get_binlog_prepared(std::vector<trx_id_t> &trx_ids) {
  trx_sys_mutex_enter();
  /* Exit fast if no prepared transaction. */
  if (trx_sys->n_prepared_trx == 0) {
    trx_sys_mutex_exit();
    return;
  }
  /* Check and find binary log prepared transaction. */
  for (auto trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list); trx != nullptr;
       trx = UT_LIST_GET_NEXT(trx_list, trx)) {
    assert_trx_in_rw_list(trx);
    if (trx_state_eq(trx, TRX_STATE_PREPARED) && trx_is_mysql_xa(trx)) {
      trx_ids.push_back(trx->id);
    }
  }
  trx_sys_mutex_exit();
}

/** Read binary log positions from buffer passed.
@param[in]	binlog_buf	binary log buffer from trx sys page
@param[out]	file_name	binary log file name
@param[out]	high		offset part high order bytes
@param[out]	low		offset part low order bytes
@return	true, if buffer has valid binary log position. */
static bool read_binlog_position(const byte *binlog_buf, const char *&file_name,
                                 uint32_t &high, uint32_t &low) {
  /* Initialize out parameters. */
  file_name = nullptr;
  high = low = 0;

  /* Check if binary log position is stored. */
  if (mach_read_from_4(binlog_buf + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD) !=
      TRX_SYS_MYSQL_LOG_MAGIC_N) {
    return (true);
  }

  /* Read binary log file name. */
  file_name =
      reinterpret_cast<const char *>(binlog_buf + TRX_SYS_MYSQL_LOG_NAME);

  /* read log file offset. */
  high = mach_read_from_4(binlog_buf + TRX_SYS_MYSQL_LOG_OFFSET_HIGH);
  low = mach_read_from_4(binlog_buf + TRX_SYS_MYSQL_LOG_OFFSET_LOW);

  return (false);
}

/** Write binary log position into passed buffer.
@param[in]	file_name	binary log file name
@param[in]	offset		binary log offset
@param[out]	binlog_buf	buffer from trx sys page to write to
@param[in,out]	mtr		mini transaction */
static void write_binlog_position(const char *file_name, uint64_t offset,
                                  byte *binlog_buf, mtr_t *mtr) {
  if (file_name == nullptr ||
      ut_strlen(file_name) >= TRX_SYS_MYSQL_LOG_NAME_LEN) {
    /* We cannot fit the name to the 512 bytes we have reserved */
    return;
  }
  const char *current_name = nullptr;
  uint32_t high = 0;
  uint32_t low = 0;

  auto empty = read_binlog_position(binlog_buf, current_name, high, low);

  if (empty) {
    mlog_write_ulint(binlog_buf + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD,
                     TRX_SYS_MYSQL_LOG_MAGIC_N, MLOG_4BYTES, mtr);
  }

  if (empty || 0 != strcmp(current_name, file_name)) {
    mlog_write_string(binlog_buf + TRX_SYS_MYSQL_LOG_NAME, (byte *)file_name,
                      1 + ut_strlen(file_name), mtr);
  }
  auto in_high = static_cast<ulint>(offset >> 32);
  auto in_low = static_cast<ulint>(offset & 0xFFFFFFFFUL);

  if (empty || high != in_high) {
    mlog_write_ulint(binlog_buf + TRX_SYS_MYSQL_LOG_OFFSET_HIGH, in_high,
                     MLOG_4BYTES, mtr);
  }
  mlog_write_ulint(binlog_buf + TRX_SYS_MYSQL_LOG_OFFSET_LOW, in_low,
                   MLOG_4BYTES, mtr);
}

void trx_sys_read_binlog_position(char *file, uint64_t &offset) {
  const char *current_name = nullptr;
  uint32_t high = 0;
  uint32_t low = 0;

  mtr_t mtr;
  mtr_start(&mtr);

  byte *binlog_pos = trx_sysf_get(&mtr) + TRX_SYS_MYSQL_LOG_INFO;
  auto empty = read_binlog_position(binlog_pos, current_name, high, low);

  if (empty) {
    file[0] = '\0';
    offset = 0;
    mtr_commit(&mtr);
    return;
  }

  strncpy(file, current_name, TRX_SYS_MYSQL_LOG_NAME_LEN);
  offset = static_cast<uint64_t>(high);
  offset = (offset << 32);
  offset |= static_cast<uint64_t>(low);

  mtr_commit(&mtr);
}

/** Check if binary log position is changed.
@param[in]	file_name	previous binary log file name
@param[in]	offset		previous binary log file offset
@param[out]	binlog_buf	buffer from trx sys page to write to
@return true, iff binary log position is modified from previous position. */
static bool binlog_position_changed(const char *file_name, uint64_t offset,
                                    byte *binlog_buf) {
  const char *cur_name = nullptr;
  uint32_t high = 0;
  uint32_t low = 0;
  bool empty = read_binlog_position(binlog_buf, cur_name, high, low);

  if (empty) {
    return (false);
  }

  if (0 != strcmp(cur_name, file_name)) {
    return (true);
  }

  auto cur_offset = static_cast<uint64_t>(high);
  cur_offset = (cur_offset << 32);
  cur_offset |= static_cast<uint64_t>(low);
  return (offset != cur_offset);
}

bool trx_sys_write_binlog_position(const char *last_file, uint64_t last_offset,
                                   const char *file, uint64_t offset) {
  mtr_t mtr;
  mtr_start(&mtr);
  byte *binlog_pos = trx_sysf_get(&mtr) + TRX_SYS_MYSQL_LOG_INFO;

  /* Return If position is already updated. */
  if (binlog_position_changed(last_file, last_offset, binlog_pos)) {
    mtr_commit(&mtr);
    return (false);
  }
  write_binlog_position(file, offset, binlog_pos, &mtr);
  mtr_commit(&mtr);
  return (true);
}

void trx_sys_update_mysql_binlog_offset(trx_t *trx, mtr_t *mtr) {
  trx_sys_update_binlog_position(trx);

  const char *file_name = trx->mysql_log_file_name;
  uint64_t offset = trx->mysql_log_offset;

  /* Reset log file name in transaction. */
  trx->mysql_log_file_name = nullptr;

  byte *binlog_pos = trx_sysf_get(mtr) + TRX_SYS_MYSQL_LOG_INFO;

  if (file_name == nullptr || file_name[0] == '\0') {
    /* Don't write blank name in binary log file position. */
    return;
  }
  write_binlog_position(file_name, offset, binlog_pos, mtr);
}

/** Find the page number in the TRX_SYS page for a given slot/rseg_id
@param[in]	rseg_id		slot number in the TRX_SYS page rseg array
@return page number from the TRX_SYS page rseg array */
page_no_t trx_sysf_rseg_find_page_no(ulint rseg_id) {
  page_no_t page_no;
  mtr_t mtr;
  mtr.start();

  trx_sysf_t *sys_header = trx_sysf_get(&mtr);

  page_no = trx_sysf_rseg_get_page_no(sys_header, rseg_id, &mtr);

  mtr.commit();

  return (page_no);
}

/*****************************************************************/ /**
 Read WSREP XID information from the trx system header if the magic value
 shows it is valid. This code has been copied from MySQL patches by Codership
 with some modifications.
 @return true if the magic value is valid. Otherwise
 return false and leave 'xid' unchanged. */
bool trx_sys_read_wsrep_checkpoint(XID *xid)
/*===================================*/
{
  trx_sysf_t *sys_header;
  mtr_t mtr;
  ulint magic;

  ut_ad(xid);

  mtr_start(&mtr);

  sys_header = trx_sysf_get(&mtr);
  magic = mach_read_from_4(sys_header + TRX_SYS_WSREP_XID_INFO +
                           TRX_SYS_WSREP_XID_MAGIC_N_FLD);

  if (magic != TRX_SYS_WSREP_XID_MAGIC_N) {
    mtr_commit(&mtr);
    return (false);
  }

  xid->set_format_id((long)mach_read_from_4(
      sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_FORMAT));
  xid->set_gtrid_length((long)mach_read_from_4(
      sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_GTRID_LEN));
  xid->set_bqual_length((long)mach_read_from_4(
      sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_BQUAL_LEN));
  xid->set_data(sys_header + TRX_SYS_WSREP_XID_INFO + TRX_SYS_WSREP_XID_DATA,
                XIDDATASIZE);

  mtr_commit(&mtr);

  return (true);
}

/** Look for a free slot for a rollback segment in the trx system file copy.
@param[in,out]	mtr		mtr
@return slot index or ULINT_UNDEFINED if not found */
ulint trx_sysf_rseg_find_free(mtr_t *mtr) {
  trx_sysf_t *sys_header = trx_sysf_get(mtr);

  for (ulint slot_no = 0; slot_no < TRX_SYS_N_RSEGS; slot_no++) {
    page_no_t page_no = trx_sysf_rseg_get_page_no(sys_header, slot_no, mtr);

    if (page_no == FIL_NULL) {
      return (slot_no);
    }
  }

  return (ULINT_UNDEFINED);
}

/** Creates the file page for the transaction system. This function is called
 only at the database creation, before trx_sys_init. */
static void trx_sysf_create(mtr_t *mtr) /*!< in: mtr */
{
  trx_sysf_t *sys_header;
  ulint slot_no;
  buf_block_t *block;
  page_t *page;
  ulint page_no;
  byte *ptr;
  ulint len;

  ut_ad(mtr);

  /* Note that below we first reserve the file space x-latch, and
  then enter the kernel: we must do it in this order to conform
  to the latching order rules. */

  mtr_x_lock_space(fil_space_get_sys_space(), mtr);

  /* Create the trx sys file block in a new allocated file segment */
  block = fseg_create(TRX_SYS_SPACE, 0, TRX_SYS + TRX_SYS_FSEG_HEADER, mtr);
  buf_block_dbg_add_level(block, SYNC_TRX_SYS_HEADER);

  ut_a(block->page.id.page_no() == TRX_SYS_PAGE_NO);

  page = buf_block_get_frame(block);

  mlog_write_ulint(page + FIL_PAGE_TYPE, FIL_PAGE_TYPE_TRX_SYS, MLOG_2BYTES,
                   mtr);

  /* Reset the doublewrite buffer magic number to zero so that we
  know that the doublewrite buffer has not yet been created (this
  suppresses a Valgrind warning) */

  mlog_write_ulint(page + TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC, 0,
                   MLOG_4BYTES, mtr);

  sys_header = trx_sysf_get(mtr);

  /* Start counting transaction ids from number 1 up */
  mach_write_to_8(sys_header + TRX_SYS_TRX_ID_STORE, 1);

  /* Reset the rollback segment slots.  Old versions of InnoDB
  define TRX_SYS_N_RSEGS as 256 (TRX_SYS_OLD_N_RSEGS) and expect
  that the whole array is initialized. */
  ptr = TRX_SYS_RSEGS + sys_header;
  len = ut_max(TRX_SYS_OLD_N_RSEGS, TRX_SYS_N_RSEGS) * TRX_SYS_RSEG_SLOT_SIZE;
  memset(ptr, 0xff, len);
  ptr += len;
  ut_a(ptr <= page + (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END));

  /* Initialize all of the page.  This part used to be uninitialized. */
  memset(ptr, 0, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END + page - ptr);

  mlog_log_string(sys_header,
                  UNIV_PAGE_SIZE - FIL_PAGE_DATA_END + page - sys_header, mtr);

  /* Create the first rollback segment in the SYSTEM tablespace */
  slot_no = trx_sysf_rseg_find_free(mtr);
  page_no = trx_rseg_header_create(TRX_SYS_SPACE, univ_page_size, PAGE_NO_MAX,
                                   slot_no, mtr);

  ut_a(slot_no == TRX_SYS_SYSTEM_RSEG_ID);
  ut_a(page_no == FSP_FIRST_RSEG_PAGE_NO);
}

/** Creates and initializes the central memory structures for the transaction
 system. This is called when the database is started.
 @return min binary heap of rsegs to purge */
lizard::purge_heap_t *trx_sys_init_at_db_start(void) {
  /** Lizard: comment out */
  // purge_pq_t *purge_queue;
  lizard::purge_heap_t *purge_heap;
  trx_sysf_t *sys_header;
  ib_uint64_t rows_to_undo = 0;
  const char *unit = "";

  /* We create the min binary heap here and pass ownership to
  purge when we init the purge sub-system. Purge is responsible
  for freeing the binary heap. */
  purge_heap = UT_NEW_NOKEY(lizard::purge_heap_t());
  ut_a(purge_heap != NULL);

  if (srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {
    /* Create the memory objects for all the rollback segments
    referred to in the TRX_SYS page or any undo tablespace
    RSEG_ARRAY page. */
    trx_rsegs_init(purge_heap);
  }

  /* VERY important: after the database is started, max_trx_id value is
  divisible by TRX_SYS_TRX_ID_WRITE_MARGIN, and the 'if' in
  trx_sys_get_new_trx_id will evaluate to TRUE when the function
  is first time called, and the value for trx id will be written
  to the disk-based header! Thus trx id values will not overlap when
  the database is repeatedly started! */

  mtr_t mtr;
  mtr.start();

  sys_header = trx_sysf_get(&mtr);

  trx_sys->max_trx_id =
      2 * TRX_SYS_TRX_ID_WRITE_MARGIN +
      ut_uint64_align_up(mach_read_from_8(sys_header + TRX_SYS_TRX_ID_STORE),
                         TRX_SYS_TRX_ID_WRITE_MARGIN);

  mtr.commit();

  trx_dummy_sess = sess_open();

  trx_lists_init_at_db_start();

  /* This mutex is not strictly required, it is here only to satisfy
  the debug code (assertions). We are still running in single threaded
  bootstrap mode. */

  trx_sys_mutex_enter();

  if (UT_LIST_GET_LEN(trx_sys->rw_trx_list) > 0) {
    const trx_t *trx;

    for (trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list); trx != nullptr;
         trx = UT_LIST_GET_NEXT(trx_list, trx)) {
      ut_ad(trx->is_recovered);
      assert_trx_in_rw_list(trx);

      if (trx_state_eq(trx, TRX_STATE_ACTIVE)) {
        rows_to_undo += trx->undo_no;
      }
    }

    if (rows_to_undo > 1000000000) {
      unit = "M";
      rows_to_undo = rows_to_undo / 1000000;
    }

    ib::info(ER_IB_MSG_1198)
        << UT_LIST_GET_LEN(trx_sys->rw_trx_list)
        << " transaction(s) which must be rolled back or"
           " cleaned up in total "
        << rows_to_undo << unit << " row operations to undo";

    ib::info(ER_IB_MSG_1199) << "Trx id counter is " << trx_sys->max_trx_id;
  }

  trx_sys->found_prepared_trx = trx_sys->n_prepared_trx > 0;

  trx_sys_mutex_exit();

  return (purge_heap);
}

/** Creates the trx_sys instance and initializes purge_queue and mutex. */
void trx_sys_create(void) {
  ut_ad(trx_sys == nullptr);

  trx_sys = static_cast<trx_sys_t *>(ut_zalloc_nokey(sizeof(*trx_sys)));

  mutex_create(LATCH_ID_TRX_SYS, &trx_sys->mutex);

  // UT_LIST_INIT(trx_sys->serialisation_list, &trx_t::no_list);
  UT_LIST_INIT(trx_sys->rw_trx_list, &trx_t::trx_list);
  UT_LIST_INIT(trx_sys->mysql_trx_list, &trx_t::mysql_trx_list);

  new (&trx_sys->rw_trx_set) TrxIdSet();

  new (&trx_sys->rsegs) Rsegs();
  trx_sys->rsegs.set_empty();

  new (&trx_sys->tmp_rsegs) Rsegs();
  trx_sys->tmp_rsegs.set_empty();
}

/** Creates and initializes the transaction system at the database creation. */
void trx_sys_create_sys_pages(void) {
  mtr_t mtr;

  mtr_start(&mtr);

  trx_sysf_create(&mtr);

  mtr_commit(&mtr);
}

/*********************************************************************
Shutdown/Close the transaction system. */
void trx_sys_close(void) {
  ut_ad(srv_shutdown_state.load() == SRV_SHUTDOWN_EXIT_THREADS);

  if (trx_sys == nullptr) {
    return;
  }

  // ulint size = trx_sys->mvcc->size();
  ulint size = lizard::trx_vision_container_size();

  if (size > 0) {
    ib::error(ER_IB_MSG_1201) << "All read views were not closed before"
                                 " shutdown: "
                              << size << " read views open";
  }

  sess_close(trx_dummy_sess);
  trx_dummy_sess = nullptr;

  trx_purge_sys_close();

  /* Only prepared or active-recovered transactions may be left in the system.
  The active-recovered transactions are allowed only if we did not force to
  rollback them during shutdown (which might happen if e.g. it is fast
  shutdown). Free all of them. */
  trx_sys_after_background_threads_shutdown_validate();

  for (trx_t *trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list); trx != nullptr;
       trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list)) {
    trx_free_prepared_or_active_recovered(trx);
  }

  /* There can't be any active transactions. */
  trx_sys->rsegs.~Rsegs();

  trx_sys->tmp_rsegs.~Rsegs();

  //  UT_DELETE(trx_sys->mvcc);

  ut_a(UT_LIST_GET_LEN(trx_sys->rw_trx_list) == 0);
  ut_a(UT_LIST_GET_LEN(trx_sys->mysql_trx_list) == 0);
  // ut_a(UT_LIST_GET_LEN(trx_sys->serialisation_list) == 0);

  /* We used placement new to create this mutex. Call the destructor. */
  mutex_free(&trx_sys->mutex);

  // trx_sys->rw_trx_ids.~trx_ids_t();

  trx_sys->rw_trx_set.~TrxIdSet();

  ut_free(trx_sys);

  trx_sys = nullptr;
}

void trx_sys_before_pre_dd_shutdown_validate() {
  /** All connections are closed and close_connection unregisters
  associated trx from mysql_trx_list. We still might have some non
  started transactions in mysql_trx_list. */
  trx_sys_mutex_enter();
  for (trx_t *trx = UT_LIST_GET_FIRST(trx_sys->mysql_trx_list); trx != nullptr;
       trx = UT_LIST_GET_NEXT(mysql_trx_list, trx)) {
    ut_a(trx->state == TRX_STATE_NOT_STARTED);
  }
  trx_sys_mutex_exit();
}

void trx_sys_after_pre_dd_shutdown_validate() {
  trx_sys_before_pre_dd_shutdown_validate();

  /** Additionally, the only left transactions are those that have
  state == TRX_STATE_PREPARED, unless we didn't expect to rollback
  all recovered transactions (e.g. fast shutdown) in which case we
  could also have some transactions with is_recovered == true and
  state == TRX_STATE_ACTIVE. */

  const auto active_recovered_trxs = trx_sys_recovered_active_trxs_count();
  if (srv_shutdown_waits_for_rollback_of_recovered_transactions()) {
    ut_a(active_recovered_trxs == 0);
  }

  trx_sys_mutex_enter();
  ut_a(UT_LIST_GET_LEN(trx_sys->rw_trx_list) ==
       trx_sys->n_prepared_trx + active_recovered_trxs);
  trx_sys_mutex_exit();
}

void trx_sys_after_background_threads_shutdown_validate() {
  trx_sys_after_pre_dd_shutdown_validate();

  trx_sys_mutex_enter();
  ut_a(UT_LIST_GET_LEN(trx_sys->mysql_trx_list) == 0);
  trx_sys_mutex_exit();
}

size_t trx_sys_recovered_active_trxs_count() {
  size_t total_trx = 0;
  trx_sys_mutex_enter();
  /* Recovered transactions are never citizens of mysql_trx_list,
  so it's enough to check rw_trx_list. */
  for (trx_t *trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list); trx != nullptr;
       trx = UT_LIST_GET_NEXT(trx_list, trx)) {
    if (trx_state_eq(trx, TRX_STATE_ACTIVE) && trx->is_recovered) {
      total_trx++;
    }
  }
  trx_sys_mutex_exit();
  return (total_trx);
}

#ifdef UNIV_DEBUG
/** Validate the trx_ut_list_t.
 @return true if valid. */
static bool trx_sys_validate_trx_list_low(
    trx_ut_list_t *trx_list) /*!< in: &trx_sys->rw_trx_list */
{
  const trx_t *trx;
  const trx_t *prev_trx = nullptr;

  ut_ad(trx_sys_mutex_own());

  ut_ad(trx_list == &trx_sys->rw_trx_list);

  for (trx = UT_LIST_GET_FIRST(*trx_list); trx != nullptr;
       prev_trx = trx, trx = UT_LIST_GET_NEXT(trx_list, prev_trx)) {
    check_trx_state(trx);
    ut_a(prev_trx == nullptr || prev_trx->id > trx->id);
  }

  return (true);
}

/** Validate the trx_sys_t::rw_trx_list.
 @return true if the list is valid. */
bool trx_sys_validate_trx_list() {
  ut_ad(trx_sys_mutex_own());

  ut_a(trx_sys_validate_trx_list_low(&trx_sys->rw_trx_list));

  return (true);
}
#endif /* UNIV_DEBUG */
#endif /* !UNIV_HOTBACKUP */

/** A list of undo tablespace IDs found in the TRX_SYS page. These are the
old type of undo tablespaces that do not have space_IDs in the reserved
range nor contain an RSEG_ARRAY page. This cannot be part of the trx_sys_t
object because it must be built before that is initialized. */
Space_Ids *trx_sys_undo_spaces;

/** Initialize trx_sys_undo_spaces, called once during srv_start(). */
void trx_sys_undo_spaces_init() {
  trx_sys_undo_spaces = UT_NEW(Space_Ids(), mem_key_undo_spaces);

  trx_sys_undo_spaces->reserve(TRX_SYS_N_RSEGS);
}

/** Free the resources occupied by trx_sys_undo_spaces,
called once during thread de-initialization. */
void trx_sys_undo_spaces_deinit() {
  if (trx_sys_undo_spaces != nullptr) {
    trx_sys_undo_spaces->clear();
    UT_DELETE(trx_sys_undo_spaces);
    trx_sys_undo_spaces = nullptr;
  }
}
