#
# basic xengine backup test with streaming and compression
#

require_xengine

start_server

FULL_BACKUP_CMD="xtrabackup
    --backup
    --extra-lsndir=$topdir/backuplsn
    --parallel=10
    --compress
    --compress-threads=10
    --target_dir=$topdir/backup_debug_sync
    --stream=xbstream >$topdir/backup.xbstream"

FULL_PREPARE_CMD="mkdir $mysql_datadir &&
  xbstream -C $mysql_datadir -x
     --parallel=10 --decompress < $topdir/backup.xbstream &&
  xtrabackup
    --prepare
    --apply-log-only
    --target-dir=$mysql_datadir"

CLEANUP_CMD="rm -rf $mysql_datadir"

RESTORE_CMD="echo"

pid_file=$topdir/backup_debug_sync/xtrabackup_debug_sync

. inc/xengine_common.sh
