/*****************************************************************************

Copyright (c) 1996, 2020, Oracle and/or its affiliates.

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

/** @file trx/trx0purge.cc
 Purge old versions

 Created 3/26/1996 Heikki Tuuri
 *******************************************************/

#include <sys/types.h>
#include <new>

#include "clone0api.h"
#include "clone0clone.h"
#include "dict0dd.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fsp0sysspace.h"
#include "fsp0types.h"
#include "fut0fut.h"
#include "ha_prototypes.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "os0thread.h"
#include "que0que.h"
#include "read0read.h"
#include "row0purge.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0sync.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"

#include "lizard0sys.h"
#include "lizard0txn.h"
#include "lizard0undo.h"
#include "lizard0purge.h"

/** Maximum allowable purge history length.  <=0 means 'infinite'. */
ulong srv_max_purge_lag = 0;

/** Max DML user threads delay in micro-seconds. */
ulong srv_max_purge_lag_delay = 0;

/** The global data structure coordinating a purge */
trx_purge_t *purge_sys = nullptr;

/** Wait for a short delay between checks. */
static constexpr int64_t PURGE_CHECK_UNDO_TRUNCATE_DELAY_IN_MS = 1000;

#ifdef UNIV_DEBUG
bool srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

/** A sentinel undo record used as a return value when we have a whole
undo log which can be skipped by purge */
static trx_undo_rec_t trx_purge_ignore_rec;

/** lizard: a sentinel undo record to identify the BLOCKED record */
static trx_undo_rec_t trx_purge_blocked_rec;

/** Builds a purge 'query' graph. The actual purge is performed by executing
this query graph.
@param[in]   trx               transaction
@param[in]   n_purge_threads   number of purge threads
@return own: the query graph */
static que_t *trx_purge_graph_build(trx_t *trx, ulint n_purge_threads) {
  ulint i;
  mem_heap_t *heap;
  que_fork_t *fork;

  heap = mem_heap_create(512);
  fork = que_fork_create(nullptr, nullptr, QUE_FORK_PURGE, heap);
  fork->trx = trx;

  for (i = 0; i < n_purge_threads; ++i) {
    que_thr_t *thr;

    thr = que_thr_create(fork, heap, nullptr);

    thr->child = row_purge_node_create(thr, heap);
  }

  return (fork);
}

void trx_purge_sys_create(ulint n_purge_threads,
                          lizard::purge_heap_t *purge_heap) {
  purge_sys = static_cast<trx_purge_t *>(ut_zalloc_nokey(sizeof(*purge_sys)));

  purge_sys->state = PURGE_STATE_INIT;
  purge_sys->event = os_event_create();

  new (&purge_sys->iter) purge_iter_t;
  new (&purge_sys->limit) purge_iter_t;
  new (&purge_sys->undo_trunc) undo::Truncate;
#ifdef UNIV_DEBUG
  new (&purge_sys->done) purge_iter_t;
#endif /* UNIV_DEBUG */


  /* Take ownership of purge_heap, we are responsible for freeing it. */
  purge_sys->purge_heap = purge_heap;

  rw_lock_create(trx_purge_latch_key, &purge_sys->latch, SYNC_PURGE_LATCH);

  mutex_create(LATCH_ID_PURGE_SYS_PQ, &purge_sys->pq_mutex);

  ut_a(n_purge_threads > 0);

  purge_sys->sess = sess_open();

  purge_sys->trx = purge_sys->sess->trx;

  ut_a(purge_sys->trx->sess == purge_sys->sess);

  /* A purge transaction is not a real transaction, we use a transaction
  here only because the query threads code requires it. It is otherwise
  quite unnecessary. We should get rid of it eventually. */
  purge_sys->trx->id = 0;
  purge_sys->trx->start_time = ut_time();
  purge_sys->trx->state = TRX_STATE_ACTIVE;
  purge_sys->trx->op_info = "purge trx";

  purge_sys->query = trx_purge_graph_build(purge_sys->trx, n_purge_threads);

  new (&purge_sys->vision) lizard::Vision();

  lizard::trx_clone_oldest_vision(&purge_sys->vision);

  purge_sys->view_active = true;

  purge_sys->rseg_iter = UT_NEW_NOKEY(lizard::TxnUndoRsegsIterator(purge_sys));

  /* Allocate 8K bytes for the initial heap. */
  purge_sys->heap = mem_heap_create(8 * 1024);

  /* reload purged_scn from heap */
  lizard::trx_purge_set_purged_scn(lizard::trx_purge_reload_purged_scn());

  /* undo retention */
  purge_sys->top_undo_utc = 0;

  new (&purge_sys->purged_gcn) Purged_gcn;
  purge_sys->purged_gcn.init();
}

void trx_purge_sys_close() {
  que_graph_free(purge_sys->query);

  ut_a(purge_sys->trx->id == 0);
  ut_a(purge_sys->sess->trx == purge_sys->trx);

  purge_sys->trx->state = TRX_STATE_NOT_STARTED;

  sess_close(purge_sys->sess);

  purge_sys->sess = nullptr;

  rw_lock_free(&purge_sys->latch);
  mutex_free(&purge_sys->pq_mutex);

  if (purge_sys->purge_heap != NULL) {
    UT_DELETE(purge_sys->purge_heap);
    purge_sys->purge_heap = NULL;
  }

  os_event_destroy(purge_sys->event);

  purge_sys->event = nullptr;

  mem_heap_free(purge_sys->heap);

  purge_sys->heap = nullptr;

  UT_DELETE(purge_sys->rseg_iter);

  purge_sys->purged_gcn.~Purged_gcn();

  ut_free(purge_sys);

  purge_sys = nullptr;
}

/*================ UNDO LOG HISTORY LIST =============================*/

/** Adds the update undo log as the first log in the history list. Removes the
 update undo log segment from the rseg slot if it is too big for reuse. */
void trx_purge_add_update_undo_to_history(
    trx_t *trx,               /*!< in: transaction */
    trx_undo_ptr_t *undo_ptr, /*!< in/out: update undo log. */
    page_t *undo_page,        /*!< in: update undo log header page,
                              x-latched */
    bool update_rseg_history_len,
    /*!< in: if true: update rseg history
    len else skip updating it. */
    ulint n_added_logs, /*!< in: number of logs added */
    mtr_t *mtr)         /*!< in: mtr */
{
  trx_undo_t *undo;
  trx_rseg_t *rseg;
  trx_rsegf_t *rseg_header;
  trx_ulogf_t *undo_header;

  undo = undo_ptr->update_undo;
  rseg = undo->rseg;

  rseg_header = trx_rsegf_get(undo->rseg->space_id, undo->rseg->page_no,
                              undo->rseg->page_size, mtr);

  undo_header = undo_page + undo->hdr_offset;

  lizard_trx_undo_hdr_uba_validation(undo_header, mtr);

  if (undo->state != TRX_UNDO_CACHED) {
    ulint hist_size;
#ifdef UNIV_DEBUG
    trx_usegf_t *seg_header = undo_page + TRX_UNDO_SEG_HDR;
#endif /* UNIV_DEBUG */

    /* The undo log segment will not be reused */

    if (UNIV_UNLIKELY(undo->id >= TRX_RSEG_N_SLOTS)) {
      ib::fatal(ER_IB_MSG_1165) << "undo->id is " << undo->id;
    }

    trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL, mtr);

    MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);

    hist_size =
        mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr);

    ut_ad(undo->size == flst_get_len(seg_header + TRX_UNDO_PAGE_LIST));

    mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE,
                     hist_size + undo->size, MLOG_4BYTES, mtr);
  }

  /* Add the log as the first in the history list */
  flst_add_first(rseg_header + TRX_RSEG_HISTORY,
                 undo_header + TRX_UNDO_HISTORY_NODE, mtr);

  if (update_rseg_history_len) {
    os_atomic_increment_ulint(&trx_sys->rseg_history_len, n_added_logs);
    if (trx_sys->rseg_history_len >
        srv_n_purge_threads * srv_purge_batch_size) {
      srv_wake_purge_thread_if_not_active();
    }
  }

  /* Update maximum transaction scn for this rollback segment. */
  assert_trx_scn_allocated(trx);
  mlog_write_ull(rseg_header + TRX_RSEG_MAX_TRX_SCN, trx->txn_desc.cmmt.scn,
                 mtr);

  /* lizard: TRX_UNDO_TRX_NO is reserved */
  //mlog_write_ull(undo_header + TRX_UNDO_TRX_NO, trx->no, mtr);

  /* Write information about delete markings to the undo log header */

  if (!undo->del_marks) {
    mlog_write_ulint(undo_header + TRX_UNDO_DEL_MARKS, FALSE, MLOG_2BYTES, mtr);
  }

  /* Write GTID information if there. */
  trx_undo_gtid_write(trx, undo_header, undo, mtr, false);

  if (rseg->last_page_no == FIL_NULL) {
    rseg->last_page_no = undo->hdr_page_no;
    rseg->last_offset = undo->hdr_offset;
    rseg->last_del_marks = undo->del_marks;
    rseg->last_scn = trx->txn_desc.cmmt.scn;
  }
}

/** Remove an rseg header from the history list.
@param[in,out]	rseg_hdr	rollback segment header
@param[in]	log_hdr		undo log segment header
@param[in,out]	mtr		mini transaction. */
void trx_purge_remove_log_hdr(trx_rsegf_t *rseg_hdr, trx_ulogf_t *log_hdr,
                              mtr_t *mtr) {
  flst_remove(rseg_hdr + TRX_RSEG_HISTORY, log_hdr + TRX_UNDO_HISTORY_NODE,
              mtr);

  os_atomic_decrement_ulint(&trx_sys->rseg_history_len, 1);
}

/** Frees a rollback segment which is in the history list.
Removes the rseg hdr from the history list.
@param[in,out]	rseg		rollback segment
@param[in]	hdr_addr	file address of log_hdr
@param[in]	noredo		skip redo logging. */
static void trx_purge_free_segment(trx_rseg_t *rseg, fil_addr_t hdr_addr,
                                   bool noredo) {
  mtr_t mtr;
  trx_rsegf_t *rseg_hdr;
  trx_ulogf_t *log_hdr;
  trx_usegf_t *seg_hdr;
  ulint seg_size;
  ulint hist_size;
  bool marked = noredo;

  for (;;) {
    page_t *undo_page;

    mtr_start(&mtr);

    if (noredo) {
      mtr.set_log_mode(MTR_LOG_NO_REDO);
    }

    rseg->latch();

    rseg_hdr =
        trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);

    undo_page = trx_undo_page_get(page_id_t(rseg->space_id, hdr_addr.page),
                                  rseg->page_size, &mtr);

    seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
    log_hdr = undo_page + hdr_addr.boffset;

    /* Mark the last undo log totally purged, so that if the
    system crashes, the tail of the undo log will not get accessed
    again. The list of pages in the undo log tail gets inconsistent
    during the freeing of the segment, and therefore purge should
    not try to access them again. */

    if (!marked) {
      marked = true;
      mlog_write_ulint(log_hdr + TRX_UNDO_DEL_MARKS, FALSE, MLOG_2BYTES, &mtr);
    }

    if (fseg_free_step_not_header(seg_hdr + TRX_UNDO_FSEG_HEADER, false,
                                  &mtr)) {
      break;
    }

    rseg->unlatch();

    mtr_commit(&mtr);
  }

  /* The page list may now be inconsistent, but the length field
  stored in the list base node tells us how big it was before we
  started the freeing. */

  seg_size = flst_get_len(seg_hdr + TRX_UNDO_PAGE_LIST);

  /* We may free the undo log segment header page; it must be freed
  within the same mtr as the undo log header is removed from the
  history list: otherwise, in case of a database crash, the segment
  could become inaccessible garbage in the file space. */

  trx_purge_remove_log_hdr(rseg_hdr, log_hdr, &mtr);

  do {
    /* Here we assume that a file segment with just the header
    page can be freed in a few steps, so that the buffer pool
    is not flooded with bufferfixed pages: see the note in
    fsp0fsp.cc. */

  } while (!fseg_free_step(seg_hdr + TRX_UNDO_FSEG_HEADER, false, &mtr));

  hist_size =
      mtr_read_ulint(rseg_hdr + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, &mtr);
  ut_ad(hist_size >= seg_size);

  mlog_write_ulint(rseg_hdr + TRX_RSEG_HISTORY_SIZE, hist_size - seg_size,
                   MLOG_4BYTES, &mtr);

  rseg->decr_curr_size(seg_size);
  rseg->unlatch();
  mtr_commit(&mtr);
}

/** Removes unnecessary history data from a rollback segment. */
static void trx_purge_truncate_rseg_history(
    trx_rseg_t *rseg,          /*!< in: rollback segment */
    const purge_iter_t *limit) /*!< in: truncate offset */
{
  fil_addr_t hdr_addr;
  fil_addr_t prev_hdr_addr;
  trx_rsegf_t *rseg_hdr;
  page_t *undo_page;
  trx_ulogf_t *log_hdr;
  trx_usegf_t *seg_hdr;
  mtr_t mtr;
  scn_t undo_trx_scn;
  const bool is_temp = fsp_is_system_temporary(rseg->space_id);

  mtr_start(&mtr);

  if (is_temp) {
    mtr.set_log_mode(MTR_LOG_NO_REDO);
  }

  rseg->latch();

  /** Lizard: It's possible limit->scn > rseg->last_scn */
  if(rseg->last_page_no != FIL_NULL && limit->scn > rseg->last_scn) {
    rseg->unlatch();
    return;
  }

  rseg_hdr =
      trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);

  hdr_addr = trx_purge_get_log_from_hist(
      flst_get_last(rseg_hdr + TRX_RSEG_HISTORY, &mtr));
loop:
  if (hdr_addr.page == FIL_NULL) {
    rseg->unlatch();
    mtr_commit(&mtr);

    return;
  }

  undo_page = trx_undo_page_get(page_id_t(rseg->space_id, hdr_addr.page),
                                rseg->page_size, &mtr);

  log_hdr = undo_page + hdr_addr.boffset;

  undo_trx_scn = lizard::trx_undo_hdr_read_scn(log_hdr, &mtr).scn;

  ut_ad(lizard::lizard_sys &&
        undo_trx_scn <= lizard::lizard_sys->scn.acquire_scn());

  if (undo_trx_scn >= limit->scn) {
    /* limit space_id should match the rollback segment
    space id to avoid freeing if the page belongs to a
    different rollback segment for the same trx_no. */
    if (undo_trx_scn == limit->scn &&
        rseg->space_id == limit->undo_rseg_space) {
      trx_undo_truncate_start(rseg, hdr_addr.page, hdr_addr.boffset,
                              limit->undo_no);
    }

    rseg->unlatch();
    mtr_commit(&mtr);

    return;
  }

  prev_hdr_addr = trx_purge_get_log_from_hist(
      flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

  seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

  if ((mach_read_from_2(seg_hdr + TRX_UNDO_STATE) == TRX_UNDO_TO_PURGE) &&
      (mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG) == 0)) {
    /* We can free the whole log segment */

    rseg->unlatch();
    mtr_commit(&mtr);

    /** Lizard: Put txn undo log segment to free list after purge */
    if (lizard::fsp_is_txn_tablespace_by_id(rseg->space_id)) {
      ut_ad(!is_temp);
      lizard::txn_purge_segment_to_free_list(rseg, hdr_addr);
    } else {
      /* calls the trx_purge_remove_log_hdr()
      inside trx_purge_free_segment(). */
      trx_purge_free_segment(rseg, hdr_addr, is_temp);
    }

  } else {
    /* Remove the log hdr from the rseg history. */

    trx_purge_remove_log_hdr(rseg_hdr, log_hdr, &mtr);

    rseg->unlatch();
    mtr_commit(&mtr);
  }

  mtr_start(&mtr);

  if (is_temp) {
    mtr.set_log_mode(MTR_LOG_NO_REDO);
  }

  rseg->latch();

  rseg_hdr =
      trx_rsegf_get(rseg->space_id, rseg->page_no, rseg->page_size, &mtr);

  hdr_addr = prev_hdr_addr;

  goto loop;
}

namespace undo {

/** Mutext for serializing undo tablespace related DDL.  These have to do with
creating and dropping undo tablespaces. */
ib_mutex_t ddl_mutex;

/** A global object that contains a vector of undo::Tablespace structs. */
Tablespaces *spaces;

/** List of currently used undo space IDs for each undo space number
along with a boolean showing whether the undo space number is in use. */
struct space_id_account *space_id_bank;

/** Initialize the undo tablespace space_id bank which is a lock free
repository for information about the space IDs used for undo tablespaces.
It is used during creation in order to assign an unused space number and
during truncation in order to assign the next space_id within that
space_number range.
It is initialized with the minimum value in the range so that if a new
space ID is needed in that range the max space ID will be used first.
As truncation occurs, the space_ids are assigned from max down to min. */
void init_space_id_bank() {
  space_id_bank = UT_NEW_ARRAY(struct space_id_account,
                               FSP_MAX_UNDO_TABLESPACES, mem_key_undo_spaces);

  for (size_t slot = 0; slot < FSP_MAX_UNDO_TABLESPACES; slot++) {
    undo::space_id_bank[slot].space_id = SPACE_UNKNOWN;
    undo::space_id_bank[slot].in_use = false;
  }
}

/** Note that the undo space number for a space ID is being used.
Put that space_id into the space_id_bank.
@param[in] space_id  undo tablespace number */
void use_space_id(space_id_t space_id) {
  size_t slot = undo::id2num(space_id) - 1;

  ut_ad(!space_id_bank[slot].in_use);

  space_id_bank[slot].space_id = space_id;
  space_id_bank[slot].in_use = true;
}

/** Mark an undo number associated with a given space_id as unused and
available to be resused.  This happens when the fil_space_t is closed
associated with a drop undo tablespace.
@param[in] space_id  Undo Tablespace ID */
void unuse_space_id(space_id_t space_id) {
  ut_ad(fsp_is_undo_tablespace(space_id));

  space_id_t space_num = undo::id2num(space_id);
  size_t slot = space_num - 1;

  space_id_bank[slot].in_use = false;
}

/** Given a valid undo space_id or SPACE_UNKNOWN, return the next space_id
for the given space number.
@param[in]  space_id   undo tablespace ID
@param[in]  space_num  undo tablespace number
@return the next tablespace ID to use */
space_id_t next_space_id(space_id_t space_id, space_id_t space_num) {
  ut_ad(space_id == SPACE_UNKNOWN || fsp_is_undo_tablespace(space_id));
  ut_ad(space_id != SPACE_UNKNOWN ||
        (space_num > 0 && space_num <= FSP_MAX_UNDO_TABLESPACES));

  space_id_t first_id = dict_sys_t::s_max_undo_space_id + 1 - space_num;
  space_id_t last_id = first_id - (FSP_MAX_UNDO_TABLESPACES *
                                   (dict_sys_t::undo_space_id_range - 1));
  return (space_id == SPACE_UNKNOWN || space_id == last_id
              ? first_id
              : space_id - FSP_MAX_UNDO_TABLESPACES);
}

/** Given a  valid undo space_id, return the next space_id for that
space number.
@param[in]  space_id  undo tablespace ID
@return the next tablespace ID to use */
space_id_t next_space_id(space_id_t space_id) {
  ut_ad(space_id != SPACE_UNKNOWN);
  ut_ad(fsp_is_undo_tablespace(space_id));

  space_id_t space_num = undo::id2num(space_id);

  return (next_space_id(space_id, space_num));
}

/** Mark that the given undo space number is being used and
return the next available space_id for that space number.
@param[in]  space_num  undo tablespace number
@return the next tablespace ID to use */
space_id_t use_next_space_id(space_id_t space_num) {
  size_t slot = space_num - 1;

  ut_ad(!space_id_bank[slot].in_use);

  space_id_t cur_id = space_id_bank[slot].space_id;
  space_id_t next_id = next_space_id(cur_id, space_num);

  space_id_bank[slot].space_id = next_id;
  space_id_bank[slot].in_use = true;

  return (next_id);
}

/** Return the next available undo space number to be used for a new
explicit undo tablespace. The slot will be marked as in-use.
@retval if success, next available undo space number.
@retval if failure, SPACE_UNKNOWN */
space_id_t get_next_available_space_num() {
  for (space_id_t slot = FSP_IMPLICIT_UNDO_TABLESPACES;
       slot < FSP_MAX_UNDO_TABLESPACES; ++slot) {
    space_id_t space_num = slot + 1;

    if (!space_id_bank[slot].in_use) {
      return (use_next_space_id(space_num));
    }
    /* Slot is in use.  Try the next slot. */
  }

  return (SPACE_UNKNOWN);
}

bool Tablespace::needs_truncation() {
  /* If it is already inactive, even implicitly, then proceed. */
  m_rsegs->s_lock();
  if (m_rsegs->is_inactive_implicit() || m_rsegs->is_inactive_explicit()) {
    m_rsegs->s_unlock();
    return (true);
  }

  /* If implicit undo truncation is turned off, or if the rsegs don't exist
  yet, don't bother checking the size. */
  if (!srv_undo_log_truncate || m_rsegs == nullptr || m_rsegs->is_empty() ||
      m_rsegs->is_init()) {
    m_rsegs->s_unlock();
    return (false);
  }
  ut_ad(m_rsegs->is_active());
  m_rsegs->s_unlock();

  /* Check if undo truncation is happening so often that too many pages
  from old space IDs are still in memory. Since undo spaces are deleted
  with BUF_REMOVE_NONE, the actual space is not deleted for that old
  space ID until all pages have been passively removed from the buffer
  pool. */
  auto count = fil_count_deleted(undo::id2num(m_id));
  if (count > CONCURRENT_UNDO_TRUNCATE_LIMIT) {
    ib::warn(ER_IB_MSG_UNDO_TRUNCATE_TOO_OFTEN);
    return (false);
  }

  page_no_t trunc_size = ut_max(
      static_cast<page_no_t>(srv_max_undo_tablespace_size / srv_page_size),
      static_cast<page_no_t>(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES));

  if (fil_space_get_size(id()) > trunc_size) {
    return (true);
  }

  return (false);
}

/** Change the space_id from its current value.
@param[in]  space_id  The new undo tablespace ID */
void Tablespace::set_space_id(space_id_t space_id) {
  ut_ad(m_num == id2num(space_id));
  m_id = space_id;
}

/** Build a standard undo tablespace name from a space_id.
@param[in]	space_id	id of the undo tablespace.
@return tablespace name of the undo tablespace file */
char *make_space_name(space_id_t space_id) {
  /* 8.0 undo tablespace names have an extra '_' */
  bool old = (id2num(space_id) == space_id);

  size_t size = sizeof(undo_space_name) + 3 + (old ? 0 : 1);

  char *name = static_cast<char *>(ut_malloc_nokey(size));

  snprintf(name, size, (old ? "%s%03" SPACE_ID_PFS : "%s_%03" SPACE_ID_PFS),
           undo_space_name, static_cast<unsigned>(id2num(space_id)));

  return (name);
}

/** Build a standard undo tablespace file name from a space_id.
This will create a name like 'undo_001' if the space_id is in the
reserved range, else it will be like 'undo001'.
@param[in]	space_id	id of the undo tablespace.
@return file_name of the undo tablespace file */
char *make_file_name(space_id_t space_id) {
  /* 8.0 undo tablespace names have an extra '_' */
  size_t len = strlen(srv_undo_dir);
  bool with_sep = (srv_undo_dir[len - 1] == OS_PATH_SEPARATOR);
  bool old = (id2num(space_id) == space_id);

  size_t size = strlen(srv_undo_dir) + (with_sep ? 0 : 1) + sizeof("undo000") +
                (old ? 0 : 1);

  char *name = static_cast<char *>(ut_malloc_nokey(size));

  memcpy(name, srv_undo_dir, len);

  if (!with_sep) {
    name[len++] = OS_PATH_SEPARATOR;
  }

  memcpy(&name[len], "undo", 4);
  len += 4;

  if (!old) {
    name[len++] = '_';
  }

  snprintf(&name[len], size - len, "%03" SPACE_ID_PFS,
           static_cast<unsigned>(id2num(space_id)));

  return (name);
}

void Tablespace::set_space_name(const char *new_space_name) {
  if (m_space_name != nullptr) {
    ut_free(m_space_name);
    m_space_name = nullptr;
  }

  size_t size = strlen(new_space_name) + 1;
  m_space_name = static_cast<char *>(ut_malloc_nokey(size));

  strncpy(m_space_name, new_space_name, size);
}

void Tablespace::set_file_name(const char *file_name) {
  /* Make a copy of the filename and normalize it. */
  char norm_fn[FN_REFLEN];
  strncpy(norm_fn, file_name, FN_REFLEN - 1);
  Fil_path::normalize(norm_fn);
  std::string tmp_fn{norm_fn};

  /* Explicit undo tablespaces use an IBU extension. */
  m_implicit = (Fil_path::has_suffix(IBU, tmp_fn) ? false : true);

  /* This name can come in three forms: absolute path, relative path,
  and basename. ADD DATAFILE for undo tablespaces does not accept a
  relative path. If a relative path comes in here, it was the scanned
  name and is relative to the datadir. So only prepend the undo_dir if
  this is just a basename. */
  std::string final_fn;
  if (tmp_fn.find_first_of(":/\\") == std::string::npos) {
    /* Prepend the undo directory. */
    bool is_circ = MySQL_undo_path.is_circular();
    final_fn += (is_circ ? MySQL_undo_path.abs_path() : MySQL_undo_path.path());
    char back = (is_circ ? MySQL_undo_path.abs_path().back()
                         : MySQL_undo_path.path().back());
    final_fn += (back == OS_PATH_SEPARATOR ? "" : OS_PATH_SEPARATOR_STR);
  }
  final_fn += tmp_fn;

  /* We are going to replace any existing m_file_name. */
  if (m_file_name != nullptr) {
    ut_free(m_file_name);
  }

  size_t len = final_fn.size();
  m_file_name = static_cast<char *>(ut_malloc_nokey(len + 1));
  memcpy(m_file_name, final_fn.c_str(), len);
  m_file_name[len] = '\0';
}

/** Populate log file name based on space_id
@param[in]	space_id	id of the undo tablespace.
@return DB_SUCCESS or error code */
char *Tablespace::make_log_file_name(space_id_t space_id) {
  size_t size = strlen(srv_log_group_home_dir) + 22 + 1 /* NUL */
                + strlen(undo::s_log_prefix) + strlen(undo::s_log_ext);

  char *name = static_cast<char *>(ut_malloc_nokey(size));

  memset(name, 0, size);

  strcpy(name, srv_log_group_home_dir);
  ulint len = strlen(name);

  if (name[len - 1] != OS_PATH_SEPARATOR) {
    name[len] = OS_PATH_SEPARATOR;
    len = strlen(name);
  }

  snprintf(name + len, size - len, "%s%lu_%s", undo::s_log_prefix,
           (ulong)id2num(space_id), s_log_ext);

  return (name);
}

void Tablespace::alter_active() {
  m_rsegs->x_lock();
  ut_d(ib::info(ER_IB_MSG_UNDO_ALTERED_ACTIVE, file_name()));
  if (m_rsegs->is_empty()) {
    m_rsegs->set_active();
  } else if (m_rsegs->is_inactive_explicit()) {
    if (purge_sys->undo_trunc.get_marked_space_num() == m_num) {
      m_rsegs->set_inactive_implicit();
    } else {
      m_rsegs->set_active();
    }
  }
  m_rsegs->x_unlock();
}

#ifdef UNIV_DEBUG
void inject_crash(const char *injection_point_name) {
  DBUG_EXECUTE_IF(injection_point_name,
                  ib::info(ER_IB_MSG_UNDO_INJECT_CRASH, injection_point_name);
                  log_buffer_flush_to_disk(); DBUG_SUICIDE(););
}

bool Inject_failure_once::should_fail() {
  DBUG_EXECUTE_IF(m_inject_name, {
    if (!m_already_failed) {
      m_already_failed = true;
      ib::info(ER_IB_MSG_UNDO_INJECT_FAILURE, m_inject_name);
      return true;
    }
  });
  return false;
}

#endif /* UNIV_DEBUG */

dberr_t start_logging(Tablespace *undo_space) {
#ifdef UNIV_DEBUG
  static undo::Inject_failure_once injector("ib_undo_trunc_fail_start_logging");
  if (injector.should_fail()) {
    return (DB_OUT_OF_MEMORY);
  }
#endif /* UNIV_DEBUG */

  dberr_t err;
  char *log_file_name = undo_space->log_file_name();

  /* Delete the log file if it exists. */
  os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);

  /* Create the log file, open it and write 0 to indicate
  init phase. */
  bool ret;
  pfs_os_file_t handle =
      os_file_create(innodb_log_file_key, log_file_name, OS_FILE_CREATE,
                     OS_FILE_NORMAL, OS_LOG_FILE, srv_read_only_mode, &ret);
  if (!ret) {
    return (DB_IO_ERROR);
  }

  ulint sz = UNIV_PAGE_SIZE;
  void *buf = ut_zalloc_nokey(sz + UNIV_PAGE_SIZE);
  if (buf == nullptr) {
    os_file_close(handle);
    return (DB_OUT_OF_MEMORY);
  }

  byte *log_buf = static_cast<byte *>(ut_align(buf, UNIV_PAGE_SIZE));

  IORequest request(IORequest::WRITE);

  request.disable_compression();

  err = os_file_write(request, log_file_name, handle, log_buf, 0, sz);

  os_file_flush(handle);
  os_file_close(handle);
  ut_free(buf);

  return (err);
}

/** Mark completion of undo truncate action by writing magic number to
the log file and then removing it from the disk.
If we are going to remove it from disk then why write magic number ?
This is to safeguard from unlink (file-system) anomalies that will keep
the link to the file even after unlink action is successful and
ref-count = 0.
@param[in]  space_num  number of the undo tablespace to truncate. */
void done_logging(space_id_t space_num) {
  dberr_t err;
  /* Calling id2num(space_num) will return the first space_id for this
  space_num. That is good enough since we only need the log_file_name. */
  Tablespace undo_space(id2num(space_num));
  char *log_file_name = undo_space.log_file_name();

  /* If this file does not exist, there is nothing to do. */
  if (!os_file_exists(log_file_name)) {
    return;
  }

  /* Open log file and write magic number to indicate
  done phase. */
  bool ret;
  pfs_os_file_t handle = os_file_create_simple_no_error_handling(
      innodb_log_file_key, log_file_name, OS_FILE_OPEN, OS_FILE_READ_WRITE,
      srv_read_only_mode, &ret);

  if (!ret) {
    os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);
    return;
  }

  ulint sz = UNIV_PAGE_SIZE;
  void *buf = ut_zalloc_nokey(sz + UNIV_PAGE_SIZE);
  if (buf == nullptr) {
    os_file_close(handle);
    os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);
    return;
  }

  byte *log_buf = static_cast<byte *>(ut_align(buf, UNIV_PAGE_SIZE));

  mach_write_to_4(log_buf, undo::s_magic);

  IORequest request(IORequest::WRITE);

  request.disable_compression();

  err = os_file_write(request, log_file_name, handle, log_buf, 0, sz);

  ut_a(err == DB_SUCCESS);

  os_file_flush(handle);
  os_file_close(handle);

  ut_free(buf);
  os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);
}

/** Check if TRUNCATE_DDL_LOG file exist.
@param[in]  space_num  undo tablespace number
@return true if exist else false. */
bool is_active_truncate_log_present(space_id_t space_num) {
  /* Calling id2num(space_num) will return the first space_id for this
  space_num. That is good enough since we only need the log_file_name. */
  Tablespace undo_space(id2num(space_num));
  char *log_file_name = undo_space.log_file_name();

  /* If the log file exists, check it for presence of magic
  number.  If found, then delete the file and report file
  doesn't exist as presence of magic number suggest that
  truncate action was complete. */

  if (os_file_exists(log_file_name)) {
    bool ret;
    pfs_os_file_t handle = os_file_create_simple_no_error_handling(
        innodb_log_file_key, log_file_name, OS_FILE_OPEN, OS_FILE_READ_WRITE,
        srv_read_only_mode, &ret);
    if (!ret) {
      os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);
      return (false);
    }

    ulint sz = UNIV_PAGE_SIZE;
    void *buf = ut_zalloc_nokey(sz + UNIV_PAGE_SIZE);
    if (buf == nullptr) {
      os_file_close(handle);
      os_file_delete_if_exists(innodb_log_file_key, log_file_name, nullptr);
      return (false);
    }

    byte *log_buf = static_cast<byte *>(ut_align(buf, UNIV_PAGE_SIZE));

    IORequest request(IORequest::READ);

    request.disable_compression();

    dberr_t err;

    err = os_file_read(request, log_file_name, handle, log_buf, 0, sz);

    os_file_close(handle);

    if (err != DB_SUCCESS) {
      ib::info(ER_IB_MSG_UNDO_TRUNCATE_FAIL_TO_READ_LOG_FILE, log_file_name,
               ut_strerr(err));

      os_file_delete(innodb_log_file_key, log_file_name);

      ut_free(buf);

      return (false);
    }

    ulint magic_no = mach_read_from_4(log_buf);

    ut_free(buf);

    if (magic_no == undo::s_magic) {
      /* Found magic number. */
      os_file_delete(innodb_log_file_key, log_file_name);
      return (false);
    }

    return (true);
  }

  return (false);
}

/** Add undo tablespace to s_under_construction vector.
@param[in]	space_id	space id of tablespace to
truncate */
void add_space_to_construction_list(space_id_t space_id) {
  s_under_construction.push_back(space_id);
}

/** Clear the s_under_construction vector. */
void clear_construction_list() { s_under_construction.clear(); }

/** Is an undo tablespace under constuction at the moment.
@param[in]	space_id	space id to check
@return true if marked for truncate, else false. */
bool is_under_construction(space_id_t space_id) {
  for (auto construct_id : s_under_construction) {
    if (construct_id == space_id) {
      return (true);
    }
  }

  return (false);
}

/* Set an undo tablespace active. */
void set_active(space_id_t space_id) {
  ut_ad(spaces != nullptr);
  ut_ad(fsp_is_undo_tablespace(space_id));

  spaces->s_lock();
  Tablespace *undo_space = spaces->find(id2num(space_id));

  if (undo_space != nullptr) {
    undo_space->set_active();
  }
  spaces->s_unlock();
}

/* Return whether the undo tablespace is active.  If this is a
non-undo tablespace, then it will not be found in spaces and it
will not be under construction, so this function will return true.
@param[in]  space_id   Undo Tablespace ID
@param[in]  get_latch  Specifies whether the rsegs->s_lock() is needed.
@return true if active (non-undo spaces are always active) */
bool is_active(space_id_t space_id, bool get_latch) {
  if (!fsp_is_undo_tablespace(space_id)) {
    return (true);
  }

  if (spaces == nullptr) {
    return (!is_under_construction(space_id));
  }

  if (get_latch) {
    undo::spaces->s_lock();
  }
  Tablespace *undo_space = spaces->find(id2num(space_id));

  if (undo_space == nullptr) {
    if (get_latch) {
      undo::spaces->s_unlock();
    }
    return (!is_under_construction(space_id));
  }

  bool ret =
      (get_latch ? undo_space->is_active() : undo_space->is_active_no_latch());

  if (get_latch) {
    undo::spaces->s_unlock();
  }

  return (ret);
}
}  // namespace undo

/* Declare this global object. */
Space_Ids undo::s_under_construction;

/** Decide if an undo truncation needs to be done at this time. If an undo
tablespace is already marked, return that so that this truncation will get
finished. If none is marked, iterate over all the UNDO tablespaces and check
if any qualify to be truncated.
  Normal operation; Choose the marked space.
                    If none are marked, choose an explicitly inactive space.
                    If none exist, check if conditions allow implicit
                       truncation.
                    If conditions allow, choose an implicitly inactive space.
                    If none exist and no space has been truncated yet,
                       look for an undo space that is too big.
  Fast shutdown;    Do not truncate.  This routine is not called.
  Slow shutdown;    Choose the marked space.
                    If none are marked, choose an explicitly inactive
                       space.
                    If none exist, check if conditions allow implicit
                       truncation.
                    If conditions allow, ignoring the previous truncate count,
                    look for an undo space that is too big.
@param[in]  truncate_count    number of times this is called in a loop
@return true if an undo tablespace was marked for truncate. */
static bool trx_purge_mark_undo_for_truncate(size_t truncate_count) {
  /* We always have at least 2 undo spaces, even though one of them may be
  inactive. */
  ut_a(undo::spaces->size() >= FSP_IMPLICIT_UNDO_TABLESPACES);

  /* Note if we are currently in a fast or slow shutdown. */
  bool normal_operation = (srv_shutdown_state == SRV_SHUTDOWN_NONE);
  bool in_fast_shutdown = (!normal_operation && srv_fast_shutdown > 0);

  /* Save time during a fast shutdown by skipping undo truncation.
  This does not affect correctness since undo tablespaces that need
  truncation can be truncated during or after startup.*/
  if (in_fast_shutdown) {
    return (false);
  }

  /* Return true if an undo tablespace is already marked for truncate. */
  auto undo_trunc = &purge_sys->undo_trunc;
  if (undo_trunc->is_marked()) {
    return (true);
  }

  undo::spaces->s_lock();

  /* In order to implicitly select an undo space to truncate, we need
  at least 2 active UNDO tablespaces.  As long as there is one undo
  tablespace active the server will continue to operate. */
  ulint num_active = 0;

  /* Look for any undo space that is inactive explicitly. */
  for (auto undo_ts : undo::spaces->m_spaces) {
    if (undo_ts->is_inactive_explicit()) {
      undo_trunc->mark(undo_ts);
      undo::spaces->s_unlock();
      return (true);
    }
    num_active += (undo_ts->is_active() ? 1 : 0);
  }

  undo::spaces->s_unlock();

  /* If we get here, there are no undo spaces currently being truncated
  and none that are SET INACTIVE explicitly. */
  ut_a(num_active > 0 + FSP_IMPLICIT_TXN_TABLESPACES);

  /* There may be some reasons not to truncate implicitly.
  If truncate is disabled, do not truncate. */
  if (!srv_undo_log_truncate) {
    return (false);
  }

  if (normal_operation) {
    /* Skip truncate if there is only one active undo tablespace to check. */
    if (num_active == 1 + FSP_IMPLICIT_TXN_TABLESPACES) {
      return (false);
    }

    /* Skip truncate if the caller has already truncated an undo space. */
    if (truncate_count > 0) {
      return (false);
    }

    /* Wait at least one second between searches. */
    if (undo_trunc->check_timer() < PURGE_CHECK_UNDO_TRUNCATE_DELAY_IN_MS) {
      return (false);
    }
    undo_trunc->reset_timer();
  }

  /* Find an undo tablespace that is too big and needs truncation.  Avoid
  bias selection and so start the scan from immediate next of the last
  space selected for truncate. Scan through all undo tablespaces. */

  undo::spaces->s_lock();

  space_id_t space_num = undo_trunc->get_scan_space_num();
  space_id_t first_space_num_scanned = space_num;

  do {
    auto undo_space = undo::spaces->find(space_num);

    if (!undo_space->is_txn() && undo_space->needs_truncation()) {
      /* Tablespace qualifies for truncate. */
      undo_trunc->increment_scan();
      undo_trunc->mark(undo_space);
      break;
    }

    space_num = undo_trunc->increment_scan();

  } while (space_num != first_space_num_scanned);

  undo::spaces->s_unlock();

  /* Return false if no undo space needs to be truncated. */
  if (!undo_trunc->is_marked()) {
    return (false);
  }

  ut_ad(space_num == undo_trunc->get_marked_space_num());

  return (true);
}

void undo::Truncate::mark(Tablespace *undo_space) {
  /* Set the internal state of this undo space to inactive_implicit
  so that its rsegs will not be allocated to any new transaction.
  If the space is already in the inactive_explicit state, it will
  stay there.
  Note that the DD is not modified since in case of crash, the
  action must be completed before the DD is available.
  Set both the state and this marked id while this routine has
  an x_lock on m_rsegs because a concurrent user thread might issue
  undo_space->alter_active(). */
  undo_space->set_inactive_implicit(&m_space_id_marked);

  m_marked_space_is_empty = false;

  /* We found an UNDO-tablespace to truncate so set the
  local purge rseg truncate frequency to 3. This will help
  accelerate the purge action and in turn truncate. */
  set_rseg_truncate_frequency(3);

  ut_d(ib::info(ER_IB_MSG_UNDO_MARKED_FOR_TRUNCATE, undo_space->file_name()));
}

size_t undo::Truncate::s_scan_pos;

/** Iterate over selected UNDO tablespace and check if all the rsegs
that resides in the tablespace have been freed.
@param[in]	limit		truncate_limit */
static bool trx_purge_check_if_marked_undo_is_empty(purge_iter_t *limit) {
  undo::Truncate *undo_trunc = &purge_sys->undo_trunc;

  ut_ad(undo_trunc->is_marked());

  /* Return immediately if the marked UNDO tablespace has already been
  found to be empty. */
  if (undo_trunc->is_marked_space_empty()) {
    return (true);
  }

  undo::spaces->s_lock();
  space_id_t space_num = undo_trunc->get_marked_space_num();
  undo::Tablespace *marked_space = undo::spaces->find(space_num);
  Rsegs *marked_rsegs = marked_space->rsegs();

  /* Scan over each rseg in this inactive undo tablespace
  and ensure that it does not hold any active undo records. */
  bool all_free = true;

  marked_rsegs->x_lock();

  /* If an undo tablespace is marked, its rsegs are inactive. */
  ut_ad(!marked_rsegs->is_active());

  for (auto rseg : *marked_rsegs) {
    rseg->latch();

    if (rseg->trx_ref_count > 0) {
      /* This rseg is still being held by an active transaction. */
      all_free = false;
    } else if (rseg->last_page_no != FIL_NULL) {
      /* This rseg still has data to be purged. */
      all_free = false;
    }

    rseg->unlatch();

    if (!all_free) {
      break;
    }
  }

  if (all_free) {
    undo_trunc->set_marked_space_empty();
  }

  marked_rsegs->x_unlock();

  undo::spaces->s_unlock();

  return (all_free);
}

/** Truncate the marked undo tablespace. This layer handles locking of
marked_rsegs and undo::spaces.
@return true for success, false for failure */
static bool trx_purge_truncate_marked_undo_low(space_id_t space_num,
                                               std::string space_name) {
  undo::Truncate *undo_trunc = &purge_sys->undo_trunc;

  /* Get the undo space pointer again. */
  undo::spaces->x_lock();

  undo::Tablespace *marked_space = undo::spaces->find(space_num);

#ifdef UNIV_DEBUG
  static undo::Inject_failure_once inject_marked_space(
      "ib_undo_trunc_fail_marked_space");
  if (inject_marked_space.should_fail()) {
    marked_space = nullptr;
  };
#endif /* UNIV_DEBUG */

  if (marked_space == nullptr) {
    undo::spaces->x_unlock();
    return (false);
  }

  ut_d(undo::inject_crash("ib_undo_trunc_before_ddl_log_start"));

  MONITOR_INC_VALUE(MONITOR_UNDO_TRUNCATE_START_LOGGING_COUNT, 1);
  dberr_t err = undo::start_logging(marked_space);
  if (err != DB_SUCCESS) {
    ib::error(ER_IB_MSG_UNDO_TRUNCATE_DELAY_BY_LOG_CREATE, space_name.c_str());
    undo::spaces->x_unlock();
    return (false);
  }
  ut_ad(err == DB_SUCCESS);

  ut_d(undo::inject_crash("ib_undo_trunc_before_truncate"));

  /* Don't do the actual truncate if we are doing a fast shutdown.
  The fixup routines will do it at startup. */
  bool in_fast_shutdown = (srv_shutdown_state.load() != SRV_SHUTDOWN_NONE &&
                           srv_fast_shutdown != 0);
#ifdef UNIV_DEBUG
  static undo::Inject_failure_once inject_fast_shutdown(
      "ib_undo_trunc_fail_fast_shutdown");
  if (inject_fast_shutdown.should_fail()) {
    in_fast_shutdown = true;
  };
#endif /* UNIV_DEBUG */

  if (in_fast_shutdown) {
    undo::spaces->x_unlock();
    return (false);
  }

  /* Do the truncate.  This will change the space_id of the marked_space. */
  bool success = trx_undo_truncate_tablespace(marked_space);

  undo::spaces->x_unlock();

  if (!success) {
    /* Note: In case of error we don't enable the rsegs nor unmark the
     tablespace. So the tablespace will continue to remain inactive. */
    ib::warn(ER_IB_MSG_UNDO_TRUNCATE_DELAY_BY_FAILURE, space_name.c_str());
    return (false);
  }

  ut_d(undo::inject_crash("ib_undo_trunc_before_state_update"));

  space_id_t new_space_id = marked_space->id();

  /* Determine the next state. */
  dd_space_states next_state;

  undo::spaces->s_lock();

  Rsegs *marked_rsegs = marked_space->rsegs();
  marked_rsegs->x_lock();

  if (marked_rsegs->is_inactive_explicit()) {
    next_state = DD_SPACE_STATE_EMPTY;

    /* This was made inactive and truncated due to
    an  ALTER TABLESPACE SET INACTIVE statement.
    Mark it empty now so that it can be DROPPED. */
    marked_rsegs->set_empty();
    ut_d(ib::info(ER_IB_MSG_UNDO_MARKED_EMPTY, marked_space->file_name()));

  } else {
    ut_ad(marked_rsegs->is_inactive_implicit());
    next_state = DD_SPACE_STATE_ACTIVE;

    /* This was made inactive and truncated due to
    normal background undo tablespace truncation.
    Make it 'active' again. */
    marked_rsegs->set_active();
    ut_d(ib::info(ER_IB_MSG_UNDO_MARKED_ACTIVE, marked_space->file_name()));
  }

  undo_trunc->reset();

  marked_rsegs->x_unlock();
  undo::spaces->s_unlock();

  ut_d(undo::inject_crash("ib_undo_trunc_before_dd_update"));

  /* Update the DD with the new space ID and state. */
  if (DD_FAILURE == dd_tablespace_set_id_and_state(space_name.c_str(),
                                                   new_space_id, next_state)) {
    return (false);
  }

  return (true);
}

/** Truncate the marked undo tablespace.
This wrapper does initial preparation and handles cleanup.
@return true for success, false for failure */
static bool trx_purge_truncate_marked_undo() {
  MONITOR_INC_VALUE(MONITOR_UNDO_TRUNCATE_COUNT, 1);
  auto counter_time_truncate = ut_time_monotonic_us();

  /* Initialize variables */
  undo::Truncate *undo_trunc = &purge_sys->undo_trunc;
  ut_ad(undo_trunc->is_marked());
  ut_ad(undo_trunc->is_marked_space_empty());

  undo::spaces->s_lock();
  space_id_t space_num = undo_trunc->get_marked_space_num();
  undo::Tablespace *marked_space = undo::spaces->find(space_num);
  std::string space_name = marked_space->space_name();
  undo::spaces->s_unlock();

  /* Don't truncate if a concurrent clone is in progress. */
  if (clone_check_active()) {
    ib::info(ER_IB_MSG_UNDO_TRUNCATE_DELAY_BY_CLONE, space_name.c_str());
    return (false);
  }

  ib::info(ER_IB_MSG_UNDO_TRUNCATE_START, space_name.c_str());

  ut_d(undo::inject_crash("ib_undo_trunc_before_mdl"));

  /* Get the MDL lock to prevent an ALTER or DROP command from interferring
  with this undo tablespace while it is being truncated. */
  MDL_ticket *mdl_ticket;
  bool dd_result =
      dd_tablespace_get_mdl(space_name.c_str(), &mdl_ticket, false);

#ifdef UNIV_DEBUG
  static undo::Inject_failure_once injector("ib_undo_trunc_fail_get_mdl");
  if (injector.should_fail()) {
    dd_release_mdl(mdl_ticket);
    dd_result = DD_FAILURE;
  };
#endif /* UNIV_DEBUG */

  if (dd_result != DD_SUCCESS) {
    MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_UNDO_TRUNCATE_MICROSECOND,
                                   counter_time_truncate);
    ib::info(ER_IB_MSG_UNDO_TRUNCATE_DELAY_BY_MDL, space_name.c_str());
    return (false);
  }
  ut_ad(mdl_ticket != nullptr);

  /* Re-check for clone after acquiring MDL. The Backup MDL from clone
  is released by clone during shutdown while provisioning. We should
  not allow truncate to proceed here. */
  if (clone_check_active()) {
    dd_release_mdl(mdl_ticket);
    ib::info(ER_IB_MSG_UNDO_TRUNCATE_DELAY_BY_CLONE, space_name.c_str());
    return (false);
  }

  /* Serialize this truncate with all undo tablespace DDLs */
  mutex_enter(&undo::ddl_mutex);

  if (!trx_purge_truncate_marked_undo_low(space_num, space_name)) {
    mutex_exit(&undo::ddl_mutex);
    dd_release_mdl(mdl_ticket);
    MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_UNDO_TRUNCATE_MICROSECOND,
                                   counter_time_truncate);
    return (false);
  }

  ut_d(undo::inject_crash("ib_undo_trunc_before_done_logging"));

  undo::spaces->x_lock();
  undo::done_logging(space_num);
  undo::spaces->x_unlock();
  MONITOR_INC_VALUE(MONITOR_UNDO_TRUNCATE_DONE_LOGGING_COUNT, 1);

  /* Truncate is complete. Now it is safe to re-use the tablespace. */
  ib::info(ER_IB_MSG_UNDO_TRUNCATE_COMPLETE, space_name.c_str());

  dd_release_mdl(mdl_ticket);

  ut_d(undo::inject_crash("ib_undo_trunc_trunc_done"));

  mutex_exit(&undo::ddl_mutex);

  MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_UNDO_TRUNCATE_MICROSECOND,
                                 counter_time_truncate);
  return (true);
}

/** Removes unnecessary history data from rollback segments. NOTE that when
 this function is called, the caller must not have any latches on undo log
 pages! */
static void trx_purge_truncate_history(
    purge_iter_t *limit,          /*!< in: truncate limit */
    const lizard::Vision *vision) /*!< in: purge view */
{
  MONITOR_INC_VALUE(MONITOR_PURGE_TRUNCATE_HISTORY_COUNT, 1);

  auto counter_time_truncate_history = ut_time_monotonic_us();

  /* We play safe and set the truncate limit at most to the purge view
  low_limit number, though this is not necessary */

  if (limit->scn > vision->snapshot_scn()) {
    limit->scn = vision->snapshot_scn();
    limit->undo_no = 0;
    limit->undo_rseg_space = SPACE_UNKNOWN;
  }

  ut_ad(limit->scn <= purge_sys->vision.snapshot_scn());

  /* Purge rollback segments in all undo tablespaces.  This may take
  some time and we do not want an undo DDL to attempt an x_lock during
  this time.  If it did, all other transactions seeking a short s_lock()
  would line up behind it.  So get the ddl_mutex before this s_lock(). */
  mutex_enter(&undo::ddl_mutex);
  undo::spaces->s_lock();
  for (auto undo_space : undo::spaces->m_spaces) {
    /* Skip undo tablespace that is already empty and marked for truncation. */
    undo::Truncate &ut = purge_sys->undo_trunc;

    if (ut.is_equal(undo_space->id()) && ut.is_marked() &&
        ut.is_marked_space_empty()) {
      continue;
    }

    /* Purge rollback segments in this undo tablespace. */
    undo_space->rsegs()->s_lock();

    for (auto rseg : *undo_space->rsegs()) {
      trx_purge_truncate_rseg_history(rseg, limit);
    }
    undo_space->rsegs()->s_unlock();
  }

  undo::spaces->s_unlock();
  mutex_exit(&undo::ddl_mutex);

  /* Purge rollback segments in the system tablespace, if any.
  Use an s-lock for the whole list since it can have gaps and
  may be sorted when added to. */
  trx_sys->rsegs.s_lock();
  for (auto rseg : trx_sys->rsegs) {
    trx_purge_truncate_rseg_history(rseg, limit);
  }
  trx_sys->rsegs.s_unlock();

  /* Purge rollback segments in the temporary tablespace. */
  trx_sys->tmp_rsegs.s_lock();
  for (auto rseg : trx_sys->tmp_rsegs) {
    trx_purge_truncate_rseg_history(rseg, limit);
  }
  trx_sys->tmp_rsegs.s_unlock();

  auto &undo_trunc = purge_sys->undo_trunc;

  MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_PURGE_TRUNCATE_HISTORY_MICROSECOND,
                                 counter_time_truncate_history);

  /* Undo Truncation. */
  const size_t space_count = undo::spaces->size();
  for (size_t i = 0; i < space_count; ++i) {
    /* Check current activity and if conditions allow,
    mark the undo space that needs to be truncated. */
    if (!trx_purge_mark_undo_for_truncate(i)) {
      break; /* No truncation is needed at this time. */
    }

    /* A space was marked but may not be yet empty. */
    ut_a(undo_trunc.is_marked());

    /* If any undo logs need to be purged from this marked space,
    try again later. */
    if (!trx_purge_check_if_marked_undo_is_empty(limit)) {
      break;
    }

    /* A space has been marked and is now empty. */
    ut_a(undo_trunc.is_marked_space_empty());

    /* Truncate the marked space. */
    if (!trx_purge_truncate_marked_undo()) {
      /* If the marked and empty space did not get trucated
      due to a concurrent clone or something else,
      try again later. */
      break;
    }
  }
}

/** Updates the last not yet purged history log info in rseg when we have
 purged a whole undo log. Advances also purge_sys->purge_trx_no past the
 purged log. */
static void trx_purge_rseg_get_next_history_log(
    trx_rseg_t *rseg,       /*!< in: rollback segment */
    ulint *n_pages_handled) /*!< in/out: number of UNDO pages
                            handled */
{
  page_t *undo_page;
  trx_ulogf_t *log_hdr;
  fil_addr_t prev_log_addr;
  ibool del_marks;
  mtr_t mtr;
  commit_scn_t cmmt;

  rseg->latch();

  ut_a(rseg->last_page_no != FIL_NULL);

  purge_sys->iter.undo_no = 0;
  purge_sys->iter.undo_rseg_space = SPACE_UNKNOWN;
  purge_sys->next_stored = FALSE;
  purge_sys->iter.scn = rseg->last_scn + 1;

  mtr_start(&mtr);

  undo_page = trx_undo_page_get_s_latched(
      page_id_t(rseg->space_id, rseg->last_page_no), rseg->page_size, &mtr);

  log_hdr = undo_page + rseg->last_offset;

  /* Increase the purge page count by one for every handled log */

  (*n_pages_handled)++;

  prev_log_addr = trx_purge_get_log_from_hist(
      flst_get_prev_addr(log_hdr + TRX_UNDO_HISTORY_NODE, &mtr));

  if (prev_log_addr.page == FIL_NULL) {
    /* No logs left in the history list */

    rseg->last_page_no = FIL_NULL;

    mtr_commit(&mtr);
    rseg->unlatch();

#ifdef UNIV_DEBUG
    trx_sys_mutex_enter();

    /* Add debug code to track history list corruption reported
    on the MySQL mailing list on Nov 9, 2004. The fut0lst.cc
    file-based list was corrupt. The prev node pointer was
    FIL_NULL, even though the list length was over 8 million nodes!
    We assume that purge truncates the history list in large
    size pieces, and if we here reach the head of the list, the
    list cannot be longer than 2000 000 undo logs now. */

    if (trx_sys->rseg_history_len > 2000000) {
      ib::warn(ER_IB_MSG_1177) << "Purge reached the head of the history"
                                  " list, but its length is still reported as "
                               << trx_sys->rseg_history_len
                               << " which is"
                                  " unusually high.";
      ib::info(ER_IB_MSG_1178) << "This can happen for multiple reasons";
      ib::info(ER_IB_MSG_1179) << "1. A long running transaction is"
                                  " withholding purging of undo logs or a read"
                                  " view is open. Please try to commit the long"
                                  " running transaction.";
      ib::info(ER_IB_MSG_1180) << "2. Try increasing the number of purge"
                                  " threads to expedite purging of undo logs.";
    }

    trx_sys_mutex_exit();
#endif
    return;
  }

  mtr_commit(&mtr);
  rseg->unlatch();

  /* Read the trx number and del marks from the previous log header */
  mtr_start(&mtr);

  log_hdr =
      trx_undo_page_get_s_latched(page_id_t(rseg->space_id, prev_log_addr.page),
                                  rseg->page_size, &mtr) +
      prev_log_addr.boffset;

  del_marks = mach_read_from_2(log_hdr + TRX_UNDO_DEL_MARKS);

  cmmt = lizard::trx_undo_hdr_read_scn(log_hdr, &mtr);

  mtr_commit(&mtr);

  rseg->latch();

  rseg->last_page_no = prev_log_addr.page;
  rseg->last_offset = prev_log_addr.boffset;
  rseg->last_del_marks = del_marks;
  rseg->last_scn = cmmt.scn;

  lizard::TxnUndoRsegs elem(rseg->last_scn);
  elem.push_back(rseg);

  /* Purge can also produce events, however these are already ordered
  in the rollback segment and any user generated event will be greater
  than the events that Purge produces. ie. Purge can never produce
  events from an empty rollback segment. */

  mutex_enter(&purge_sys->pq_mutex);

  purge_sys->purge_heap->push(elem);

  lizard_purged_scn_validation();

  mutex_exit(&purge_sys->pq_mutex);

  rseg->unlatch();
}

/** Position the purge sys "iterator" on the undo record to use for purging.
@param[in,out]	purge_sys	purge instance
@param[in]	page_size	page size */
static void trx_purge_read_undo_rec(trx_purge_t *purge_sys,
                                    const page_size_t &page_size) {
  ulint offset;
  page_no_t page_no;
  ib_uint64_t undo_no;
  space_id_t undo_rseg_space;
  trx_id_t modifier_trx_id;

  purge_sys->hdr_offset = purge_sys->rseg->last_offset;
  page_no = purge_sys->hdr_page_no = purge_sys->rseg->last_page_no;

  if (purge_sys->rseg->last_del_marks) {
    mtr_t mtr;
    trx_undo_rec_t *undo_rec = nullptr;

    mtr_start(&mtr);

    undo_rec = trx_undo_get_first_rec(
        &modifier_trx_id, purge_sys->rseg->space_id, page_size,
        purge_sys->hdr_page_no, purge_sys->hdr_offset, RW_S_LATCH, &mtr);

    if (undo_rec != nullptr) {
      offset = page_offset(undo_rec);
      undo_no = trx_undo_rec_get_undo_no(undo_rec);
      undo_rseg_space = purge_sys->rseg->space_id;
      page_no = page_get_page_no(page_align(undo_rec));
    } else {
      offset = 0;
      undo_no = 0;
      undo_rseg_space = SPACE_UNKNOWN;
    }

    mtr_commit(&mtr);
  } else {
    offset = 0;
    undo_no = 0;
    undo_rseg_space = SPACE_UNKNOWN;
    modifier_trx_id = 0;
  }

  purge_sys->offset = offset;
  purge_sys->page_no = page_no;
  purge_sys->iter.undo_no = undo_no;
  purge_sys->iter.modifier_trx_id = modifier_trx_id;
  purge_sys->iter.undo_rseg_space = undo_rseg_space;

  ut_a(purge_sys->iter.scn <= purge_sys->vision.snapshot_scn());

  purge_sys->next_stored = TRUE;
}

/** Chooses the next undo log to purge and updates the info in purge_sys. This
 function is used to initialize purge_sys when the next record to purge is
 not known, and also to update the purge system info on the next record when
 purge has handled the whole undo log for a transaction.
 @return true if the top of purge_sys::purge_heap is kept, because of its scn is larger
 than purge_sys->vision */
static bool trx_purge_choose_next_log(void) {
  bool keep_top = false;
  std::pair<commit_scn_t, bool> purged_result;
  ut_ad(purge_sys->next_stored == FALSE);

  const page_size_t &page_size = purge_sys->rseg_iter->set_next(&keep_top);

  if (purge_sys->rseg != NULL && !keep_top) {
    lizard_ut_ad(lizard::txn_undo_log_has_purged(purge_sys->rseg, page_size));
    purged_result =
        lizard::txn_undo_set_state_at_purge(purge_sys->rseg, page_size);
    /** If purging txn undo log hdr correctly */
    if (!purged_result.second) {
      lizard::trx_purge_set_purged_scn(purged_result.first.scn);
      purge_sys->purged_gcn.flush(purged_result.first.gcn);
      purge_sys->top_undo_utc = purged_result.first.utc;
    }
    trx_purge_read_undo_rec(purge_sys, page_size);
  } else {
    /* There is nothing to do yet. */
    os_thread_yield();
  }
  return keep_top;
}

/** Gets the next record to purge and updates the info in the purge system.
 @return copy of an undo log record or pointer to the dummy undo log record */
static trx_undo_rec_t *trx_purge_get_next_rec(
    ulint *n_pages_handled, /*!< in/out: number of UNDO pages
                            handled */
    mem_heap_t *heap)       /*!< in: memory heap where copied */
{
  trx_undo_rec_t *rec;
  trx_undo_rec_t *rec_copy;
  trx_undo_rec_t *rec2;
  page_t *undo_page;
  page_t *page;
  ulint offset;
  page_no_t page_no;
  space_id_t space;
  mtr_t mtr;

  ut_ad(purge_sys->next_stored);
  ut_ad(purge_sys->iter.scn <= purge_sys->vision.snapshot_scn());

  space = purge_sys->rseg->space_id;
  page_no = purge_sys->page_no;
  offset = purge_sys->offset;

  const page_size_t page_size(purge_sys->rseg->page_size);

  if (offset == 0) {
    /* It is the dummy undo log record, which means that there is
    no need to purge this undo log */

    trx_purge_rseg_get_next_history_log(purge_sys->rseg, n_pages_handled);

    /* Look for the next undo log and record to purge */

    trx_purge_choose_next_log();

    return (&trx_purge_ignore_rec);
  }

  mtr_start(&mtr);

  undo_page =
      trx_undo_page_get_s_latched(page_id_t(space, page_no), page_size, &mtr);

  rec = undo_page + offset;

  rec2 = rec;

  for (;;) {
    ulint type;
    trx_undo_rec_t *next_rec;
    ulint cmpl_info;

    /* Try first to find the next record which requires a purge
    operation from the same page of the same undo log */

    next_rec = trx_undo_page_get_next_rec(rec2, purge_sys->hdr_page_no,
                                          purge_sys->hdr_offset);

    if (next_rec == nullptr) {
      rec2 = trx_undo_get_next_rec(rec2, purge_sys->hdr_page_no,
                                   purge_sys->hdr_offset, &mtr);
      break;
    }

    rec2 = next_rec;

    type = trx_undo_rec_get_type(rec2);

    if (type == TRX_UNDO_DEL_MARK_REC) {
      break;
    }

    cmpl_info = trx_undo_rec_get_cmpl_info(rec2);

    if (trx_undo_rec_get_extern_storage(rec2)) {
      break;
    }

    if ((type == TRX_UNDO_UPD_EXIST_REC) &&
        !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
      break;
    }
  }

  if (rec2 == nullptr) {
    mtr_commit(&mtr);

    trx_purge_rseg_get_next_history_log(purge_sys->rseg, n_pages_handled);

    /* Look for the next undo log and record to purge */

    trx_purge_choose_next_log();

    mtr_start(&mtr);

    undo_page =
        trx_undo_page_get_s_latched(page_id_t(space, page_no), page_size, &mtr);

  } else {
    page = page_align(rec2);

    purge_sys->offset = rec2 - page;
    purge_sys->page_no = page_get_page_no(page);
    purge_sys->iter.undo_no = trx_undo_rec_get_undo_no(rec2);
    purge_sys->iter.undo_rseg_space = space;

    if (undo_page != page) {
      /* We advance to a new page of the undo log: */
      (*n_pages_handled)++;
    }
  }

  rec_copy = trx_undo_rec_copy(undo_page, static_cast<uint32_t>(offset), heap);

  mtr_commit(&mtr);

  return (rec_copy);
}

/** Fetches the next undo log record from the history list to purge. It must
 be released with the corresponding release function.
 @return copy of an undo log record or pointer to trx_purge_ignore_rec,
 if the whole undo log can skipped in purge; NULL if none left */
static MY_ATTRIBUTE((warn_unused_result))
    trx_undo_rec_t *trx_purge_fetch_next_rec(
        trx_id_t *modifier_trx_id,
        /*!< out: modifier trx id. this is the
        trx that created the undo record. */
        roll_ptr_t *roll_ptr,   /*!< out: roll pointer to undo record */
        ulint *n_pages_handled, /*!< in/out: number of UNDO log pages
                                handled */
        mem_heap_t *heap)       /*!< in: memory heap where copied */
{
  bool keep_top = false;
  if (!purge_sys->next_stored) {
    keep_top = trx_purge_choose_next_log();

    /** keep_top == true mean the top of heap is larger than purge_sys's
    vision. That is to say, there are logs left in the history list */
    if (!purge_sys->next_stored && !keep_top) {
      DBUG_PRINT("ib_purge", ("no logs left in the history list"));
      return (nullptr);
    }
  }

  if (purge_sys->iter.scn > purge_sys->vision.snapshot_scn() || keep_top) {
    return NULL;
  }

  if (lizard::Undo_retention::instance()->purge_advise()) {
    return &trx_purge_blocked_rec;
  }

  *roll_ptr = trx_undo_build_roll_ptr(FALSE, purge_sys->rseg->space_id,
                                      purge_sys->page_no, purge_sys->offset);

  *modifier_trx_id = purge_sys->iter.modifier_trx_id;

  /* The following call will advance the stored values of the
  purge iterator. */

  return (trx_purge_get_next_rec(n_pages_handled, heap));
}

/** This function runs a purge batch.
@param[in]      n_purge_threads  number of purge threads
@param[in]      batch_size       number of pages to purge
@param[out]     blocked          is blocked by retention
@return number of undo log pages handled in the batch */
static ulint trx_purge_attach_undo_recs(const ulint n_purge_threads,
                                        ulint batch_size,
                                        bool *blocked) {
  que_thr_t *thr;
  ulint n_pages_handled = 0;

#ifdef UNIV_DEBUG
  std::set<table_id_t> all_table_ids;
#endif /* UNIV_DEBUG */

  ut_a(n_purge_threads > 0);
  ut_a(n_purge_threads <= MAX_PURGE_THREADS);

  if (blocked) *blocked = false; // lizard

  purge_sys->limit = purge_sys->iter;

  que_thr_t *run_thrs[MAX_PURGE_THREADS];

  /* Validate some pre-requisites and reset done flag. */
  ulint i = 0;

  for (thr = UT_LIST_GET_FIRST(purge_sys->query->thrs);
       thr != nullptr && i < n_purge_threads;
       thr = UT_LIST_GET_NEXT(thrs, thr), ++i) {
    purge_node_t *node;

    /* Get the purge node. */
    node = static_cast<purge_node_t *>(thr->child);

    ut_a(que_node_get_type(node) == QUE_NODE_PURGE);
    ut_a(node->recs == nullptr);
    ut_a(node->done);

    node->done = false;

    ut_a(!thr->is_active);

    run_thrs[i] = thr;
  }

  /* There should never be fewer nodes than threads, the inverse
  however is allowed because we only use purge threads as needed. */
  ut_a(i == n_purge_threads);
  ut_ad(trx_purge_check_limit());

  mem_heap_t *heap = purge_sys->heap;

  mem_heap_empty(heap);

  using GroupBy = std::map<
      table_id_t, purge_node_t::Recs *, std::less<table_id_t>,
      mem_heap_allocator<std::pair<const table_id_t, purge_node_t::Recs *>>>;

  GroupBy group_by{GroupBy::key_compare{},
                   mem_heap_allocator<GroupBy::value_type>{heap}};

  for (ulint i = 0; n_pages_handled < batch_size; ++i) {
    /* Track the max {trx_id, undo_no} for truncating the
    UNDO logs once we have purged the records. */

    if (trx_purge_check_limit()) {
      purge_sys->limit = purge_sys->iter;
    }

    purge_node_t::rec_t rec;

    /* Fetch the next record, and advance the purge_sys->iter. */
    rec.undo_rec = trx_purge_fetch_next_rec(&rec.modifier_trx_id, &rec.roll_ptr,
                                            &n_pages_handled, heap);

    if (rec.undo_rec == &trx_purge_ignore_rec) {
      continue;

    } else if (rec.undo_rec == nullptr) {
      break;
    } else if (rec.undo_rec == &trx_purge_blocked_rec) {
      if (blocked) *blocked = true; // lizard
      break;
    }

    table_id_t table_id;

    table_id = trx_undo_rec_get_table_id(rec.undo_rec);

#ifdef UNIV_DEBUG
    all_table_ids.insert(table_id);
#endif /* UNIV_DEBUG */

    GroupBy::iterator lb = group_by.lower_bound(table_id);

    if (lb != group_by.end() && !(group_by.key_comp()(table_id, lb->first))) {
      lb->second->push_back(rec);

    } else {
      using value_type = GroupBy::value_type;

      void *ptr;
      purge_node_t::Recs *recs;

      ptr = mem_heap_alloc(heap, sizeof(purge_node_t::Recs));

      /* Call the destructor explicitly in row_purge_end() */
      recs = new (ptr)
          purge_node_t::Recs{mem_heap_allocator<purge_node_t::rec_t>{heap}};

      recs->push_back(rec);

      group_by.insert(lb, value_type(table_id, recs));
    }
  }

  /* Objective is to ensure that all the table entries in one
  batch are handled by the same thread. Ths is to avoid contention
  on the dict_index_t::lock */

  GroupBy::const_iterator end = group_by.cend();

  for (GroupBy::const_iterator it = group_by.cbegin(); it != end;) {
    for (ulint i = 0; i < n_purge_threads && it != end; ++i, ++it) {
      purge_node_t *node;

      node = static_cast<purge_node_t *>(run_thrs[i]->child);

      ut_a(que_node_get_type(node) == QUE_NODE_PURGE);

      if (node->recs == nullptr) {
        node->recs = it->second;
      } else {
        for (auto iter = it->second->begin(); iter != it->second->end();
             ++iter) {
          node->recs->push_back(*iter);
        }
      }
    }
  }

#ifdef UNIV_DEBUG
  {
    /* Add validation routine to check whether undo records of same table id
    is being processed by different purge threads concurrently. */

    for (auto xter = all_table_ids.begin(); xter != all_table_ids.end();
         ++xter) {
      table_id_t tid = *xter;
      std::vector<bool> table_exists;

      for (ulint i = 0; i < n_purge_threads; ++i) {
        purge_node_t *node = static_cast<purge_node_t *>(run_thrs[i]->child);
        ut_ad(node->check_duplicate_undo_no());
        table_exists.push_back(node->is_table_id_exists(tid));
      }

      ptrdiff_t N = std::count(table_exists.begin(), table_exists.end(), true);
      ut_ad(N == 0 || N == 1);
    }
  }
#endif /* UNIV_DEBUG */

  ut_ad(trx_purge_check_limit());

  return (n_pages_handled);
}

/** Calculate the DML delay required.
 @return delay in microseconds or ULINT_MAX */
static ulint trx_purge_dml_delay(void) {
  /* Determine how much data manipulation language (DML) statements
  need to be delayed in order to reduce the lagging of the purge
  thread. */
  ulint delay = 0; /* in microseconds; default: no delay */

  /* If purge lag is set (ie. > 0) then calculate the new DML delay.
  Note: we do a dirty read of the trx_sys_t data structure here,
  without holding trx_sys->mutex. */

  if (srv_max_purge_lag > 0 &&
      trx_sys->rseg_history_len > srv_n_purge_threads * srv_purge_batch_size) {
    float ratio;

    ratio = float(trx_sys->rseg_history_len) / srv_max_purge_lag;

    if (ratio > 1.0) {
      /* If the history list length exceeds the srv_max_purge_lag, the data
      manipulation statements are delayed by at least 5 microseconds. */
      delay = (ulint)((ratio - 0.9995) * 10000);
    }

    if (delay > srv_max_purge_lag_delay) {
      delay = srv_max_purge_lag_delay;
    }

    MONITOR_SET(MONITOR_DML_PURGE_DELAY, delay);
  }

  return (delay);
}

/** Wait for pending purge jobs to complete. */
static void trx_purge_wait_for_workers_to_complete() {
  ulint i = 0;
  ulint n_submitted = purge_sys->n_submitted;

  /* Ensure that the work queue empties out. */
  while (!os_compare_and_swap_ulint(&purge_sys->n_completed, n_submitted,
                                    n_submitted)) {
    if (++i < 10) {
      os_thread_yield();
    } else {
      if (srv_get_task_queue_length() > 0) {
        srv_release_threads(SRV_WORKER, 1);
      }

      os_thread_sleep(20);
      i = 0;
    }
  }

  /* None of the worker threads should be doing any work. */
  ut_a(purge_sys->n_submitted == purge_sys->n_completed);

  /* There should be no outstanding tasks as long
  as the worker threads are active. */
  ut_a(srv_get_task_queue_length() == 0);
}

/** Remove old historical changes from the rollback segments. */
static void trx_purge_truncate(void) {
  ut_ad(trx_purge_check_limit());

  if (purge_sys->limit.scn == 0) {
    trx_purge_truncate_history(&purge_sys->iter, &purge_sys->vision);
  } else {
    trx_purge_truncate_history(&purge_sys->limit, &purge_sys->vision);
  }
}

/** This function runs a purge batch.
 @return number of undo log pages handled in the batch */
ulint trx_purge(ulint n_purge_threads, /*!< in: number of purge tasks
                                       to submit to the work queue */
                ulint batch_size,      /*!< in: the maximum number of records
                                       to purge in one batch */
                bool truncate,         /*!< in: truncate history if true */
                bool *blocked)         /*!< out: is blocked by retention */
{
  que_thr_t *thr = nullptr;
  ulint n_pages_handled;
  //scn_t old_scn;

  ut_a(n_purge_threads > 0);

  srv_dml_needed_delay = trx_purge_dml_delay();

  /* The number of tasks submitted should be completed. */
  ut_a(purge_sys->n_submitted == purge_sys->n_completed);

  rw_lock_x_lock(&purge_sys->latch);

  purge_sys->view_active = false;

  //old_scn = purge_sys->vision.snapshot_scn();
  lizard::trx_clone_oldest_vision(&purge_sys->vision);
  /* In the following special case, the purge view will back down:
  If all transactions are with no GTIDs within a GTID persistence cycle,
  and then one transation has GTID, Clone_persist_gtid will return an older
  scn which leading to purge view fallback.*/
  //ut_a(old_scn <= purge_sys->vision.snapshot_scn());

  purge_sys->view_active = true;

  rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG
  if (srv_purge_view_update_only_debug) {
    return (0);
  }
#endif /* UNIV_DEBUG */

  /* Fetch the UNDO recs that need to be purged. */
  n_pages_handled =
      trx_purge_attach_undo_recs(n_purge_threads, batch_size, blocked);

  /* Do we do an asynchronous purge or not ? */
  if (n_purge_threads > 1) {
    /* Submit the tasks to the work queue. */
    for (ulint i = 0; i < n_purge_threads - 1; ++i) {
      thr = que_fork_scheduler_round_robin(purge_sys->query, thr);

      ut_a(thr != nullptr);

      srv_que_task_enqueue_low(thr);
    }

    thr = que_fork_scheduler_round_robin(purge_sys->query, thr);
    ut_a(thr != nullptr);

    purge_sys->n_submitted += n_purge_threads - 1;

    goto run_synchronously;

    /* Do it synchronously. */
  } else {
    thr = que_fork_scheduler_round_robin(purge_sys->query, nullptr);
    ut_ad(thr);

  run_synchronously:
    ++purge_sys->n_submitted;

    que_run_threads(thr);

    os_atomic_inc_ulint(&purge_sys->pq_mutex, &purge_sys->n_completed, 1);

    if (n_purge_threads > 1) {
      trx_purge_wait_for_workers_to_complete();
    }
  }

  ut_a(purge_sys->n_submitted == purge_sys->n_completed);

#ifdef UNIV_DEBUG
  rw_lock_x_lock(&purge_sys->latch);
  if (purge_sys->limit.scn == 0) {
    purge_sys->done = purge_sys->iter;
  } else {
    purge_sys->done = purge_sys->limit;
  }
  rw_lock_x_unlock(&purge_sys->latch);
#endif /* UNIV_DEBUG */

  /* During upgrade, to know whether purge is empty,
  we rely on purge history length. So truncate the
  undo logs during upgrade to update purge history
  length. */
  if (truncate || srv_upgrade_old_undo_found) {
    trx_purge_truncate();
  }

  MONITOR_INC_VALUE(MONITOR_PURGE_INVOKED, 1);
  MONITOR_INC_VALUE(MONITOR_PURGE_N_PAGE_HANDLED, n_pages_handled);

  return (n_pages_handled);
}

/** Get the purge state.
 @return purge state. */
purge_state_t trx_purge_state(void) {
  purge_state_t state;

  rw_lock_x_lock(&purge_sys->latch);

  state = purge_sys->state;

  rw_lock_x_unlock(&purge_sys->latch);

  return (state);
}

/** Stop purge and wait for it to stop, move to PURGE_STATE_STOP. */
void trx_purge_stop(void) {
  purge_state_t state;
  int64_t sig_count = os_event_reset(purge_sys->event);

  ut_a(srv_n_purge_threads > 0);

  rw_lock_x_lock(&purge_sys->latch);

  ut_a(purge_sys->state != PURGE_STATE_INIT);
  ut_a(purge_sys->state != PURGE_STATE_EXIT);
  ut_a(purge_sys->state != PURGE_STATE_DISABLED);

  ++purge_sys->n_stop;

  state = purge_sys->state;

  if (state == PURGE_STATE_RUN) {
    ib::info(ER_IB_MSG_1181) << "Stopping purge";

    /* We need to wakeup the purge thread in case it is suspended,
    so that it can acknowledge the state change. */

    srv_purge_wakeup();
  }

  purge_sys->state = PURGE_STATE_STOP;

  rw_lock_x_unlock(&purge_sys->latch);

  if (state != PURGE_STATE_STOP) {
    /* Wait for purge coordinator to signal that it
    is suspended. */
    os_event_wait_low(purge_sys->event, sig_count);
  } else {
    bool once = true;

    rw_lock_x_lock(&purge_sys->latch);

    /* Wait for purge to signal that it has actually stopped. */
    while (purge_sys->running) {
      if (once) {
        ib::info(ER_IB_MSG_1182) << "Waiting for purge to stop";
        once = false;
      }

      rw_lock_x_unlock(&purge_sys->latch);

      os_thread_sleep(10000);

      rw_lock_x_lock(&purge_sys->latch);
    }

    rw_lock_x_unlock(&purge_sys->latch);
  }

  MONITOR_INC_VALUE(MONITOR_PURGE_STOP_COUNT, 1);
}

/** Resume purge, move to PURGE_STATE_RUN. */
void trx_purge_run(void) {
  /* Flush any GTIDs to disk so that purge can proceed immediately. */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  gtid_persistor.wait_flush(true, false, false, nullptr);

  rw_lock_x_lock(&purge_sys->latch);

  switch (purge_sys->state) {
    case PURGE_STATE_INIT:
    case PURGE_STATE_EXIT:
    case PURGE_STATE_DISABLED:
      ut_error;

    case PURGE_STATE_RUN:
    case PURGE_STATE_STOP:
      break;
  }

  if (purge_sys->n_stop > 0) {
    ut_a(purge_sys->state == PURGE_STATE_STOP);

    --purge_sys->n_stop;

    if (purge_sys->n_stop == 0) {
      ib::info(ER_IB_MSG_1183) << "Resuming purge";

      purge_sys->state = PURGE_STATE_RUN;
    }

    MONITOR_INC_VALUE(MONITOR_PURGE_RESUME_COUNT, 1);
  } else {
    ut_a(purge_sys->state == PURGE_STATE_RUN);
  }

  rw_lock_x_unlock(&purge_sys->latch);

  srv_purge_wakeup();
}

/** Initialize the undo::Tablespaces object. */
void undo::Tablespaces::init() {
  /** Fix the size of the vector so that it will not do
  allocations for inserts. This way the contents can be
  read without using a latch. */
  m_spaces.reserve(FSP_MAX_UNDO_TABLESPACES);

  m_latch = static_cast<rw_lock_t *>(ut_zalloc_nokey(sizeof(*m_latch)));

  rw_lock_create(undo_spaces_lock_key, m_latch, SYNC_UNDO_SPACES);

  mutex_create(LATCH_ID_UNDO_DDL, &ddl_mutex);
}

/** De-initialize the undo::Tablespaces object. */
void undo::Tablespaces::deinit() {
  clear();

  rw_lock_free(m_latch);
  ut_free(m_latch);
  m_latch = nullptr;
  mutex_free(&ddl_mutex);
}

/** Add a new space_id to the back of the vector.
The vector has been pre-allocated to 128 so read threads will
not loose what is pointed to. If tablespace_name and file_name
are standard names, they are optional.
@param[in]	ref_undo_space	undo tablespace */
void undo::Tablespaces::add(Tablespace &ref_undo_space, int pos) {
  ut_ad(is_reserved(ref_undo_space.id()));

  if (contains(ref_undo_space.num())) {
    return;
  }

  auto undo_space = UT_NEW_NOKEY(Tablespace(ref_undo_space));

  m_spaces.insert((pos < 0 || pos > (int)m_spaces.size())
                      ? m_spaces.end()
                      : m_spaces.begin() + pos,
                  undo_space);
}

/** Drop an existing explicit undo::Tablespace.
@param[in]	undo_space	pointer to undo space */
void undo::Tablespaces::drop(Tablespace *undo_space) {
  ut_ad(is_reserved(undo_space->id()));
  ut_ad(contains(undo_space->num()));

  for (auto it = m_spaces.begin(); it != m_spaces.end(); it++) {
    if (*it == undo_space) {
      UT_DELETE(undo_space);
      m_spaces.erase(it);
      break;
    }
  }
}

/** Drop an existing explicit undo::Tablespace.
@param[in]	ref_undo_space	reference to undo space */
void undo::Tablespaces::drop(Tablespace &ref_undo_space) {
  ut_ad(is_reserved(ref_undo_space.id()));

  for (auto it = m_spaces.begin(); it != m_spaces.end(); it++) {
    if ((*it)->id() == ref_undo_space.id()) {
      UT_DELETE(*it);
      m_spaces.erase(it);
      break;
    }
  }
}
