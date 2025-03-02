/*****************************************************************************

Copyright (c) 2000, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2012, Facebook Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/
 
#define MYSQL_SERVER

#include <sql_table.h>	// explain_filename, nz2, EXPLAIN_PARTITIONS_AS_COMMENT,
			// EXPLAIN_FILENAME_MAX_EXTRA_LENGTH

#include <sql_acl.h>	// PROCESS_ACL
#include <debug_sync.h> // DEBUG_SYNC
#include <my_base.h>	// HA_OPTION_*
#include <mysys_err.h>
#include <mysql/innodb_priv.h>
#include <mysql/thread_pool_priv.h>
#include <my_check_opt.h>

/** @file ha_innodb.cc */

/* Include necessary InnoDB headers */
#include "univ.i"
#include "buf0dump.h"
#include "buf0lru.h"
#include "buf0flu.h"
#include "buf0dblwr.h"
#include "btr0sea.h"
#include "os0file.h"
#include "os0thread.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "trx0roll.h"
#include "trx0trx.h"

#include "trx0sys.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "row0sel.h"
#include "row0upd.h"
#include "log0log.h"
#include "log0online.h"
#include "lock0lock.h"
#include "dict0crea.h"
#include "btr0cur.h"
#include "btr0btr.h"
#include "fsp0fsp.h"
#include "sync0sync.h"
#include "fil0fil.h"
#include "trx0xa.h"
#include "row0merge.h"
#include "dict0boot.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "ha_prototypes.h"
#include "ut0mem.h"
#include "ibuf0ibuf.h"
#include "dict0dict.h"
#include "srv0mon.h"
#include "api0api.h"
#include "api0misc.h"
#include "pars0pars.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "row0import.h"
#include "row0quiesce.h"
#ifdef UNIV_DEBUG
#include "trx0purge.h"
#endif /* UNIV_DEBUG */
#include "fts0priv.h"
#include "page0zip.h"

#ifdef WITH_WSREP
#include "dict0priv.h"
#include "key.h"     // key_copy, key_unpack, key_cmp_if_same, key_cmp
#endif /* WITH_WSREP */
enum_tx_isolation thd_get_trx_isolation(const THD* thd);

#include "ha_innodb.h"
#include "i_s.h"
#include "xtradb_i_s.h"

# ifndef MYSQL_PLUGIN_IMPORT
#  define MYSQL_PLUGIN_IMPORT /* nothing */
# endif /* MYSQL_PLUGIN_IMPORT */
#ifdef WITH_WSREP
#include "../storage/innobase/include/ut0byte.h"
#include "wsrep_api.h"
#include <wsrep_mysqld.h>
#include <my_md5.h>
extern my_bool wsrep_certify_nonPK;
class  binlog_trx_data;
extern handlerton *binlog_hton;

extern MYSQL_PLUGIN_IMPORT mysql_mutex_t LOCK_wsrep_rollback;
extern MYSQL_PLUGIN_IMPORT mysql_cond_t COND_wsrep_rollback;
extern MYSQL_PLUGIN_IMPORT wsrep_aborting_thd_t wsrep_aborting_thd;

static inline wsrep_ws_handle_t*
wsrep_ws_handle(THD* thd, const trx_t* trx) {
	return wsrep_ws_handle_for_trx(wsrep_thd_ws_handle(thd),
				       (wsrep_trx_id_t)trx->id);
}

extern bool wsrep_prepare_key_for_innodb(const uchar *cache_key,
					 size_t cache_key_len,
                                         const uchar* row_id,
                                         size_t row_id_len,
                                         wsrep_buf_t* key,
                                         size_t* key_len);

extern handlerton * wsrep_hton;
extern TC_LOG* tc_log;
extern void wsrep_cleanup_transaction(THD *thd);
#endif /* WITH_WSREP */

/** to protect innobase_open_files */
static mysql_mutex_t innobase_share_mutex;
/** to force correct commit order in binlog */
static ulong commit_threads = 0;
static mysql_cond_t commit_cond;
static mysql_mutex_t commit_cond_m;
static bool innodb_inited = 0;

#define INSIDE_HA_INNOBASE_CC

#define EQ_CURRENT_THD(thd) ((thd) == current_thd)

static struct handlerton* innodb_hton_ptr;

static const long AUTOINC_OLD_STYLE_LOCKING = 0;
static const long AUTOINC_NEW_STYLE_LOCKING = 1;
static const long AUTOINC_NO_LOCKING = 2;

static long innobase_mirrored_log_groups;
static long innobase_log_buffer_size;
static long innobase_additional_mem_pool_size;
static long innobase_file_io_threads;
static long innobase_open_files;
static long innobase_autoinc_lock_mode;
static ulong innobase_commit_concurrency = 0;
static ulong innobase_read_io_threads;
static ulong innobase_write_io_threads;
static long innobase_buffer_pool_instances = 1;

static ulong innobase_log_block_size;

static long long innobase_buffer_pool_size, innobase_log_file_size;

/** Percentage of the buffer pool to reserve for 'old' blocks.
Connected to buf_LRU_old_ratio. */
static uint innobase_old_blocks_pct;

/** Maximum on-disk size of change buffer in terms of percentage
of the buffer pool. */
static uint innobase_change_buffer_max_size = CHANGE_BUFFER_DEFAULT_SIZE;

/* The default values for the following char* start-up parameters
are determined in innobase_init below: */

static char*	innobase_data_home_dir			= NULL;
static char*	innobase_data_file_path			= NULL;
static char*	innobase_file_format_name		= NULL;
static char*	innobase_change_buffering		= NULL;
static char*	innobase_enable_monitor_counter		= NULL;
static char*	innobase_disable_monitor_counter	= NULL;
static char*	innobase_reset_monitor_counter		= NULL;
static char*	innobase_reset_all_monitor_counter	= NULL;

/* The highest file format being used in the database. The value can be
set by user, however, it will be adjusted to the newer file format if
a table of such format is created/opened. */
static char*	innobase_file_format_max		= NULL;

static char*	innobase_file_flush_method		= NULL;

/* This variable can be set in the server configure file, specifying
stopword table to be used */
static char*	innobase_server_stopword_table		= NULL;

/* Below we have boolean-valued start-up parameters, and their default
values */

static ulong	innobase_fast_shutdown			= 1;
static my_bool	innobase_file_format_check		= TRUE;
#ifdef UNIV_LOG_ARCHIVE
static my_bool	innobase_log_archive			= FALSE;
static char*	innobase_log_arch_dir			= NULL;
#endif /* UNIV_LOG_ARCHIVE */
static my_bool	innobase_use_atomic_writes		= FALSE;
static my_bool	innobase_use_doublewrite		= TRUE;
static my_bool	innobase_use_checksums			= TRUE;
static my_bool	innobase_locks_unsafe_for_binlog	= FALSE;
static my_bool	innobase_rollback_on_timeout		= FALSE;
static my_bool	innobase_create_status_file		= FALSE;
static my_bool	innobase_stats_on_metadata		= TRUE;
static my_bool	innobase_large_prefix			= FALSE;
static my_bool	innodb_optimize_fulltext_only		= FALSE;

static char*	internal_innobase_data_file_path	= NULL;

static char*	innodb_version_str = (char*) INNODB_VERSION_STR;

/** Possible values for system variable "innodb_stats_method". The values
are defined the same as its corresponding MyISAM system variable
"myisam_stats_method"(see "myisam_stats_method_names"), for better usability */
static const char* innodb_stats_method_names[] = {
	"nulls_equal",
	"nulls_unequal",
	"nulls_ignored",
	NullS
};

/** Used to define an enumerate type of the system variable innodb_stats_method.
This is the same as "myisam_stats_method_typelib" */
static TYPELIB innodb_stats_method_typelib = {
	array_elements(innodb_stats_method_names) - 1,
	"innodb_stats_method_typelib",
	innodb_stats_method_names,
	NULL
};

/** Possible values for system variables "innodb_checksum_algorithm" and
"innodb_log_checksum_algorithm". */
static const char* innodb_checksum_algorithm_names[] = {
	"crc32",
	"strict_crc32",
	"innodb",
	"strict_innodb",
	"none",
	"strict_none",
	NullS
};

/** Used to define an enumerate type of the system variables
innodb_checksum_algorithm and innodb_log_checksum_algorithm. */
static TYPELIB innodb_checksum_algorithm_typelib = {
	array_elements(innodb_checksum_algorithm_names) - 1,
	"innodb_checksum_algorithm_typelib",
	innodb_checksum_algorithm_names,
	NULL
};

/** Possible values for system variable "innodb_cleaner_lsn_age_factor".  */
static const char* innodb_cleaner_lsn_age_factor_names[] = {
	"legacy",
	"high_checkpoint",
	NullS
};

/** Enumeration for innodb_cleaner_lsn_age_factor.  */
static TYPELIB innodb_cleaner_lsn_age_factor_typelib = {
	array_elements(innodb_cleaner_lsn_age_factor_names) - 1,
	"innodb_cleaner_lsn_age_factor_typelib",
	innodb_cleaner_lsn_age_factor_names,
	NULL
};

/** Possible values for system variable "innodb_foreground_preflush".  */
static const char* innodb_foreground_preflush_names[] = {
	"sync_preflush",
	"exponential_backoff",
	NullS
};

/* Enumeration for innodb_foreground_preflush.  */
static TYPELIB innodb_foreground_preflush_typelib = {
	array_elements(innodb_foreground_preflush_names) - 1,
	"innodb_foreground_preflush_typelib",
	innodb_foreground_preflush_names,
	NULL
};

/** Possible values for system variable "innodb_empty_free_list_algorithm".  */
static const char* innodb_empty_free_list_algorithm_names[] = {
	"legacy",
	"backoff",
	NullS
};

/** Enumeration for innodb_empty_free_list_algorithm.  */
static TYPELIB innodb_empty_free_list_algorithm_typelib = {
	array_elements(innodb_empty_free_list_algorithm_names) - 1,
	"innodb_empty_free_list_algorithm_typelib",
	innodb_empty_free_list_algorithm_names,
	NULL
};

/* The following counter is used to convey information to InnoDB
about server activity: in case of normal DML ops it is not
sensible to call srv_active_wake_master_thread after each
operation, we only do it every INNOBASE_WAKE_INTERVAL'th step. */

#define INNOBASE_WAKE_INTERVAL	32
static ulong	innobase_active_counter	= 0;

static hash_table_t*	innobase_open_tables;

/** Allowed values of innodb_change_buffering */
static const char* innobase_change_buffering_values[IBUF_USE_COUNT] = {
	"none",		/* IBUF_USE_NONE */
	"inserts",	/* IBUF_USE_INSERT */
	"deletes",	/* IBUF_USE_DELETE_MARK */
	"changes",	/* IBUF_USE_INSERT_DELETE_MARK */
	"purges",	/* IBUF_USE_DELETE */
	"all"		/* IBUF_USE_ALL */
};

/* Call back function array defined by MySQL and used to
retrieve FTS results. */
const struct _ft_vft ft_vft_result = {NULL,
				      innobase_fts_find_ranking,
				      innobase_fts_close_ranking,
				      innobase_fts_retrieve_ranking,
				      NULL};

const struct _ft_vft_ext ft_vft_ext_result = {innobase_fts_get_version,
					      innobase_fts_flags,
					      innobase_fts_retrieve_docid,
					      innobase_fts_count_matches};

#ifdef HAVE_PSI_INTERFACE
/* Keys to register pthread mutexes/cond in the current file with
performance schema */
static mysql_pfs_key_t	innobase_share_mutex_key;
static mysql_pfs_key_t	commit_cond_mutex_key;
static mysql_pfs_key_t	commit_cond_key;

static PSI_mutex_info	all_pthread_mutexes[] = {
	{&commit_cond_mutex_key, "commit_cond_mutex", 0},
	{&innobase_share_mutex_key, "innobase_share_mutex", 0}
};

static PSI_cond_info	all_innodb_conds[] = {
	{&commit_cond_key, "commit_cond", 0}
};

# ifdef UNIV_PFS_MUTEX
/* all_innodb_mutexes array contains mutexes that are
performance schema instrumented if "UNIV_PFS_MUTEX"
is defined */
static PSI_mutex_info all_innodb_mutexes[] = {
	{&autoinc_mutex_key, "autoinc_mutex", 0},
#  ifndef PFS_SKIP_BUFFER_MUTEX_RWLOCK
	{&buffer_block_mutex_key, "buffer_block_mutex", 0},
#  endif /* !PFS_SKIP_BUFFER_MUTEX_RWLOCK */
	{&buf_pool_zip_mutex_key, "buf_pool_zip_mutex", 0},
	{&buf_pool_LRU_list_mutex_key, "buf_pool_LRU_list_mutex", 0},
	{&buf_pool_free_list_mutex_key, "buf_pool_free_list_mutex", 0},
	{&buf_pool_zip_free_mutex_key, "buf_pool_zip_free_mutex", 0},
	{&buf_pool_zip_hash_mutex_key, "buf_pool_zip_hash_mutex", 0},
	{&buf_pool_flush_state_mutex_key, "buf_pool_flush_state_mutex", 0},
	{&cache_last_read_mutex_key, "cache_last_read_mutex", 0},
	{&dict_foreign_err_mutex_key, "dict_foreign_err_mutex", 0},
	{&dict_sys_mutex_key, "dict_sys_mutex", 0},
	{&file_format_max_mutex_key, "file_format_max_mutex", 0},
	{&fil_system_mutex_key, "fil_system_mutex", 0},
	{&flush_list_mutex_key, "flush_list_mutex", 0},
	{&fts_bg_threads_mutex_key, "fts_bg_threads_mutex", 0},
	{&fts_delete_mutex_key, "fts_delete_mutex", 0},
	{&fts_optimize_mutex_key, "fts_optimize_mutex", 0},
	{&fts_doc_id_mutex_key, "fts_doc_id_mutex", 0},
	{&fts_pll_tokenize_mutex_key, "fts_pll_tokenize_mutex", 0},
	{&log_flush_order_mutex_key, "log_flush_order_mutex", 0},
	{&hash_table_mutex_key, "hash_table_mutex", 0},
	{&ibuf_bitmap_mutex_key, "ibuf_bitmap_mutex", 0},
	{&ibuf_mutex_key, "ibuf_mutex", 0},
	{&ibuf_pessimistic_insert_mutex_key,
		 "ibuf_pessimistic_insert_mutex", 0},
#  ifndef HAVE_ATOMIC_BUILTINS
	{&server_mutex_key, "server_mutex", 0},
#  endif /* !HAVE_ATOMIC_BUILTINS */
	{&log_bmp_sys_mutex_key, "log_bmp_sys_mutex", 0},
	{&log_sys_mutex_key, "log_sys_mutex", 0},
#  ifdef UNIV_MEM_DEBUG
	{&mem_hash_mutex_key, "mem_hash_mutex", 0},
#  endif /* UNIV_MEM_DEBUG */
	{&mem_pool_mutex_key, "mem_pool_mutex", 0},
	{&mutex_list_mutex_key, "mutex_list_mutex", 0},
	{&page_zip_stat_per_index_mutex_key, "page_zip_stat_per_index_mutex", 0},
	{&purge_sys_bh_mutex_key, "purge_sys_bh_mutex", 0},
	{&recv_sys_mutex_key, "recv_sys_mutex", 0},
	{&recv_writer_mutex_key, "recv_writer_mutex", 0},
	{&rseg_mutex_key, "rseg_mutex", 0},
#  ifdef UNIV_SYNC_DEBUG
	{&rw_lock_debug_mutex_key, "rw_lock_debug_mutex", 0},
#  endif /* UNIV_SYNC_DEBUG */
	{&rw_lock_list_mutex_key, "rw_lock_list_mutex", 0},
	{&rw_lock_mutex_key, "rw_lock_mutex", 0},
	{&srv_dict_tmpfile_mutex_key, "srv_dict_tmpfile_mutex", 0},
	{&srv_innodb_monitor_mutex_key, "srv_innodb_monitor_mutex", 0},
	{&srv_misc_tmpfile_mutex_key, "srv_misc_tmpfile_mutex", 0},
	{&srv_monitor_file_mutex_key, "srv_monitor_file_mutex", 0},
#  ifdef UNIV_SYNC_DEBUG
	{&sync_thread_mutex_key, "sync_thread_mutex", 0},
#  endif /* UNIV_SYNC_DEBUG */
	{&buf_dblwr_mutex_key, "buf_dblwr_mutex", 0},
	{&trx_undo_mutex_key, "trx_undo_mutex", 0},
	{&srv_sys_mutex_key, "srv_sys_mutex", 0},
	{&lock_sys_mutex_key, "lock_mutex", 0},
	{&lock_sys_wait_mutex_key, "lock_wait_mutex", 0},
	{&trx_mutex_key, "trx_mutex", 0},
	{&srv_sys_tasks_mutex_key, "srv_threads_mutex", 0},
	/* mutex with os_fast_mutex_ interfaces */
#  ifndef PFS_SKIP_EVENT_MUTEX
	{&event_os_mutex_key, "event_os_mutex", 0},
#  endif /* PFS_SKIP_EVENT_MUTEX */
	{&os_mutex_key, "os_mutex", 0},
#ifndef HAVE_ATOMIC_BUILTINS
	{&srv_conc_mutex_key, "srv_conc_mutex", 0},
#endif /* !HAVE_ATOMIC_BUILTINS */
#ifndef HAVE_ATOMIC_BUILTINS_64
	{&monitor_mutex_key, "monitor_mutex", 0},
#endif /* !HAVE_ATOMIC_BUILTINS_64 */
	{&ut_list_mutex_key, "ut_list_mutex", 0},
	{&trx_sys_mutex_key, "trx_sys_mutex", 0},
	{&zip_pad_mutex_key, "zip_pad_mutex", 0},
};
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
/* all_innodb_rwlocks array contains rwlocks that are
performance schema instrumented if "UNIV_PFS_RWLOCK"
is defined */
static PSI_rwlock_info all_innodb_rwlocks[] = {
#  ifdef UNIV_LOG_ARCHIVE
	{&archive_lock_key, "archive_lock", 0},
#  endif /* UNIV_LOG_ARCHIVE */
	{&btr_search_latch_key, "btr_search_latch", 0},
#  ifndef PFS_SKIP_BUFFER_MUTEX_RWLOCK
	{&buf_block_lock_key, "buf_block_lock", 0},
#  endif /* !PFS_SKIP_BUFFER_MUTEX_RWLOCK */
#  ifdef UNIV_SYNC_DEBUG
	{&buf_block_debug_latch_key, "buf_block_debug_latch", 0},
#  endif /* UNIV_SYNC_DEBUG */
	{&dict_operation_lock_key, "dict_operation_lock", 0},
	{&fil_space_latch_key, "fil_space_latch", 0},
	{&checkpoint_lock_key, "checkpoint_lock", 0},
	{&fts_cache_rw_lock_key, "fts_cache_rw_lock", 0},
	{&fts_cache_init_rw_lock_key, "fts_cache_init_rw_lock", 0},
	{&trx_i_s_cache_lock_key, "trx_i_s_cache_lock", 0},
	{&trx_purge_latch_key, "trx_purge_latch", 0},
	{&index_tree_rw_lock_key, "index_tree_rw_lock", 0},
	{&index_online_log_key, "index_online_log", 0},
	{&dict_table_stats_key, "dict_table_stats", 0},
	{&hash_table_rw_lock_key, "hash_table_locks", 0}
};
# endif /* UNIV_PFS_RWLOCK */

# ifdef UNIV_PFS_THREAD
/* all_innodb_threads array contains threads that are
performance schema instrumented if "UNIV_PFS_THREAD"
is defined */
static PSI_thread_info	all_innodb_threads[] = {
	{&trx_rollback_clean_thread_key, "trx_rollback_clean_thread", 0},
	{&io_handler_thread_key, "io_handler_thread", 0},
	{&srv_lock_timeout_thread_key, "srv_lock_timeout_thread", 0},
	{&srv_error_monitor_thread_key, "srv_error_monitor_thread", 0},
	{&srv_monitor_thread_key, "srv_monitor_thread", 0},
	{&srv_master_thread_key, "srv_master_thread", 0},
	{&srv_purge_thread_key, "srv_purge_thread", 0},
	{&buf_page_cleaner_thread_key, "page_cleaner_thread", 0},
	{&buf_lru_manager_thread_key, "lru_manager_thread", 0},
	{&recv_writer_thread_key, "recv_writer_thread", 0},
	{&srv_log_tracking_thread_key, "srv_redo_log_follow_thread", 0}
};
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
/* all_innodb_files array contains the type of files that are
performance schema instrumented if "UNIV_PFS_IO" is defined */
static PSI_file_info	all_innodb_files[] = {
	{&innodb_file_data_key, "innodb_data_file", 0},
	{&innodb_file_log_key, "innodb_log_file", 0},
	{&innodb_file_temp_key, "innodb_temp_file", 0},
	{&innodb_file_bmp_key, "innodb_bmp_file", 0}
};
# endif /* UNIV_PFS_IO */
#endif /* HAVE_PSI_INTERFACE */

/** Always normalize table name to lower case on Windows */
#ifdef __WIN__
#define normalize_table_name(norm_name, name)           \
	normalize_table_name_low(norm_name, name, TRUE)
#else
#define normalize_table_name(norm_name, name)           \
	normalize_table_name_low(norm_name, name, FALSE)
#endif /* __WIN__ */

/** Set up InnoDB API callback function array */
ib_cb_t innodb_api_cb[] = {
	(ib_cb_t) ib_cursor_open_table,
	(ib_cb_t) ib_cursor_read_row,
	(ib_cb_t) ib_cursor_insert_row,
	(ib_cb_t) ib_cursor_delete_row,
	(ib_cb_t) ib_cursor_update_row,
	(ib_cb_t) ib_cursor_moveto,
	(ib_cb_t) ib_cursor_first,
	(ib_cb_t) ib_cursor_next,
	(ib_cb_t) ib_cursor_last,
	(ib_cb_t) ib_cursor_set_match_mode,
	(ib_cb_t) ib_sec_search_tuple_create,
	(ib_cb_t) ib_clust_read_tuple_create,
	(ib_cb_t) ib_tuple_delete,
	(ib_cb_t) ib_tuple_copy,
	(ib_cb_t) ib_tuple_read_u8,
	(ib_cb_t) ib_tuple_write_u8,
	(ib_cb_t) ib_tuple_read_u16,
	(ib_cb_t) ib_tuple_write_u16,
	(ib_cb_t) ib_tuple_read_u32,
	(ib_cb_t) ib_tuple_write_u32,
	(ib_cb_t) ib_tuple_read_u64,
	(ib_cb_t) ib_tuple_write_u64,
	(ib_cb_t) ib_tuple_read_i8,
	(ib_cb_t) ib_tuple_write_i8,
	(ib_cb_t) ib_tuple_read_i16,
	(ib_cb_t) ib_tuple_write_i16,
	(ib_cb_t) ib_tuple_read_i32,
	(ib_cb_t) ib_tuple_write_i32,
	(ib_cb_t) ib_tuple_read_i64,
	(ib_cb_t) ib_tuple_write_i64,
	(ib_cb_t) ib_tuple_get_n_cols,
	(ib_cb_t) ib_col_set_value,
	(ib_cb_t) ib_col_get_value,
	(ib_cb_t) ib_col_get_meta,
	(ib_cb_t) ib_trx_begin,
	(ib_cb_t) ib_trx_commit,
	(ib_cb_t) ib_trx_rollback,
	(ib_cb_t) ib_trx_start,
	(ib_cb_t) ib_trx_release,
	(ib_cb_t) ib_trx_state,
	(ib_cb_t) ib_cursor_lock,
	(ib_cb_t) ib_cursor_close,
	(ib_cb_t) ib_cursor_new_trx,
	(ib_cb_t) ib_cursor_reset,
	(ib_cb_t) ib_open_table_by_name,
	(ib_cb_t) ib_col_get_name,
	(ib_cb_t) ib_table_truncate,
	(ib_cb_t) ib_cursor_open_index_using_name,
	(ib_cb_t) ib_close_thd,
	(ib_cb_t) ib_cfg_get_cfg,
	(ib_cb_t) ib_cursor_set_memcached_sync,
	(ib_cb_t) ib_cursor_set_cluster_access,
	(ib_cb_t) ib_cursor_commit_trx,
	(ib_cb_t) ib_cfg_trx_level,
	(ib_cb_t) ib_tuple_get_n_user_cols,
	(ib_cb_t) ib_cursor_set_lock_mode,
	(ib_cb_t) ib_cursor_clear_trx,
	(ib_cb_t) ib_get_idx_field_name,
	(ib_cb_t) ib_trx_get_start_time,
	(ib_cb_t) ib_cfg_bk_commit_interval,
	(ib_cb_t) ib_cursor_stmt_begin,
	(ib_cb_t) ib_trx_read_only
};

/*************************************************************//**
Check whether valid argument given to innodb_ft_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

/** Validate passed-in "value" is a valid directory name.
This function is registered as a callback with MySQL.
@param[in,out]	thd	thread handle
@param[in]	var	pointer to system variable
@param[out]	save	immediate result for update
@param[in]	value	incoming string
@return 0 for valid name */
static
int
innodb_tmpdir_validate(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				save,
	struct st_mysql_value*		value)
{

	char*	alter_tmp_dir;
	char*	innodb_tmp_dir;
	char	buff[OS_FILE_MAX_PATH];
	int	len = sizeof(buff);
	char	tmp_abs_path[FN_REFLEN + 2];

	ut_ad(save != NULL);
	ut_ad(value != NULL);

	if (check_global_access(thd, FILE_ACL)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: FILE Permissions required");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	alter_tmp_dir = (char*) value->val_str(value, buff, &len);

	if (!alter_tmp_dir) {
		*static_cast<const char**>(save) = alter_tmp_dir;
		return(0);
	}

	if (strlen(alter_tmp_dir) > FN_REFLEN) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"Path length should not exceed %d bytes", FN_REFLEN);
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	my_realpath(tmp_abs_path, alter_tmp_dir, 0);
	size_t	tmp_abs_len = strlen(tmp_abs_path);

	if (my_access(tmp_abs_path, F_OK)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path doesn't exist.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	} else if (my_access(tmp_abs_path, R_OK | W_OK)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Server doesn't have permission in "
			"the given location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	MY_STAT stat_info_dir;

	if (my_stat(tmp_abs_path, &stat_info_dir, MYF(0))) {
		if ((stat_info_dir.st_mode & S_IFDIR) != S_IFDIR) {

			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_WRONG_ARGUMENTS,
				"Given path is not a directory. ");
			*static_cast<const char**>(save) = NULL;
			return(1);
		}
	}

	if (!is_mysql_datadir_path(tmp_abs_path)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path Location should not be same as "
			"mysql data directory location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	innodb_tmp_dir = static_cast<char*>(
		thd_memdup(thd, tmp_abs_path, tmp_abs_len + 1));
	*static_cast<const char**>(save) = innodb_tmp_dir;
	return(0);
}

/** "GEN_CLUST_INDEX" is the name reserved for InnoDB default
system clustered index when there is no primary key. */
const char innobase_index_reserve_name[] = "GEN_CLUST_INDEX";
/************************************************************//**
Synchronously read and parse the redo log up to the last
checkpoint to write the changed page bitmap.
@return 0 to indicate success.  Current implementation cannot fail. */
static
my_bool
innobase_flush_changed_page_bitmaps();
/*==================================*/
/************************************************************//**
Delete all the bitmap files for data less than the specified LSN.
If called with lsn == 0 (i.e. set by RESET request) or
IB_ULONGLONG_MAX, restart the bitmap file sequence, otherwise
continue it.
@return 0 to indicate success, 1 for failure. */
static
my_bool
innobase_purge_changed_page_bitmaps(
/*================================*/
	ulonglong lsn);	/*!< in: LSN to purge files up to */


/*****************************************************************//**
Check whether this is a fake change transaction.
@return TRUE if a fake change transaction */
static
my_bool
innobase_is_fake_change(
/*====================*/
	handlerton	*hton,  /*!< in: InnoDB handlerton */
	THD*		thd);	/*!< in: MySQL thread handle of the user for
				  whom the transaction is being committed */

/** Empty free list algorithm.
Checks if buffer pool is big enough to enable backoff algorithm.
InnoDB empty free list algorithm backoff requires free pages
from LRU for the best performance.
buf_LRU_buf_pool_running_out cancels query if 1/4 of
buffer pool belongs to LRU or freelist.
At the same time buf_flush_LRU_list_batch
keeps up to BUF_LRU_MIN_LEN in LRU.
In order to avoid deadlock baclkoff requires buffer pool
to be at least 4*BUF_LRU_MIN_LEN,
but flush peformance is bad because of trashing
and additional BUF_LRU_MIN_LEN pages are requested.
@param[in]	algorithm	desired algorithm from srv_empty_free_list_t
@return	true if it's possible to enable backoff. */
static inline
bool
innodb_empty_free_list_algorithm_allowed(
	srv_empty_free_list_t	algorithm)
{
	long long buf_pool_pages = srv_buf_pool_size / srv_page_size
				/ srv_buf_pool_instances;

	return(buf_pool_pages >= BUF_LRU_MIN_LEN * (4 + 1)
			|| algorithm != SRV_EMPTY_FREE_LIST_BACKOFF);
}

/** Get the list of foreign keys referencing a specified table
table.
@param thd		The thread handle
@param path		Path to the table
@param f_key_list[out]	The list of foreign keys

@return error code or zero for success */
static
int
innobase_get_parent_fk_list(
	THD*			thd,
	const char*		path,
	List<FOREIGN_KEY_INFO>*	f_key_list);

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return	InnoDB isolation level */
static inline
ulint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso);	/*!< in: MySQL isolation level code */

static const char innobase_hton_name[]= "InnoDB";

static MYSQL_THDVAR_BOOL(support_xa, PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB support for the XA two-phase commit",
  /* check_func */ NULL, /* update_func */ NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_BOOL(table_locks, PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB locking in LOCK TABLES",
  /* check_func */ NULL, /* update_func */ NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_BOOL(strict_mode, PLUGIN_VAR_OPCMDARG,
  "Use strict mode when evaluating create options.",
  NULL, NULL, FALSE);

static MYSQL_THDVAR_BOOL(ft_enable_stopword, PLUGIN_VAR_OPCMDARG,
  "Create FTS index with stopword.",
  NULL, NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_ULONG(lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
  "Timeout in seconds an InnoDB transaction may wait for a lock before being rolled back. Values above 100000000 disable the timeout.",
  NULL, NULL, 50, 1, 1024 * 1024 * 1024, 0);

static MYSQL_THDVAR_STR(ft_user_stopword_table,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "User supplied stopword table name, effective in the session level.",
  innodb_stopword_table_validate, NULL, NULL);

static MYSQL_THDVAR_ULONG(flush_log_at_trx_commit, PLUGIN_VAR_OPCMDARG,
  "Set to 0 (write and flush once per second),"
  " 1 (write and flush at each commit)"
  " or 2 (write at commit, flush once per second).",
  NULL, NULL, 1, 0, 2, 0);

static MYSQL_THDVAR_STR(tmpdir,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "Directory for temporary non-tablespace files.",
  innodb_tmpdir_validate, NULL, NULL);

static SHOW_VAR innodb_status_variables[]= {
  {"buffer_pool_dump_status",
  (char*) &export_vars.innodb_buffer_pool_dump_status,	  SHOW_CHAR},
  {"buffer_pool_load_status",
  (char*) &export_vars.innodb_buffer_pool_load_status,	  SHOW_CHAR},
  {"background_log_sync",
  (char*) &export_vars.innodb_background_log_sync,	  SHOW_LONG},
  {"buffer_pool_pages_data",
  (char*) &export_vars.innodb_buffer_pool_pages_data,	  SHOW_LONG},
  {"buffer_pool_bytes_data",
  (char*) &export_vars.innodb_buffer_pool_bytes_data,	  SHOW_LONG},
  {"buffer_pool_pages_dirty",
  (char*) &export_vars.innodb_buffer_pool_pages_dirty,	  SHOW_LONG},
  {"buffer_pool_bytes_dirty",
  (char*) &export_vars.innodb_buffer_pool_bytes_dirty,	  SHOW_LONG},
  {"buffer_pool_pages_flushed",
  (char*) &export_vars.innodb_buffer_pool_pages_flushed,  SHOW_LONG},
  {"buffer_pool_pages_LRU_flushed",
  (char*) &export_vars.innodb_buffer_pool_pages_LRU_flushed,  SHOW_LONG},
  {"buffer_pool_pages_free",
  (char*) &export_vars.innodb_buffer_pool_pages_free,	  SHOW_LONG},
#ifdef UNIV_DEBUG
  {"buffer_pool_pages_latched",
  (char*) &export_vars.innodb_buffer_pool_pages_latched,  SHOW_LONG},
#endif /* UNIV_DEBUG */
  {"buffer_pool_pages_made_not_young",
  (char*) &export_vars.innodb_buffer_pool_pages_made_not_young, SHOW_LONG},
  {"buffer_pool_pages_made_young",
  (char*) &export_vars.innodb_buffer_pool_pages_made_young, SHOW_LONG},
  {"buffer_pool_pages_misc",
  (char*) &export_vars.innodb_buffer_pool_pages_misc,	  SHOW_LONG},
  {"buffer_pool_pages_old",
  (char*) &export_vars.innodb_buffer_pool_pages_old,	  SHOW_LONG},
  {"buffer_pool_pages_total",
  (char*) &export_vars.innodb_buffer_pool_pages_total,	  SHOW_LONG},
  {"buffer_pool_read_ahead_rnd",
  (char*) &export_vars.innodb_buffer_pool_read_ahead_rnd, SHOW_LONG},
  {"buffer_pool_read_ahead",
  (char*) &export_vars.innodb_buffer_pool_read_ahead,	  SHOW_LONG},
  {"buffer_pool_read_ahead_evicted",
  (char*) &export_vars.innodb_buffer_pool_read_ahead_evicted, SHOW_LONG},
  {"buffer_pool_read_requests",
  (char*) &export_vars.innodb_buffer_pool_read_requests,  SHOW_LONG},
  {"buffer_pool_reads",
  (char*) &export_vars.innodb_buffer_pool_reads,	  SHOW_LONG},
  {"buffer_pool_wait_free",
  (char*) &export_vars.innodb_buffer_pool_wait_free,	  SHOW_LONG},
  {"buffer_pool_write_requests",
  (char*) &export_vars.innodb_buffer_pool_write_requests, SHOW_LONG},
  {"checkpoint_age",
  (char*) &export_vars.innodb_checkpoint_age,		  SHOW_LONG},
  {"checkpoint_max_age",
  (char*) &export_vars.innodb_checkpoint_max_age,	  SHOW_LONG},
  {"data_fsyncs",
  (char*) &export_vars.innodb_data_fsyncs,		  SHOW_LONG},
  {"data_pending_fsyncs",
  (char*) &export_vars.innodb_data_pending_fsyncs,	  SHOW_LONG},
  {"data_pending_reads",
  (char*) &export_vars.innodb_data_pending_reads,	  SHOW_LONG},
  {"data_pending_writes",
  (char*) &export_vars.innodb_data_pending_writes,	  SHOW_LONG},
  {"data_read",
  (char*) &export_vars.innodb_data_read,		  SHOW_LONG},
  {"data_reads",
  (char*) &export_vars.innodb_data_reads,		  SHOW_LONG},
  {"data_writes",
  (char*) &export_vars.innodb_data_writes,		  SHOW_LONG},
  {"data_written",
  (char*) &export_vars.innodb_data_written,		  SHOW_LONG},
  {"dblwr_pages_written",
  (char*) &export_vars.innodb_dblwr_pages_written,	  SHOW_LONG},
  {"dblwr_writes",
  (char*) &export_vars.innodb_dblwr_writes,		  SHOW_LONG},
  {"deadlocks",
  (char*) &export_vars.innodb_deadlocks,		  SHOW_LONG},
  {"have_atomic_builtins",
  (char*) &export_vars.innodb_have_atomic_builtins,	  SHOW_BOOL},
  {"history_list_length",
  (char*) &export_vars.innodb_history_list_length,	  SHOW_LONG},
  {"ibuf_discarded_delete_marks",
  (char*) &export_vars.innodb_ibuf_discarded_delete_marks, SHOW_LONG},
  {"ibuf_discarded_deletes",
  (char*) &export_vars.innodb_ibuf_discarded_deletes,	  SHOW_LONG},
  {"ibuf_discarded_inserts",
  (char*) &export_vars.innodb_ibuf_discarded_inserts,	  SHOW_LONG},
  {"ibuf_free_list",
  (char*) &export_vars.innodb_ibuf_free_list,		  SHOW_LONG},
  {"ibuf_merged_delete_marks",
  (char*) &export_vars.innodb_ibuf_merged_delete_marks,	  SHOW_LONG},
  {"ibuf_merged_deletes",
  (char*) &export_vars.innodb_ibuf_merged_deletes,	  SHOW_LONG},
  {"ibuf_merged_inserts",
  (char*) &export_vars.innodb_ibuf_merged_inserts,	  SHOW_LONG},
  {"ibuf_merges",
  (char*) &export_vars.innodb_ibuf_merges,		  SHOW_LONG},
  {"ibuf_segment_size",
  (char*) &export_vars.innodb_ibuf_segment_size,	  SHOW_LONG},
  {"ibuf_size",
  (char*) &export_vars.innodb_ibuf_size,		  SHOW_LONG},
  {"log_waits",
  (char*) &export_vars.innodb_log_waits,		  SHOW_LONG},
  {"log_write_requests",
  (char*) &export_vars.innodb_log_write_requests,	  SHOW_LONG},
  {"log_writes",
  (char*) &export_vars.innodb_log_writes,		  SHOW_LONG},
  {"lsn_current",
  (char*) &export_vars.innodb_lsn_current,		  SHOW_LONGLONG},
  {"lsn_flushed",
  (char*) &export_vars.innodb_lsn_flushed,		  SHOW_LONGLONG},
  {"lsn_last_checkpoint",
  (char*) &export_vars.innodb_lsn_last_checkpoint,	  SHOW_LONGLONG},
  {"master_thread_active_loops",
  (char*) &export_vars.innodb_master_thread_active_loops, SHOW_LONG},
  {"master_thread_idle_loops",
  (char*) &export_vars.innodb_master_thread_idle_loops,   SHOW_LONG},
  {"max_trx_id",
  (char*) &export_vars.innodb_max_trx_id,		  SHOW_LONGLONG},
  {"mem_adaptive_hash",
  (char*) &export_vars.innodb_mem_adaptive_hash,	  SHOW_LONG},
  {"mem_dictionary",
  (char*) &export_vars.innodb_mem_dictionary,		  SHOW_LONG},
  {"mem_total",
  (char*) &export_vars.innodb_mem_total,		  SHOW_LONG},
  {"mutex_os_waits",
  (char*) &export_vars.innodb_mutex_os_waits,		  SHOW_LONGLONG},
  {"mutex_spin_rounds",
  (char*) &export_vars.innodb_mutex_spin_rounds,	  SHOW_LONGLONG},
  {"mutex_spin_waits",
  (char*) &export_vars.innodb_mutex_spin_waits,		  SHOW_LONGLONG},
  {"oldest_view_low_limit_trx_id",
  (char*) &export_vars.innodb_oldest_view_low_limit_trx_id, SHOW_LONGLONG},
  {"os_log_fsyncs",
  (char*) &export_vars.innodb_os_log_fsyncs,		  SHOW_LONG},
  {"os_log_pending_fsyncs",
  (char*) &export_vars.innodb_os_log_pending_fsyncs,	  SHOW_LONG},
  {"os_log_pending_writes",
  (char*) &export_vars.innodb_os_log_pending_writes,	  SHOW_LONG},
  {"os_log_written",
  (char*) &export_vars.innodb_os_log_written,		  SHOW_LONGLONG},
  {"page_size",
  (char*) &export_vars.innodb_page_size,		  SHOW_LONG},
  {"pages_created",
  (char*) &export_vars.innodb_pages_created,		  SHOW_LONG},
  {"pages_read",
  (char*) &export_vars.innodb_pages_read,		  SHOW_LONG},
  {"pages_written",
  (char*) &export_vars.innodb_pages_written,		  SHOW_LONG},
  {"purge_trx_id",
  (char*) &export_vars.innodb_purge_trx_id,		  SHOW_LONGLONG},
  {"purge_undo_no",
  (char*) &export_vars.innodb_purge_undo_no,		  SHOW_LONGLONG},
  {"row_lock_current_waits",
  (char*) &export_vars.innodb_row_lock_current_waits,	  SHOW_LONG},
  {"current_row_locks",
  (char*) &export_vars.innodb_current_row_locks,		  SHOW_LONG},
  {"row_lock_time",
  (char*) &export_vars.innodb_row_lock_time,		  SHOW_LONGLONG},
  {"row_lock_time_avg",
  (char*) &export_vars.innodb_row_lock_time_avg,	  SHOW_LONG},
  {"row_lock_time_max",
  (char*) &export_vars.innodb_row_lock_time_max,	  SHOW_LONG},
  {"row_lock_waits",
  (char*) &export_vars.innodb_row_lock_waits,		  SHOW_LONG},
  {"rows_deleted",
  (char*) &export_vars.innodb_rows_deleted,		  SHOW_LONG},
  {"rows_inserted",
  (char*) &export_vars.innodb_rows_inserted,		  SHOW_LONG},
  {"rows_read",
  (char*) &export_vars.innodb_rows_read,		  SHOW_LONG},
  {"rows_updated",
  (char*) &export_vars.innodb_rows_updated,		  SHOW_LONG},
  {"num_open_files",
  (char*) &export_vars.innodb_num_open_files,		  SHOW_LONG},
  {"read_views_memory",
  (char*) &export_vars.innodb_read_views_memory,	  SHOW_LONG},
  {"descriptors_memory",
  (char*) &export_vars.innodb_descriptors_memory,	  SHOW_LONG},
  {"s_lock_os_waits",
  (char*) &export_vars.innodb_s_lock_os_waits,		  SHOW_LONGLONG},
  {"s_lock_spin_rounds",
  (char*) &export_vars.innodb_s_lock_spin_rounds,	  SHOW_LONGLONG},
  {"s_lock_spin_waits",
  (char*) &export_vars.innodb_s_lock_spin_waits,	  SHOW_LONGLONG},
  {"truncated_status_writes",
  (char*) &export_vars.innodb_truncated_status_writes,	  SHOW_LONG},
  {"available_undo_logs",
  (char*) &export_vars.innodb_available_undo_logs,        SHOW_LONG},
#ifdef UNIV_DEBUG
  {"purge_trx_id_age",
  (char*) &export_vars.innodb_purge_trx_id_age,           SHOW_LONG},
  {"purge_view_trx_id_age",
  (char*) &export_vars.innodb_purge_view_trx_id_age,      SHOW_LONG},
#endif /* UNIV_DEBUG */
  {"x_lock_os_waits",
  (char*) &export_vars.innodb_x_lock_os_waits,		  SHOW_LONGLONG},
  {"x_lock_spin_rounds",
  (char*) &export_vars.innodb_x_lock_spin_rounds,	  SHOW_LONGLONG},
  {"x_lock_spin_waits",
  (char*) &export_vars.innodb_x_lock_spin_waits,	  SHOW_LONGLONG},
  {"secondary_index_triggered_cluster_reads",
  (char*) &export_vars.innodb_sec_rec_cluster_reads,	  SHOW_LONG},
  {"secondary_index_triggered_cluster_reads_avoided",
  (char*) &export_vars.innodb_sec_rec_cluster_reads_avoided, SHOW_LONG},
  {"buffered_aio_submitted",
  (char*) &export_vars.innodb_buffered_aio_submitted,	  SHOW_LONG},

  {"scan_pages_contiguous",
  (char*) &export_vars.innodb_fragmentation_stats.scan_pages_contiguous,
  SHOW_LONG},
  {"scan_pages_disjointed",
  (char*) &export_vars.innodb_fragmentation_stats.scan_pages_disjointed,
  SHOW_LONG},
  {"scan_pages_total_seek_distance",
  (char*) &export_vars.innodb_fragmentation_stats.scan_pages_total_seek_distance,
  SHOW_LONG},
  {"scan_data_size",
  (char*) &export_vars.innodb_fragmentation_stats.scan_data_size,
  SHOW_LONG},
  {"scan_deleted_recs_size",
  (char*) &export_vars.innodb_fragmentation_stats.scan_deleted_recs_size,
  SHOW_LONG},
  {NullS, NullS, SHOW_LONG}
};

/************************************************************************//**
Handling the shared INNOBASE_SHARE structure that is needed to provide table
locking. Register the table name if it doesn't exist in the hash table. */
static
INNOBASE_SHARE*
get_share(
/*======*/
	const char*	table_name);	/*!< in: table to lookup */

/************************************************************************//**
Free the shared object that was registered with get_share(). */
static
void
free_share(
/*=======*/
	INNOBASE_SHARE*	share);		/*!< in/own: share to free */

/*****************************************************************//**
Frees a possible InnoDB trx object associated with the current THD.
@return	0 or error number */
static
int
innobase_close_connection(
/*======================*/
	handlerton*	hton,		/*!< in/out: Innodb handlerton */
	THD*		thd);		/*!< in: MySQL thread handle for
					which to close the connection */

/*****************************************************************//**
Cancel any pending lock request associated with the current THD. */
static
void
innobase_kill_connection(
/*======================*/
        handlerton*	hton,	/*!< in:  innobase handlerton */
	THD*	thd);	/*!< in: handle to the MySQL thread being killed */

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return	0 */
static
int
innobase_commit(
/*============*/
	handlerton*	hton,		/*!< in/out: Innodb handlerton */
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx);	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback(
/*==============*/
	handlerton*	hton,		/*!< in/out: Innodb handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx);	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */
	void*		savepoint);	/*!< in: savepoint data */

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd);		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */

/*****************************************************************//**
Sets a transaction savepoint.
@return	always 0, that is, always succeeds */
static
int
innobase_savepoint(
/*===============*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user's XA transaction for which
					we need to take a savepoint */
	void*		savepoint);	/*!< in: savepoint data */

/*****************************************************************//**
Release transaction savepoint name.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_release_savepoint(
/*=======================*/
	handlerton*	hton,		/*!< in/out: handlerton for Innodb */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction's
					savepoint should be released */
	void*		savepoint);	/*!< in: savepoint data */

/************************************************************************//**
Function for constructing an InnoDB table handler instance. */
static
handler*
innobase_create_handler(
/*====================*/
	handlerton*	hton,		/*!< in/out: handlerton for Innodb */
	TABLE_SHARE*	table,
	MEM_ROOT*	mem_root);

/** @brief Initialize the default value of innodb_commit_concurrency.

Once InnoDB is running, the innodb_commit_concurrency must not change
from zero to nonzero. (Bug #42101)

The initial default value is 0, and without this extra initialization,
SET GLOBAL innodb_commit_concurrency=DEFAULT would set the parameter
to 0, even if it was initially set to nonzero at the command line
or configuration file. */
static
void
innobase_commit_concurrency_init_default();
/*=======================================*/

/** @brief Initialize the default and max value of innodb_undo_logs.

Once InnoDB is running, the default value and the max value of
innodb_undo_logs must be equal to the available undo logs,
given by srv_available_undo_logs. */
static
void
innobase_undo_logs_init_default_max();
/*==================================*/

/************************************************************//**
Validate the file format name and return its corresponding id.
@return	valid file format id */
static
uint
innobase_file_format_name_lookup(
/*=============================*/
	const char*	format_name);	/*!< in: pointer to file format
					name */
/************************************************************//**
Validate the file format check config parameters, as a side effect it
sets the srv_max_file_format_at_startup variable.
@return	the format_id if valid config value, otherwise, return -1 */
static
int
innobase_file_format_validate_and_set(
/*==================================*/
	const char*	format_max);	/*!< in: parameter value */

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return	0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		all);		/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return	number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	XID*		xid_list,	/*!< in/out: prepared transactions */
	uint		len);		/*!< in: number of slots in xid_list */
/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return	0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	XID*		xid);		/*!< in: X/Open XA transaction
					identification */
/*******************************************************************//**
This function is used to rollback one X/Open XA distributed transaction
which is in the prepared state
@return	0 or error number */
static
int
innobase_rollback_by_xid(
/*=====================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	XID*		xid);		/*!< in: X/Open XA transaction
					identification */
/*******************************************************************//**
Create a consistent view for a cursor based on current transaction
which is created if the corresponding MySQL thread still lacks one.
This consistent view is then used inside of MySQL when accessing records
using a cursor.
@return	pointer to cursor view or NULL */
static
void*
innobase_create_cursor_view(
/*========================*/
	handlerton*	hton,		/*!< in: innobase hton */
	THD*		thd);		/*!< in: user thread handle */
/*******************************************************************//**
Set the given consistent cursor view to a transaction which is created
if the corresponding MySQL thread still lacks one. If the given
consistent cursor view is NULL global read view of a transaction is
restored to a transaction read view. */
static
void
innobase_set_cursor_view(
/*=====================*/
	handlerton*	hton,		/*!< in: handlerton of Innodb */
	THD*		thd,		/*!< in: user thread handle */
	void*		curview);	/*!< in: Consistent cursor view to
					be set */
/*******************************************************************//**
Close the given consistent cursor view of a transaction and restore
global read view to a transaction read view. Transaction is created if the
corresponding MySQL thread still lacks one. */
static
void
innobase_close_cursor_view(
/*=======================*/
	handlerton*	hton,		/*!< in: handlerton of Innodb */
	THD*		thd,		/*!< in: user thread handle */
	void*		curview);	/*!< in: Consistent read view to be
					closed */
/*****************************************************************//**
Removes all tables in the named database inside InnoDB. */
static
void
innobase_drop_database(
/*===================*/
	handlerton*	hton,		/*!< in: handlerton of Innodb */
	char*		path);		/*!< in: database path; inside InnoDB
					the name of the last directory in
					the path is used as the database name:
					for example, in 'mysql/data/test' the
					database name is 'test' */
/*******************************************************************//**
Closes an InnoDB database. */
static
int
innobase_end(
/*=========*/
	handlerton*		hton,	/* in: Innodb handlerton */
	ha_panic_function	type);

/*****************************************************************//**
Stores the current binlog coordinates in the trx system header. */
static
int
innobase_store_binlog_info(
/*=======================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	THD*		thd);	/*!< in: MySQL thread handle */

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return	0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	handlerton*	hton,		/* in: Innodb handlerton */
	THD*		thd);		/* in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
clones snapshot for a consistent read from another session, if it has one.
@return	0 */
static
int
innobase_start_trx_and_clone_read_view(
/*====================================*/
	handlerton*	hton,		/* in: Innodb handlerton */
	THD*		thd,		/* in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	THD*		from_thd);	/* in: MySQL thread handle of the
					user session from which the consistent
					read should be cloned */
/****************************************************************//**
Flushes InnoDB logs to disk and makes a checkpoint. Really, a commit flushes
the logs, and the name of this function should be innobase_checkpoint.
@return	TRUE if error */
static
bool
innobase_flush_logs(
/*================*/
	handlerton*	hton);		/*!< in: InnoDB handlerton */

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,		/*!< in: the innodb handlerton */
	THD*		thd,		/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*	stat_print);
/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type);

/*****************************************************************//**
Commits a transaction in an InnoDB database. */
static
void
innobase_commit_low(
/*================*/
	trx_t*	trx);	/*!< in: transaction handle */

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can enable monitor counters/groups by specifying
"loose-innodb_monitor_enable = monitor_name1;monitor_name2..."
in server configuration file or at the command line. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str);	/*!< in: monitor counter enable list */

/*********************************************************************
Normalizes a table name string. A normalized name consists of the
database name catenated to '/' and table name. An example:
test/mytable. On Windows normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE. */
static
void
normalize_table_name_low(
/*=====================*/
	char*           norm_name,      /* out: normalized name as a
					null-terminated string */
	const char*     name,           /* in: table name string */
	ibool           set_lower_case); /* in: TRUE if we want to set
					 name to lower case */

/*****************************************************************//**
Checks if the filename name is reserved in InnoDB.
@return true if the name is reserved */
static
bool
innobase_check_reserved_file_name(
/*===================*/
        handlerton*     hton,           /*!< in: handlerton of Innodb */
        const char*     name);          /*!< in: Name of the database */


/** Creates a new compression dictionary. */
static
handler_create_zip_dict_result
innobase_create_zip_dict(
	handlerton*	hton,	/*!< in: innobase handlerton */
	THD*		thd,	/*!< in: handle to the MySQL thread */
	const char*	name,	/*!< in: zip dictionary name */
	ulint*		name_len,
				/*!< in/out: zip dictionary name length */
	const char*	data,	/*!< in: zip dictionary data */
	ulint*		data_len);
				/*!< in/out: zip dictionary data length */

/** Drops a existing compression dictionary. */
static
handler_drop_zip_dict_result
innobase_drop_zip_dict(
	handlerton*	hton,	/*!< in: innobase handlerton */
	THD*		thd,	/*!< in: handle to the MySQL thread */
	const char*	name,	/*!< in: zip dictionary name */
	ulint*		name_len);
				/*!< in/out: zip dictionary name length */

/*************************************************************//**
Removes old archived transaction log files.
@return	true on error */
static bool innobase_purge_archive_logs(
	handlerton *hton,		/*!< in: InnoDB handlerton */
	time_t before_date,		/*!< in: all files modified
					before timestamp should be removed */
	const char* to_filename)	/*!< in: this and earler files
					should be removed */
{
	ulint err= DB_ERROR;
	if (before_date > 0) {
		err= purge_archived_logs(before_date, 0);
	} else if (to_filename) {
		if (is_prefix(to_filename, IB_ARCHIVED_LOGS_PREFIX)) {
			unsigned long long log_file_lsn = strtoll(to_filename
					+ IB_ARCHIVED_LOGS_PREFIX_LEN,
					NULL, 10);
			if (log_file_lsn > 0 && log_file_lsn < ULLONG_MAX) {
				err= purge_archived_logs(0, log_file_lsn);
			}
		}
	}
	return (err != DB_SUCCESS);
}

/** Sync innodb_kill_idle_transaction and kill_idle_transaction values.

@param[in,out]	thd	thread handle
@param[in]	var	pointer to system variable
@param[out]	var_ptr	where the formal string goes
@param[in]	save	immediate result from check function */
static void innodb_kill_idle_transaction_update(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save)
{
	ulong	in_val = *static_cast<const long*>(save);
	kill_idle_transaction_timeout= in_val;
	srv_kill_idle_transaction= in_val;
}

/*************************************************************//**
Check for a valid value of innobase_commit_concurrency.
@return	0 for valid innodb_commit_concurrency */
static
int
innobase_commit_concurrency_validate(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	long long	intbuf;
	ulong		commit_concurrency;

	DBUG_ENTER("innobase_commit_concurrency_validate");

	if (value->val_int(value, &intbuf)) {
		/* The value is NULL. That is invalid. */
		DBUG_RETURN(1);
	}

	*reinterpret_cast<ulong*>(save) = commit_concurrency
		= static_cast<ulong>(intbuf);

	/* Allow the value to be updated, as long as it remains zero
	or nonzero. */
	DBUG_RETURN(!(!commit_concurrency == !innobase_commit_concurrency));
}

/*******************************************************************//**
Function for constructing an InnoDB table handler instance. */
static
handler*
innobase_create_handler(
/*====================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	TABLE_SHARE*	table,
	MEM_ROOT*	mem_root)
{
	return(new (mem_root) ha_innobase(hton, table));
}

/* General functions */

/*************************************************************//**
Check that a page_size is correct for InnoDB.  If correct, set the
associated page_size_shift which is the power of 2 for this page size.
@return	an associated page_size_shift if valid, 0 if invalid. */
inline
int
innodb_page_size_validate(
/*======================*/
	ulong	page_size)		/*!< in: Page Size to evaluate */
{
	DBUG_ENTER("innodb_page_size_validate");

#ifdef WITH_WSREP
	/* With a 4K page_size, Galera triggers an assert upon restart.
	 * Until that gets resolved, require using the default page size (16K).
	*/
	if (page_size == UNIV_PAGE_SIZE_ORIG) {
		DBUG_RETURN(UNIV_PAGE_SIZE_SHIFT_ORIG);
	}
#else
	ulong		n;

	for (n = UNIV_PAGE_SIZE_SHIFT_MIN;
	     n <= UNIV_PAGE_SIZE_SHIFT_MAX;
	     n++) {
		if (page_size == (ulong) (1 << n)) {
			DBUG_RETURN(n);
		}
	}
#endif /* WITH_WSREP */
	DBUG_RETURN(0);
}

/******************************************************************//**
Returns true if the thread is the replication thread on the slave
server. Used in srv_conc_enter_innodb() to determine if the thread
should be allowed to enter InnoDB - the replication thread is treated
differently than other threads. Also used in
srv_conc_force_exit_innodb().
@return	true if thd is the replication thread */
UNIV_INTERN
ibool
thd_is_replication_slave_thread(
/*============================*/
	THD*	thd)	/*!< in: thread handle */
{
	return((ibool) thd_slave_thread(thd));
}

/******************************************************************//**
Gets information on the durability property requested by thread.
Used when writing either a prepare or commit record to the log
buffer. @return the durability property. */
UNIV_INTERN
enum durability_properties
thd_requested_durability(
/*=====================*/
	const THD* thd)	/*!< in: thread handle */
{
	return(thd_get_durability_property(thd));
}

/******************************************************************//**
Returns true if transaction should be flagged as read-only.
@return	true if the thd is marked as read-only */
UNIV_INTERN
ibool
thd_trx_is_read_only(
/*=================*/
	THD*	thd)	/*!< in: thread handle */
{
	return(thd != 0 && thd_tx_is_read_only(thd));
}

/******************************************************************//**
Check if the transaction is an auto-commit transaction. TRUE also
implies that it is a SELECT (read-only) transaction.
@return	true if the transaction is an auto commit read-only transaction. */
UNIV_INTERN
ibool
thd_trx_is_auto_commit(
/*===================*/
	THD*	thd)	/*!< in: thread handle, can be NULL */
{
	return(thd != NULL
	       && !thd_test_options(
		       thd,
		       OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)
	       && thd_is_select(thd));
}

/******************************************************************//**
Save some CPU by testing the value of srv_thread_concurrency in inline
functions. */
static inline
void
innobase_srv_conc_enter_innodb(
/*===========================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef WITH_WSREP
	if (wsrep_on(trx->mysql_thd) && 
	    wsrep_thd_is_BF(trx->mysql_thd, FALSE)) return;
#endif /* WITH_WSREP */
	if (srv_thread_concurrency) {
		if (trx->n_tickets_to_enter_innodb > 0) {

			/* If trx has 'free tickets' to enter the engine left,
			then use one such ticket */

			--trx->n_tickets_to_enter_innodb;

		} else if (trx->mysql_thd != NULL
			   && thd_is_replication_slave_thread(trx->mysql_thd)) {

			UT_WAIT_FOR(
				srv_conc_get_active_threads()
				< srv_thread_concurrency,
				srv_replication_delay * 1000);

		}  else {
			srv_conc_enter_innodb(trx);
		}
	}
}

/******************************************************************//**
Note that the thread wants to leave InnoDB only if it doesn't have
any spare tickets. */
static inline
void
innobase_srv_conc_exit_innodb(
/*==========================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!sync_thread_levels_nonempty_trx(trx->has_search_latch));
#endif /* UNIV_SYNC_DEBUG */
#ifdef WITH_WSREP
	if (wsrep_on(trx->mysql_thd) && 
	    wsrep_thd_is_BF(trx->mysql_thd, FALSE)) return;
#endif /* WITH_WSREP */

	/* This is to avoid making an unnecessary function call. */
	if (trx->declared_to_be_inside_innodb
	    && trx->n_tickets_to_enter_innodb == 0) {

		srv_conc_force_exit_innodb(trx);
	}
}

/******************************************************************//**
Force a thread to leave InnoDB even if it has spare tickets. */
static inline
void
innobase_srv_conc_force_exit_innodb(
/*================================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!sync_thread_levels_nonempty_trx(trx->has_search_latch));
#endif /* UNIV_SYNC_DEBUG */

	/* This is to avoid making an unnecessary function call. */
	if (trx->declared_to_be_inside_innodb) {
		srv_conc_force_exit_innodb(trx);
	}
}

/******************************************************************//**
Returns the NUL terminated value of glob_hostname.
@return	pointer to glob_hostname. */
UNIV_INTERN
const char*
server_get_hostname()
/*=================*/
{
	return(glob_hostname);
}

/******************************************************************//**
Returns true if the transaction this thread is processing has edited
non-transactional tables. Used by the deadlock detector when deciding
which transaction to rollback in case of a deadlock - we try to avoid
rolling back transactions that have edited non-transactional tables.
@return	true if non-transactional tables have been edited */
UNIV_INTERN
ibool
thd_has_edited_nontrans_tables(
/*===========================*/
	THD*	thd)	/*!< in: thread handle */
{
	return((ibool) thd_non_transactional_update(thd));
}

/******************************************************************//**
Returns true if the thread is executing a SELECT statement.
@return	true if thd is executing SELECT */
UNIV_INTERN
ibool
thd_is_select(
/*==========*/
	const THD*	thd)	/*!< in: thread handle */
{
	return(thd_sql_command(thd) == SQLCOM_SELECT);
}

/******************************************************************//**
Returns true if the thread supports XA,
global value of innodb_supports_xa if thd is NULL.
@return	true if thd has XA support */
UNIV_INTERN
ibool
thd_supports_xa(
/*============*/
	THD*	thd)	/*!< in: thread handle, or NULL to query
			the global innodb_supports_xa */
{
	return(THDVAR(thd, support_xa));
}

/** Get the value of innodb_tmpdir.
@param[in]	thd	thread handle, or NULL to query
			the global innodb_tmpdir.
@retval NULL if innodb_tmpdir="" */
UNIV_INTERN
const char*
thd_innodb_tmpdir(
	THD*	thd)
{
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!sync_thread_levels_nonempty_trx(false));
#endif /* UNIV_SYNC_DEBUG */

	const char*	tmp_dir = THDVAR(thd, tmpdir);
	if (tmp_dir != NULL && *tmp_dir == '\0') {
		tmp_dir = NULL;
	}

	return(tmp_dir);
}
/******************************************************************//**
Returns the lock wait timeout for the current connection.
@return	the lock wait timeout, in seconds */
UNIV_INTERN
ulong
thd_lock_wait_timeout(
/*==================*/
	THD*	thd)	/*!< in: thread handle, or NULL to query
			the global innodb_lock_wait_timeout */
{
	/* According to <mysql/plugin.h>, passing thd == NULL
	returns the global value of the session variable. */
	return(THDVAR(thd, lock_wait_timeout));
}

/******************************************************************//**
Set the time waited for the lock for the current query. */
UNIV_INTERN
void
thd_set_lock_wait_time(
/*===================*/
	THD*	thd,	/*!< in/out: thread handle */
	ulint	value)	/*!< in: time waited for the lock */
{
	if (thd) {
		thd_storage_lock_wait(thd, value);
	}
}

/******************************************************************//**
*/
UNIV_INTERN
ulong
thd_flush_log_at_trx_commit(
/*================================*/
	THD*	thd)
{
	return(THDVAR(thd, flush_log_at_trx_commit));
}

/******************************************************************//**
Returns true if expand_fast_index_creation is enabled for the current
session.
@return	the value of the server's expand_fast_index_creation variable */
UNIV_INTERN
ibool
thd_expand_fast_index_creation(
/*================================*/
	void*	thd)
{
	return((ibool) (((THD*) thd)->variables.expand_fast_index_creation));
}

/********************************************************************//**
Obtain the InnoDB transaction of a MySQL thread.
@return	reference to transaction pointer */
MY_ATTRIBUTE((warn_unused_result, nonnull))
static inline
trx_t*&
thd_to_trx(
/*=======*/
	THD*	thd)	/*!< in: MySQL thread */
{
	return(*(trx_t**) thd_ha_data(thd, innodb_hton_ptr));
}
#ifdef WITH_WSREP
ulonglong
thd_to_trx_id(
/*=======*/
	THD*	thd)	/*!< in: MySQL thread */
{
	return(thd_to_trx(thd)->id);
}
#endif

my_bool
ha_innobase::is_fake_change_enabled(THD* thd)
{
	trx_t*	trx	= thd_to_trx(thd);
	return(trx && UNIV_UNLIKELY(trx->fake_changes));
}


/********************************************************************//**
In XtraDB it is impossible for a transaction to own a search latch outside of
InnoDB code, so there is nothing to release on demand.  We keep this function to
simplify maintenance.
@return 0 */
static
int
innobase_release_temporary_latches(
/*===============================*/
	handlerton*	hton MY_ATTRIBUTE((unused)),	/*!< in: handlerton */
	THD*		thd MY_ATTRIBUTE((unused)))	/*!< in: MySQL thread */
{
#ifdef UNIV_DEBUG
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (!innodb_inited || thd == NULL) {

		return(0);
	}

	trx_t*	trx = thd_to_trx(thd);

	if (trx != NULL) {
#ifdef UNIV_SYNC_DEBUG
		ut_ad(!btr_search_own_any());
#endif
		trx_search_latch_release_if_reserved(trx);
	}
#endif

	return(0);
}

#ifdef WITH_WSREP
static int
wsrep_abort_transaction(handlerton* hton, THD *bf_thd, THD *victim_thd,
			my_bool signal);
static void
wsrep_fake_trx_id(handlerton* hton, THD *thd);
static int innobase_wsrep_set_checkpoint(handlerton* hton, const XID* xid);
static int innobase_wsrep_get_checkpoint(handlerton* hton, XID* xid);
#endif
/********************************************************************//**
Increments innobase_active_counter and every INNOBASE_WAKE_INTERVALth
time calls srv_active_wake_master_thread. This function should be used
when a single database operation may introduce a small need for
server utility activity, like checkpointing. */
static inline
void
innobase_active_small(void)
/*=======================*/
{
	innobase_active_counter++;

	if ((innobase_active_counter % INNOBASE_WAKE_INTERVAL) == 0) {
		srv_active_wake_master_thread();
	}
}

/********************************************************************//**
Converts an InnoDB error code to a MySQL error code and also tells to MySQL
about a possible transaction rollback inside InnoDB caused by a lock wait
timeout or a deadlock.
@return	MySQL error code */
static
int
convert_error_code_to_mysql(
/*========================*/
	dberr_t	error,	/*!< in: InnoDB error code */
	ulint	flags,  /*!< in: InnoDB table flags, or 0 */
	THD*	thd)	/*!< in: user thread handle or NULL */
{
	switch (error) {
	case DB_SUCCESS:
		return(0);

	case DB_INTERRUPTED:
		thd_set_kill_status(thd ? thd : thd_get_current_thd());
		return(-1);

	case DB_FOREIGN_EXCEED_MAX_CASCADE:
		ut_ad(thd);
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_ROW_IS_REFERENCED,
				    "InnoDB: Cannot delete/update "
				    "rows with cascading foreign key "
				    "constraints that exceed max "
				    "depth of %d. Please "
				    "drop extra constraints and try "
				    "again", DICT_FK_MAX_RECURSIVE_LOAD);

		/* fall through */

	case DB_ERROR:
	default:
		return(-1); /* unspecified error */

	case DB_DUPLICATE_KEY:
		/* Be cautious with returning this error, since
		mysql could re-enter the storage layer to get
		duplicated key info, the operation requires a
		valid table handle and/or transaction information,
		which might not always be available in the error
		handling stage. */
		return(HA_ERR_FOUND_DUPP_KEY);

	case DB_READ_ONLY:
		if(srv_force_recovery) {
			return(HA_ERR_INNODB_FORCED_RECOVERY);
		}
		return(HA_ERR_TABLE_READONLY);

	case DB_FOREIGN_DUPLICATE_KEY:
		return(HA_ERR_FOREIGN_DUPLICATE_KEY);

	case DB_MISSING_HISTORY:
		return(HA_ERR_TABLE_DEF_CHANGED);

	case DB_RECORD_NOT_FOUND:
		return(HA_ERR_NO_ACTIVE_RECORD);

	case DB_DEADLOCK:
		/* Since we rolled back the whole transaction, we must
		tell it also to MySQL so that MySQL knows to empty the
		cached binlog for this transaction */

		if (thd) {
			thd_mark_transaction_to_rollback(thd, TRUE);
		}

		return(HA_ERR_LOCK_DEADLOCK);

	case DB_LOCK_WAIT_TIMEOUT:
		/* Starting from 5.0.13, we let MySQL just roll back the
		latest SQL statement in a lock wait timeout. Previously, we
		rolled back the whole transaction. */

		if (thd) {
			thd_mark_transaction_to_rollback(
				thd, (bool) row_rollback_on_timeout);
		}

		return(HA_ERR_LOCK_WAIT_TIMEOUT);

	case DB_NO_REFERENCED_ROW:
		return(HA_ERR_NO_REFERENCED_ROW);

	case DB_ROW_IS_REFERENCED:
		return(HA_ERR_ROW_IS_REFERENCED);

	case DB_CANNOT_ADD_CONSTRAINT:
	case DB_CHILD_NO_INDEX:
	case DB_PARENT_NO_INDEX:
		return(HA_ERR_CANNOT_ADD_FOREIGN);

	case DB_CANNOT_DROP_CONSTRAINT:

		return(HA_ERR_ROW_IS_REFERENCED); /* TODO: This is a bit
						misleading, a new MySQL error
						code should be introduced */

	case DB_CORRUPTION:
		return(HA_ERR_CRASHED);

	case DB_OUT_OF_FILE_SPACE:
		return(HA_ERR_RECORD_FILE_FULL);

	case DB_TEMP_FILE_WRITE_FAILURE:
		return(HA_ERR_TEMP_FILE_WRITE_FAILURE);

	case DB_TABLE_IN_FK_CHECK:
		return(HA_ERR_TABLE_IN_FK_CHECK);

	case DB_TABLE_IS_BEING_USED:
		return(HA_ERR_WRONG_COMMAND);

	case DB_TABLESPACE_DELETED:
	case DB_TABLE_NOT_FOUND:
		return(HA_ERR_NO_SUCH_TABLE);

	case DB_TABLESPACE_NOT_FOUND:
		return(HA_ERR_NO_SUCH_TABLE);

	case DB_TOO_BIG_RECORD: {
		/* If prefix is true then a 768-byte prefix is stored
		locally for BLOB fields. Refer to dict_table_get_format() */
		bool prefix = (dict_tf_get_format(flags) == UNIV_FORMAT_A);
		my_printf_error(ER_TOO_BIG_ROWSIZE,
			"Row size too large (> %lu). Changing some columns "
			"to TEXT or BLOB %smay help. In current row "
			"format, BLOB prefix of %d bytes is stored inline.",
			MYF(0),
			page_get_free_space_of_empty(flags &
				DICT_TF_COMPACT) / 2,
			prefix ? "or using ROW_FORMAT=DYNAMIC "
			"or ROW_FORMAT=COMPRESSED ": "",
			prefix ? DICT_MAX_FIXED_COL_LEN : 0);
		return(HA_ERR_TO_BIG_ROW);
	}


	case DB_TOO_BIG_FOR_REDO:
		my_printf_error(ER_TOO_BIG_ROWSIZE, "%s" , MYF(0),
				"The size of BLOB/TEXT data inserted"
				" in one transaction is greater than"
				" 10% of redo log size. Increase the"
				" redo log size using innodb_log_file_size.");
		return(HA_ERR_TO_BIG_ROW);

	case DB_TOO_BIG_INDEX_COL:
		my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
			 DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags));
		return(HA_ERR_INDEX_COL_TOO_LONG);

	case DB_NO_SAVEPOINT:
		return(HA_ERR_NO_SAVEPOINT);

	case DB_LOCK_TABLE_FULL:
		/* Since we rolled back the whole transaction, we must
		tell it also to MySQL so that MySQL knows to empty the
		cached binlog for this transaction */

		if (thd) {
			thd_mark_transaction_to_rollback(thd, TRUE);
		}

		return(HA_ERR_LOCK_TABLE_FULL);

	case DB_FTS_INVALID_DOCID:
		return(HA_FTS_INVALID_DOCID);
	case DB_FTS_EXCEED_RESULT_CACHE_LIMIT:
		return(HA_ERR_FTS_EXCEED_RESULT_CACHE_LIMIT);
	case DB_TOO_MANY_CONCURRENT_TRXS:
		return(HA_ERR_TOO_MANY_CONCURRENT_TRXS);
	case DB_UNSUPPORTED:
		return(HA_ERR_UNSUPPORTED);
	case DB_INDEX_CORRUPT:
		return(HA_ERR_INDEX_CORRUPT);
	case DB_UNDO_RECORD_TOO_BIG:
		return(HA_ERR_UNDO_REC_TOO_BIG);
	case DB_OUT_OF_MEMORY:
		return(HA_ERR_OUT_OF_MEM);
	case DB_TABLESPACE_EXISTS:
		return(HA_ERR_TABLESPACE_EXISTS);
	case DB_IDENTIFIER_TOO_LONG:
		return(HA_ERR_INTERNAL_ERROR);
	case DB_FTS_TOO_MANY_WORDS_IN_PHRASE:
		return(HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE);
	}
}

/*************************************************************//**
Prints info of a THD object (== user session thread) to the given file. */
UNIV_INTERN
void
innobase_mysql_print_thd(
/*=====================*/
	FILE*	f,		/*!< in: output stream */
	THD*	thd,		/*!< in: MySQL THD object */
	uint	max_query_len)	/*!< in: max query length to print, or 0 to
				use the default max length */
{
	char	buffer[1024];

	fputs(thd_security_context(thd, buffer, sizeof buffer,
				   max_query_len), f);
	putc('\n', f);
}

/******************************************************************//**
Get the error message format string.
@return the format string or 0 if not found. */
UNIV_INTERN
const char*
innobase_get_err_msg(
/*=================*/
	int	error_code)	/*!< in: MySQL error code */
{
	return(my_get_err_msg(error_code));
}

/******************************************************************//**
Get the variable length bounds of the given character set. */
UNIV_INTERN
void
innobase_get_cset_width(
/*====================*/
	ulint	cset,		/*!< in: MySQL charset-collation code */
	ulint*	mbminlen,	/*!< out: minimum length of a char (in bytes) */
	ulint*	mbmaxlen)	/*!< out: maximum length of a char (in bytes) */
{
	CHARSET_INFO*	cs;
	ut_ad(cset <= MAX_CHAR_COLL_NUM);
	ut_ad(mbminlen);
	ut_ad(mbmaxlen);

	cs = all_charsets[cset];
	if (cs) {
		*mbminlen = cs->mbminlen;
		*mbmaxlen = cs->mbmaxlen;
		ut_ad(*mbminlen < DATA_MBMAX);
		ut_ad(*mbmaxlen < DATA_MBMAX);
	} else {
		THD*	thd = current_thd;

		if (thd && thd_sql_command(thd) == SQLCOM_DROP_TABLE) {

			/* Fix bug#46256: allow tables to be dropped if the
			collation is not found, but issue a warning. */
			if ((log_warnings)
			    && (cset != 0)){

				sql_print_warning(
					"Unknown collation #%lu.", cset);
			}
		} else {

			ut_a(cset == 0);
		}

		*mbminlen = *mbmaxlen = 0;
	}
}

/******************************************************************//**
Converts an identifier to a table name. */
UNIV_INTERN
void
innobase_convert_from_table_id(
/*===========================*/
	struct charset_info_st*	cs,	/*!< in: the 'from' character set */
	char*			to,	/*!< out: converted identifier */
	const char*		from,	/*!< in: identifier to convert */
	ulint			len)	/*!< in: length of 'to', in bytes */
{
	uint	errors;

	strconvert(cs, from, &my_charset_filename, to, (uint) len, &errors);
}

/**********************************************************************
Check if the length of the identifier exceeds the maximum allowed.
return true when length of identifier is too long. */
UNIV_INTERN
my_bool
innobase_check_identifier_length(
/*=============================*/
	const char*	id)	/* in: FK identifier to check excluding the
				database portion. */
{
	int		well_formed_error = 0;
	CHARSET_INFO	*cs = system_charset_info;
	DBUG_ENTER("innobase_check_identifier_length");

	size_t len = cs->cset->well_formed_len(
		cs, id, id + strlen(id),
		NAME_CHAR_LEN, &well_formed_error);

	if (well_formed_error || len == NAME_CHAR_LEN) {
		my_error(ER_TOO_LONG_IDENT, MYF(0), id);
		DBUG_RETURN(true);
	}
	DBUG_RETURN(false);
}

/******************************************************************//**
Converts an identifier to UTF-8. */
UNIV_INTERN
void
innobase_convert_from_id(
/*=====================*/
	struct charset_info_st*	cs,	/*!< in: the 'from' character set */
	char*			to,	/*!< out: converted identifier */
	const char*		from,	/*!< in: identifier to convert */
	ulint			len)	/*!< in: length of 'to', in bytes */
{
	uint	errors;

	strconvert(cs, from, system_charset_info, to, (uint) len, &errors);
}

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively.
@return	0 if a=b, <0 if a<b, >1 if a>b */
UNIV_INTERN
int
innobase_strcasecmp(
/*================*/
	const char*	a,	/*!< in: first string to compare */
	const char*	b)	/*!< in: second string to compare */
{
	if (!a) {
		if (!b) {
			return(0);
		} else {
			return(-1);
		}
	} else if (!b) {
		return(1);
	}

	return(my_strcasecmp(system_charset_info, a, b));
}

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively. The
second string contains wildcards.
@return 0 if a match is found, 1 if not */
UNIV_INTERN
int
innobase_wildcasecmp(
/*=================*/
	const char*	a,	/*!< in: string to compare */
	const char*	b)	/*!< in: wildcard string to compare */
{
	return(wild_case_compare(system_charset_info, a, b));
}

/******************************************************************//**
Strip dir name from a full path name and return only the file name
@return file name or "null" if no file name */
UNIV_INTERN
const char*
innobase_basename(
/*==============*/
	const char*	path_name)	/*!< in: full path name */
{
	const char*	name = base_name(path_name);

	return((name) ? name : "null");
}

/******************************************************************//**
Makes all characters in a NUL-terminated UTF-8 string lower case. */
UNIV_INTERN
void
innobase_casedn_str(
/*================*/
	char*	a)	/*!< in/out: string to put in lower case */
{
	my_casedn_str(system_charset_info, a);
}

/**********************************************************************//**
Determines the connection character set.
@return	connection character set */
UNIV_INTERN
struct charset_info_st*
innobase_get_charset(
/*=================*/
	THD*	mysql_thd)	/*!< in: MySQL thread handle */
{
	return(thd_charset(mysql_thd));
}

/**********************************************************************//**
Determines the current SQL statement.
@return	SQL statement string */
UNIV_INTERN
const char*
innobase_get_stmt(
/*==============*/
	THD*	thd,		/*!< in: MySQL thread handle */
	size_t*	length)		/*!< out: length of the SQL statement */
{
	LEX_STRING* stmt;

	stmt = thd_query_string(thd);
	*length = stmt->length;
	return(stmt->str);
}

/**********************************************************************//**
Get the current setting of the table_def_size global parameter. We do
a dirty read because for one there is no synchronization object and
secondly there is little harm in doing so even if we get a torn read.
@return	value of table_def_size */
UNIV_INTERN
ulint
innobase_get_table_cache_size(void)
/*===============================*/
{
	return(table_def_size);
}

/**********************************************************************//**
Get the current setting of the lower_case_table_names global parameter from
mysqld.cc. We do a dirty read because for one there is no synchronization
object and secondly there is little harm in doing so even if we get a torn
read.
@return	value of lower_case_table_names */
UNIV_INTERN
ulint
innobase_get_lower_case_table_names(void)
/*=====================================*/
{
	return(lower_case_table_names);
}
/** return one of the tmpdir path
@return tmpdir path*/
UNIV_INTERN
char*
innobase_mysql_tmpdir(void) { return (mysql_tmpdir); }

/** Create a temporary file in the location specified by the parameter
path. If the path is null, then it will be created in tmpdir.
@param[in]	path	location for creating temporary file
@return	temporary file descriptor, or < 0 on error */
UNIV_INTERN
int
innobase_mysql_tmpfile(
	const char*	path)
{
#ifdef WITH_INNODB_DISALLOW_WRITES
	os_event_wait(srv_allow_writes_event);
#endif /* WITH_INNODB_DISALLOW_WRITES */
	int	fd2 = -1;
	File	fd;

	DBUG_EXECUTE_IF(
		"innobase_tmpfile_creation_failure",
		return(-1);
	);

	if (path == NULL) {
		fd = mysql_tmpfile("ib");
	} else {
		fd = mysql_tmpfile_path(path, "ib");
	}

	if (fd >= 0) {
		/* Copy the file descriptor, so that the additional resources
		allocated by create_temp_file() can be freed by invoking
		my_close().

		Because the file descriptor returned by this function
		will be passed to fdopen(), it will be closed by invoking
		fclose(), which in turn will invoke close() instead of
		my_close(). */

#ifdef _WIN32
		/* Note that on Windows, the integer returned by mysql_tmpfile
		has no relation to C runtime file descriptor. Here, we need
		to call my_get_osfhandle to get the HANDLE and then convert it
		to C runtime filedescriptor. */
		{
			HANDLE hFile = my_get_osfhandle(fd);
			HANDLE hDup;
			BOOL bOK = DuplicateHandle(
					GetCurrentProcess(),
					hFile, GetCurrentProcess(),
					&hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
			if (bOK) {
				fd2 = _open_osfhandle((intptr_t) hDup, 0);
			} else {
				my_osmaperr(GetLastError());
				fd2 = -1;
			}
		}
#else
		fd2 = dup(fd);
#endif
		if (fd2 < 0) {
			char errbuf[MYSYS_STRERROR_SIZE];
			DBUG_PRINT("error",("Got error %d on dup",fd2));
			my_errno=errno;
			my_error(EE_OUT_OF_FILERESOURCES,
				 MYF(ME_BELL+ME_WAITTANG),
				 "ib*", my_errno,
				 my_strerror(errbuf, sizeof(errbuf), my_errno));
		}
		my_close(fd, MYF(MY_WME));
	}
	return(fd2);
}

/*********************************************************************//**
Wrapper around MySQL's copy_and_convert function.
@return	number of bytes copied to 'to' */
UNIV_INTERN
ulint
innobase_convert_string(
/*====================*/
	void*		to,		/*!< out: converted string */
	ulint		to_length,	/*!< in: number of bytes reserved
					for the converted string */
	CHARSET_INFO*	to_cs,		/*!< in: character set to convert to */
	const void*	from,		/*!< in: string to convert */
	ulint		from_length,	/*!< in: number of bytes to convert */
	CHARSET_INFO*	from_cs,	/*!< in: character set to convert
					from */
	uint*		errors)		/*!< out: number of errors encountered
					during the conversion */
{
	return(copy_and_convert(
			(char*) to, (uint32) to_length, to_cs,
			(const char*) from, (uint32) from_length, from_cs,
			errors));
}

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "charset_coll" and writes
the result to "buf". The result is converted to "system_charset_info".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return	number of bytes that were written */
UNIV_INTERN
ulint
innobase_raw_format(
/*================*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		charset_coll,	/*!< in: charset collation */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size)	/*!< in: output buffer size
					in bytes */
{
	/* XXX we use a hard limit instead of allocating
	but_size bytes from the heap */
	CHARSET_INFO*	data_cs;
	char		buf_tmp[8192];
	ulint		buf_tmp_used;
	uint		num_errors;

	data_cs = all_charsets[charset_coll];

	buf_tmp_used = innobase_convert_string(buf_tmp, sizeof(buf_tmp),
					       system_charset_info,
					       data, data_len, data_cs,
					       &num_errors);

	return(ut_str_sql_format(buf_tmp, buf_tmp_used, buf, buf_size));
}

/*********************************************************************//**
Compute the next autoinc value.

For MySQL replication the autoincrement values can be partitioned among
the nodes. The offset is the start or origin of the autoincrement value
for a particular node. For n nodes the increment will be n and the offset
will be in the interval [1, n]. The formula tries to allocate the next
value for a particular node.

Note: This function is also called with increment set to the number of
values we want to reserve for multi-value inserts e.g.,

	INSERT INTO T VALUES(), (), ();

innobase_next_autoinc() will be called with increment set to 3 where
autoinc_lock_mode != TRADITIONAL because we want to reserve 3 values for
the multi-value INSERT above.
@return	the next value */
UNIV_INTERN
ulonglong
innobase_next_autoinc(
/*==================*/
	ulonglong	current,	/*!< in: Current value */
	ulonglong	need,		/*!< in: count of values needed */
	ulonglong	step,		/*!< in: AUTOINC increment step */
	ulonglong	offset,		/*!< in: AUTOINC offset */
	ulonglong	max_value)	/*!< in: max value for type */
{
	ulonglong	next_value;
	ulonglong	block = need * step;

	/* Should never be 0. */
	ut_a(need > 0);
	ut_a(block > 0);
	ut_a(max_value > 0);

	/* According to MySQL documentation, if the offset is greater than
	the step then the offset is ignored. */
	if (offset > block) {
		offset = 0;
	}

	/* Check for overflow. Current can be > max_value if the value is
	in reality a negative value.The visual studio compilers converts
	large double values automatically into unsigned long long datatype
	maximum value */

	if (block >= max_value
	    || offset > max_value
	    || current >= max_value
	    || max_value - offset <= offset) {

		next_value = max_value;
	} else {
		ut_a(max_value > current);

		ulonglong	free = max_value - current;

		if (free < offset || free - offset <= block) {
			next_value = max_value;
		} else {
			next_value = 0;
		}
	}

	if (next_value == 0) {
		ulonglong	next;

		if (current > offset) {
			next = (current - offset) / step;
		} else {
			next = (offset - current) / step;
		}

		ut_a(max_value > next);
		next_value = next * step;
		/* Check for multiplication overflow. */
		ut_a(next_value >= next);
		ut_a(max_value > next_value);

		/* Check for overflow */
		if (max_value - next_value >= block) {

			next_value += block;

			if (max_value - next_value >= offset) {
				next_value += offset;
			} else {
				next_value = max_value;
			}
		} else {
			next_value = max_value;
		}
	}

	ut_a(next_value != 0);
	ut_a(next_value <= max_value);

	return(next_value);
}

/*********************************************************************//**
Initializes some fields in an InnoDB transaction object. */
static
void
innobase_trx_init(
/*==============*/
	THD*	thd,	/*!< in: user thread handle */
	trx_t*	trx)	/*!< in/out: InnoDB transaction handle */
{
	DBUG_ENTER("innobase_trx_init");
	DBUG_ASSERT(EQ_CURRENT_THD(thd));
	DBUG_ASSERT(thd == trx->mysql_thd);

	trx->check_foreigns = !thd_test_options(
		thd, OPTION_NO_FOREIGN_KEY_CHECKS);

	trx->check_unique_secondary = !thd_test_options(
		thd, OPTION_RELAXED_UNIQUE_CHECKS);

#ifdef EXTENDED_SLOWLOG
	if (thd_log_slow_verbosity(thd) & (1ULL << SLOG_V_INNODB)) {
		trx->take_stats = TRUE;
	} else {
		trx->take_stats = FALSE;
	}
#else
	trx->take_stats = FALSE;
#endif

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Allocates an InnoDB transaction for a MySQL handler object for DML.
@return	InnoDB transaction handle */
UNIV_INTERN
trx_t*
innobase_trx_allocate(
/*==================*/
	THD*	thd)	/*!< in: user thread handle */
{
	trx_t*	trx;

	DBUG_ENTER("innobase_trx_allocate");
	DBUG_ASSERT(thd != NULL);
	DBUG_ASSERT(EQ_CURRENT_THD(thd));

	trx = trx_allocate_for_mysql();

	trx->mysql_thd = thd;

	innobase_trx_init(thd, trx);

	DBUG_RETURN(trx);
}

/*********************************************************************//**
Gets the InnoDB transaction handle for a MySQL handler object, creates
an InnoDB transaction struct if the corresponding MySQL thread struct still
lacks one.
@return	InnoDB transaction handle */
static inline
trx_t*
check_trx_exists(
/*=============*/
	THD*	thd)	/*!< in: user thread handle */
{
	trx_t*&	trx = thd_to_trx(thd);

	ut_ad(EQ_CURRENT_THD(thd));

	if (trx == NULL) {
		trx = innobase_trx_allocate(thd);
	} else if (UNIV_UNLIKELY(trx->magic_n != TRX_MAGIC_N)) {
		mem_analyze_corruption(trx);
		ut_error;
	}

	innobase_trx_init(thd, trx);

	return(trx);
}

/*************************************************************************
Gets current trx. */
trx_t*
innobase_get_trx()
{
	THD *thd=current_thd;
	if (likely(thd != 0)) {
		trx_t*& trx = thd_to_trx(thd);
		return(trx);
	} else {
		return(NULL);
	}
}

ibool
innobase_get_slow_log()
{
#ifdef EXTENDED_SLOWLOG
	return((ibool) thd_opt_slow_log());
#else
	return(FALSE);
#endif
}

/*********************************************************************//**
Note that a transaction has been registered with MySQL.
@return true if transaction is registered with MySQL 2PC coordinator */
static inline
bool
trx_is_registered_for_2pc(
/*=========================*/
	const trx_t*	trx)	/* in: transaction */
{
	return(trx->is_registered == 1);
}

/*********************************************************************//**
Note that a transaction has been registered with MySQL 2PC coordinator. */
static inline
void
trx_register_for_2pc(
/*==================*/
	trx_t*	trx)	/* in: transaction */
{
	trx->is_registered = 1;
	ut_ad(trx->owns_prepare_mutex == 0);
}

/*********************************************************************//**
Note that a transaction has been deregistered. */
static inline
void
trx_deregister_from_2pc(
/*====================*/
	trx_t*	trx)	/* in: transaction */
{
	trx->is_registered = 0;
	trx->owns_prepare_mutex = 0;
}

/*********************************************************************//**
Check if transaction is started.
@reutrn true if transaction is in state started */
static
bool
trx_is_started(
/*===========*/
	trx_t*	trx)	/* in: transaction */
{
	return(trx->state != TRX_STATE_NOT_STARTED);
}

/****************************************************************//**
Update log_checksum_algorithm_ptr with a pointer to the function corresponding
to a given checksum algorithm. */
static
void
innodb_log_checksum_func_update(
/*============================*/
	ulint	algorithm)	/*!< in: algorithm */
{
	switch (algorithm) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
	case SRV_CHECKSUM_ALGORITHM_INNODB:
		log_checksum_algorithm_ptr=log_block_calc_checksum_innodb;
		break;
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
	case SRV_CHECKSUM_ALGORITHM_CRC32:
		log_checksum_algorithm_ptr=log_block_calc_checksum_crc32;
		break;
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
	case SRV_CHECKSUM_ALGORITHM_NONE:
		log_checksum_algorithm_ptr=log_block_calc_checksum_none;
		break;
	default:
		ut_a(0);
	}
}

/****************************************************************//**
On update hook for the innodb_log_checksum_algorithm variable. */
static
void
innodb_log_checksum_algorithm_update(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	srv_checksum_algorithm_t	algorithm;

	algorithm = (srv_checksum_algorithm_t)
		(*static_cast<const ulong*>(save));

	/* Make sure we are the only log user */
	mutex_enter(&log_sys->mutex);

	innodb_log_checksum_func_update(algorithm);

	srv_log_checksum_algorithm = algorithm;

	mutex_exit(&log_sys->mutex);
}

/*********************************************************************//**
Copy table flags from MySQL's HA_CREATE_INFO into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
UNIV_INTERN
void
innobase_copy_frm_flags_from_create_info(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const HA_CREATE_INFO*	create_info)	/*!< in: create info */
{
	ibool	ps_on;
	ibool	ps_off;

	if (dict_table_is_temporary(innodb_table)) {
		/* Temp tables do not use persistent stats. */
		ps_on = FALSE;
		ps_off = TRUE;
	} else {
		ps_on = create_info->table_options
			& HA_OPTION_STATS_PERSISTENT;
		ps_off = create_info->table_options
			& HA_OPTION_NO_STATS_PERSISTENT;
	}

	dict_stats_set_persistent(innodb_table, ps_on, ps_off);

	dict_stats_auto_recalc_set(
		innodb_table,
		create_info->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON,
		create_info->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF);

	innodb_table->stats_sample_pages = create_info->stats_sample_pages;
}

/*********************************************************************//**
Copy table flags from MySQL's TABLE_SHARE into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
UNIV_INTERN
void
innobase_copy_frm_flags_from_table_share(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const TABLE_SHARE*	table_share)	/*!< in: table share */
{
	ibool	ps_on;
	ibool	ps_off;

	if (dict_table_is_temporary(innodb_table)) {
		/* Temp tables do not use persistent stats */
		ps_on = FALSE;
		ps_off = TRUE;
	} else {
		ps_on = table_share->db_create_options
			& HA_OPTION_STATS_PERSISTENT;
		ps_off = table_share->db_create_options
			& HA_OPTION_NO_STATS_PERSISTENT;
	}

	dict_stats_set_persistent(innodb_table, ps_on, ps_off);

	dict_stats_auto_recalc_set(
		innodb_table,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF);

	innodb_table->stats_sample_pages = table_share->stats_sample_pages;
}

/*********************************************************************//**
Construct ha_innobase handler. */
UNIV_INTERN
ha_innobase::ha_innobase(
/*=====================*/
	handlerton*	hton,
	TABLE_SHARE*	table_arg)
	:handler(hton, table_arg),
	int_table_flags(HA_REC_NOT_IN_SEQ |
		  HA_NULL_IN_KEY |
		  HA_CAN_INDEX_BLOBS |
		  HA_CAN_SQL_HANDLER |
		  HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
		  HA_PRIMARY_KEY_IN_READ_INDEX |
		  HA_BINLOG_ROW_CAPABLE |
		  HA_CAN_GEOMETRY | HA_PARTIAL_COLUMN_READ |
		  HA_TABLE_SCAN_ON_INDEX | HA_CAN_FULLTEXT |
		  HA_CAN_FULLTEXT_EXT | HA_CAN_EXPORT | HA_ONLINE_ANALYZE),
	start_of_scan(0),
	num_write_row(0)
{}

/*********************************************************************//**
Destruct ha_innobase handler. */
UNIV_INTERN
ha_innobase::~ha_innobase()
/*======================*/
{
}

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
prebuilt struct. */
UNIV_INTERN inline
void
ha_innobase::update_thd(
/*====================*/
	THD*	thd)	/*!< in: thd to use the handle */
{
	trx_t*		trx;

	DBUG_ENTER("ha_innobase::update_thd");
	DBUG_PRINT("ha_innobase::update_thd", ("user_thd: %p -> %p",
		   user_thd, thd));

	/* The table should have been opened in ha_innobase::open(). */
	DBUG_ASSERT(prebuilt->table->n_ref_count > 0);

	trx = check_trx_exists(thd);

	if (prebuilt->trx != trx) {

		row_update_prebuilt_trx(prebuilt, trx);
	}

	user_thd = thd;
	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
prebuilt struct. */
UNIV_INTERN
void
ha_innobase::update_thd()
/*=====================*/
{
	THD*	thd = ha_thd();

	ut_ad(EQ_CURRENT_THD(thd));
	update_thd(thd);
}

/*********************************************************************//**
Registers an InnoDB transaction with the MySQL 2PC coordinator, so that
the MySQL XA code knows to call the InnoDB prepare and commit, or rollback
for the transaction. This MUST be called for every transaction for which
the user may call commit or rollback. Calling this several times to register
the same transaction is allowed, too. This function also registers the
current SQL statement. */
static inline
void
innobase_register_trx(
/*==================*/
	handlerton*	hton,	/* in: Innobase handlerton */
	THD*		thd,	/* in: MySQL thd (connection) object */
	trx_t*		trx)	/* in: transaction to register */
{
	trans_register_ha(thd, FALSE, hton);

	if (!trx_is_registered_for_2pc(trx)
	    && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

		trans_register_ha(thd, TRUE, hton);
	}

	trx_register_for_2pc(trx);
}

/*	BACKGROUND INFO: HOW THE MYSQL QUERY CACHE WORKS WITH INNODB
	------------------------------------------------------------

1) The use of the query cache for TBL is disabled when there is an
uncommitted change to TBL.

2) When a change to TBL commits, InnoDB stores the current value of
its global trx id counter, let us denote it by INV_TRX_ID, to the table object
in the InnoDB data dictionary, and does only allow such transactions whose
id <= INV_TRX_ID to use the query cache.

3) When InnoDB does an INSERT/DELETE/UPDATE to a table TBL, or an implicit
modification because an ON DELETE CASCADE, we invalidate the MySQL query cache
of TBL immediately.

How this is implemented inside InnoDB:

1) Since every modification always sets an IX type table lock on the InnoDB
table, it is easy to check if there can be uncommitted modifications for a
table: just check if there are locks in the lock list of the table.

2) When a transaction inside InnoDB commits, it reads the global trx id
counter and stores the value INV_TRX_ID to the tables on which it had a lock.

3) If there is an implicit table change from ON DELETE CASCADE or SET NULL,
InnoDB calls an invalidate method for the MySQL query cache for that table.

How this is implemented inside sql_cache.cc:

1) The query cache for an InnoDB table TBL is invalidated immediately at an
INSERT/UPDATE/DELETE, just like in the case of MyISAM. No need to delay
invalidation to the transaction commit.

2) To store or retrieve a value from the query cache of an InnoDB table TBL,
any query must first ask InnoDB's permission. We must pass the thd as a
parameter because InnoDB will look at the trx id, if any, associated with
that thd. Also the full_name which is used as key to search for the table
object. The full_name is a string containing the normalized path to the
table in the canonical format.

3) Use of the query cache for InnoDB tables is now allowed also when
AUTOCOMMIT==0 or we are inside BEGIN ... COMMIT. Thus transactions no longer
put restrictions on the use of the query cache.
*/

/******************************************************************//**
The MySQL query cache uses this to check from InnoDB if the query cache at
the moment is allowed to operate on an InnoDB table. The SQL query must
be a non-locking SELECT.

The query cache is allowed to operate on certain query only if this function
returns TRUE for all tables in the query.

If thd is not in the autocommit state, this function also starts a new
transaction for thd if there is no active trx yet, and assigns a consistent
read view to it if there is no read view yet.

Why a deadlock of threads is not possible: the query cache calls this function
at the start of a SELECT processing. Then the calling thread cannot be
holding any InnoDB semaphores. The calling thread is holding the
query cache mutex, and this function will reserve the InnoDB trx_sys->mutex.
Thus, the 'rank' in sync0sync.h of the MySQL query cache mutex is above
the InnoDB trx_sys->mutex.
@return TRUE if permitted, FALSE if not; note that the value FALSE
does not mean we should invalidate the query cache: invalidation is
called explicitly */
static
my_bool
innobase_query_caching_of_table_permitted(
/*======================================*/
	THD*	thd,		/*!< in: thd of the user who is trying to
				store a result to the query cache or
				retrieve it */
	char*	full_name,	/*!< in: normalized path to the table */
	uint	full_name_len,	/*!< in: length of the normalized path
                                to the table */
	ulonglong *unused)	/*!< unused for this engine */
{
	ibool	is_autocommit;
	trx_t*	trx;
	char	norm_name[1000];

	ut_a(full_name_len < 999);

	trx = check_trx_exists(thd);

	if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
		/* In the SERIALIZABLE mode we add LOCK IN SHARE MODE to every
		plain SELECT if AUTOCOMMIT is not on. */

		return((my_bool)FALSE);
	}

	if (UNIV_UNLIKELY(trx->has_search_latch)) {
		sql_print_error("The calling thread is holding the adaptive "
				"search, latch though calling "
				"innobase_query_caching_of_table_permitted.");
		trx_print(stderr, trx, 1024);
	}

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

		is_autocommit = TRUE;
	} else {
		is_autocommit = FALSE;

	}

	if (is_autocommit && trx->n_mysql_tables_in_use == 0) {
		/* We are going to retrieve the query result from the query
		cache. This cannot be a store operation to the query cache
		because then MySQL would have locks on tables already.

		TODO: if the user has used LOCK TABLES to lock the table,
		then we open a transaction in the call of row_.. below.
		That trx can stay open until UNLOCK TABLES. The same problem
		exists even if we do not use the query cache. MySQL should be
		modified so that it ALWAYS calls some cleanup function when
		the processing of a query ends!

		We can imagine we instantaneously serialize this consistent
		read trx to the current trx id counter. If trx2 would have
		changed the tables of a query result stored in the cache, and
		trx2 would have already committed, making the result obsolete,
		then trx2 would have already invalidated the cache. Thus we
		can trust the result in the cache is ok for this query. */

		return((my_bool)TRUE);
	}

	/* Normalize the table name to InnoDB format */
	normalize_table_name(norm_name, full_name);

	innobase_register_trx(innodb_hton_ptr, thd, trx);

	if (row_search_check_if_query_cache_permitted(trx, norm_name)) {

		/* printf("Query cache for %s permitted\n", norm_name); */

		return((my_bool)TRUE);
	}

	/* printf("Query cache for %s NOT permitted\n", norm_name); */

	return((my_bool)FALSE);
}

/*****************************************************************//**
Invalidates the MySQL query cache for the table. */
UNIV_INTERN
void
innobase_invalidate_query_cache(
/*============================*/
	trx_t*		trx,		/*!< in: transaction which
					modifies the table */
	const char*	full_name,	/*!< in: concatenation of
					database name, null char NUL,
					table name, null char NUL;
					NOTE that in Windows this is
					always in LOWER CASE! */
	ulint		full_name_len)	/*!< in: full name length where
					also the null chars count */
{
	/* Note that the sync0sync.h rank of the query cache mutex is just
	above the InnoDB trx_sys_t->lock. The caller of this function must
	not have latches of a lower rank. */

#ifdef HAVE_QUERY_CACHE
	char	qcache_key_name[2 * (NAME_LEN + 1)];
	size_t	tabname_len;
	size_t	dbname_len;

	/* Construct the key("db-name\0table$name\0") for the query cache using
	the path name("db@002dname\0table@0024name\0") of the table in its
        canonical form. */
	dbname_len = filename_to_tablename(full_name, qcache_key_name,
					   sizeof(qcache_key_name));
	tabname_len = filename_to_tablename(full_name + strlen(full_name) + 1,
					    qcache_key_name + dbname_len + 1,
					    sizeof(qcache_key_name)
                                            - dbname_len - 1);

	/* Argument TRUE below means we are using transactions */
	mysql_query_cache_invalidate4(trx->mysql_thd,
				      qcache_key_name,
				      (dbname_len + tabname_len + 2),
				      TRUE);
#endif
}

/*****************************************************************//**
Convert an SQL identifier to the MySQL system_charset_info (UTF-8)
and quote it if needed.
@return	pointer to the end of buf */
static
char*
innobase_convert_identifier(
/*========================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: identifier to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd,	/*!< in: MySQL connection thread, or NULL */
	ibool		file_id)/*!< in: TRUE=id is a table or database name;
				FALSE=id is an UTF-8 string */
{
	const char*	s	= id;
	int		q;

	char nz[MAX_TABLE_NAME_LEN + 1];
	char nz2[MAX_TABLE_NAME_LEN + 1];

	if (file_id) {

		/* Decode the table name.  The MySQL function expects
		a NUL-terminated string.  The input and output strings
		buffers must not be shared. */
		ut_a(idlen <= MAX_TABLE_NAME_LEN);
		memcpy(nz, id, idlen);
		nz[idlen] = 0;

		s = nz2;
		idlen = explain_filename(thd, nz, nz2, sizeof nz2,
					 EXPLAIN_PARTITIONS_AS_COMMENT);
		goto no_quote;
	}

	/* See if the identifier needs to be quoted. */
	if (UNIV_UNLIKELY(!thd)) {
		q = '"';
	} else {
		q = get_quote_char_for_identifier(thd, s, (int) idlen);
	}

	if (q == EOF) {
no_quote:
		if (UNIV_UNLIKELY(idlen > buflen)) {
			idlen = buflen;
		}
		memcpy(buf, s, idlen);
		return(buf + idlen);
	}

	/* Quote the identifier. */
	if (buflen < 2) {
		return(buf);
	}

	*buf++ = q;
	buflen--;

	for (; idlen; idlen--) {
		int	c = *s++;
		if (UNIV_UNLIKELY(c == q)) {
			if (UNIV_UNLIKELY(buflen < 3)) {
				break;
			}

			*buf++ = c;
			*buf++ = c;
			buflen -= 2;
		} else {
			if (UNIV_UNLIKELY(buflen < 2)) {
				break;
			}

			*buf++ = c;
			buflen--;
		}
	}

	*buf++ = q;
	return(buf);
}

/*****************************************************************//**
Convert a table or index name to the MySQL system_charset_info (UTF-8)
and quote it if needed.
@return	pointer to the end of buf */
UNIV_INTERN
char*
innobase_convert_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: identifier to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd,	/*!< in: MySQL connection thread, or NULL */
	ibool		table_id)/*!< in: TRUE=id is a table or database name;
				FALSE=id is an index name */
{
	char*		s	= buf;
	const char*	bufend	= buf + buflen;

	if (table_id) {
		const char*	slash = (const char*) memchr(id, '/', idlen);
		if (!slash) {

			goto no_db_name;
		}

		/* Print the database name and table name separately. */
		s = innobase_convert_identifier(s, bufend - s, id, slash - id,
						thd, TRUE);
		if (UNIV_LIKELY(s < bufend)) {
			*s++ = '.';
			s = innobase_convert_identifier(s, bufend - s,
							slash + 1, idlen
							- (slash - id) - 1,
							thd, TRUE);
		}
	} else if (UNIV_UNLIKELY(*id == TEMP_INDEX_PREFIX)) {
		/* Temporary index name (smart ALTER TABLE) */
		const char temp_index_suffix[]= "--temporary--";

		s = innobase_convert_identifier(buf, buflen, id + 1, idlen - 1,
						thd, FALSE);
		if (s - buf + (sizeof temp_index_suffix - 1) < buflen) {
			memcpy(s, temp_index_suffix,
			       sizeof temp_index_suffix - 1);
			s += sizeof temp_index_suffix - 1;
		}
	} else {
no_db_name:
		s = innobase_convert_identifier(buf, buflen, id, idlen,
						thd, table_id);
	}

	return(s);
}

/*****************************************************************//**
A wrapper function of innobase_convert_name(), convert a table or
index name to the MySQL system_charset_info (UTF-8) and quote it if needed.
@return	pointer to the end of buf */
UNIV_INTERN
void
innobase_format_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	name,	/*!< in: index or table name to format */
	ibool		is_index_name) /*!< in: index name */
{
	const char*     bufend;

	bufend = innobase_convert_name(buf, buflen, name, strlen(name),
				       NULL, !is_index_name);

	ut_ad((ulint) (bufend - buf) < buflen);

	buf[bufend - buf] = '\0';
}

/**********************************************************************//**
Determines if the currently running transaction has been interrupted.
@return	TRUE if interrupted */
UNIV_INTERN
ibool
trx_is_interrupted(
/*===============*/
	const trx_t*	trx)	/*!< in: transaction */
{
	return(trx && trx->mysql_thd && thd_killed(trx->mysql_thd));
}

/**********************************************************************//**
Determines if the currently running transaction is in strict mode.
@return	TRUE if strict */
UNIV_INTERN
ibool
trx_is_strict(
/*==========*/
	trx_t*	trx)	/*!< in: transaction */
{
	return(trx && trx->mysql_thd && THDVAR(trx->mysql_thd, strict_mode));
}

/**************************************************************//**
Resets some fields of a prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */
inline
void
ha_innobase::reset_template(void)
/*=============================*/
{
	ut_ad(prebuilt->magic_n == ROW_PREBUILT_ALLOCATED);
	ut_ad(prebuilt->magic_n2 == prebuilt->magic_n);

	/* Force table to be freed in close_thread_table(). */
	DBUG_EXECUTE_IF("free_table_in_fts_query",
		if (prebuilt->in_fts_query) {
			table->m_needs_reopen = true;
		}
	);

	prebuilt->keep_other_fields_on_keyread = 0;
	prebuilt->read_just_key = 0;
	prebuilt->in_fts_query = 0;
	prebuilt->end_range = false;
	/* Reset index condition pushdown state. */
	if (prebuilt->idx_cond) {
		prebuilt->idx_cond = NULL;
		prebuilt->idx_cond_n_cols = 0;
		/* Invalidate prebuilt->mysql_template
		in ha_innobase::write_row(). */
		prebuilt->template_type = ROW_MYSQL_NO_TEMPLATE;
	}
}

/*****************************************************************//**
Call this when you have opened a new table handle in HANDLER, before you
call index_read_idx() etc. Actually, we can let the cursor stay open even
over a transaction commit! Then you should call this before every operation,
fetch next etc. This function inits the necessary things even after a
transaction commit. */
UNIV_INTERN
void
ha_innobase::init_table_handle_for_HANDLER(void)
/*============================================*/
{
	/* If current thd does not yet have a trx struct, create one.
	If the current handle does not yet have a prebuilt struct, create
	one. Update the trx pointers in the prebuilt struct. Normally
	this operation is done in external_lock. */

	update_thd(ha_thd());

	/* Initialize the prebuilt struct much like it would be inited in
	external_lock */

	trx_search_latch_release_if_reserved(prebuilt->trx);

	innobase_srv_conc_force_exit_innodb(prebuilt->trx);

	/* If the transaction is not started yet, start it */

	trx_start_if_not_started_xa(prebuilt->trx);

	/* Assign a read view if the transaction does not have it yet */

	trx_assign_read_view(prebuilt->trx);

	innobase_register_trx(ht, user_thd, prebuilt->trx);

	/* We did the necessary inits in this function, no need to repeat them
	in row_search_for_mysql */

	prebuilt->sql_stat_start = FALSE;

	/* We let HANDLER always to do the reads as consistent reads, even
	if the trx isolation level would have been specified as SERIALIZABLE */

	prebuilt->select_lock_type = LOCK_NONE;
	prebuilt->stored_select_lock_type = LOCK_NONE;

	/* Always fetch all columns in the index record */

	prebuilt->hint_need_to_fetch_extra_cols = ROW_RETRIEVE_ALL_COLS;

	/* We want always to fetch all columns in the whole row? Or do
	we???? */

	prebuilt->used_in_HANDLER = TRUE;
	reset_template();
}

/*********************************************************************//**
Opens an InnoDB database.
@return	0 on success, error code on failure */
static
int
innobase_init(
/*==========*/
	void	*p)	/*!< in: InnoDB handlerton */
{
	static char	current_dir[3];		/*!< Set if using current lib */
	int		err;
	bool		ret;
	char		*default_path;
	uint		format_id;
	ulong		num_pll_degree;

	DBUG_ENTER("innobase_init");
	handlerton *innobase_hton= (handlerton*) p;
	innodb_hton_ptr = innobase_hton;

	innobase_hton->state = SHOW_OPTION_YES;
	innobase_hton->db_type= DB_TYPE_INNODB;
	innobase_hton->savepoint_offset = sizeof(trx_named_savept_t);
	innobase_hton->close_connection = innobase_close_connection;
	innobase_hton->savepoint_set = innobase_savepoint;
	innobase_hton->savepoint_rollback = innobase_rollback_to_savepoint;
	innobase_hton->savepoint_rollback_can_release_mdl =
				innobase_rollback_to_savepoint_can_release_mdl;
	innobase_hton->savepoint_release = innobase_release_savepoint;
	innobase_hton->commit = innobase_commit;
	innobase_hton->rollback = innobase_rollback;
	innobase_hton->prepare = innobase_xa_prepare;
	innobase_hton->recover = innobase_xa_recover;
	innobase_hton->commit_by_xid = innobase_commit_by_xid;
	innobase_hton->rollback_by_xid = innobase_rollback_by_xid;
	innobase_hton->create_cursor_read_view = innobase_create_cursor_view;
	innobase_hton->set_cursor_read_view = innobase_set_cursor_view;
	innobase_hton->close_cursor_read_view = innobase_close_cursor_view;
	innobase_hton->create = innobase_create_handler;
	innobase_hton->drop_database = innobase_drop_database;
	innobase_hton->panic = innobase_end;

	innobase_hton->start_consistent_snapshot =
		innobase_start_trx_and_assign_read_view;
	innobase_hton->clone_consistent_snapshot =
		innobase_start_trx_and_clone_read_view;

	innobase_hton->store_binlog_info =
		innobase_store_binlog_info;

	innobase_hton->flush_logs = innobase_flush_logs;
	innobase_hton->show_status = innobase_show_status;
	innobase_hton->flags = HTON_SUPPORTS_EXTENDED_KEYS |
		HTON_SUPPORTS_ONLINE_BACKUPS | HTON_SUPPORTS_FOREIGN_KEYS |
		HTON_SUPPORTS_COMPRESSED_COLUMNS;

	innobase_hton->release_temporary_latches =
		innobase_release_temporary_latches;
	innobase_hton->purge_archive_logs = innobase_purge_archive_logs;

	innobase_hton->data = &innodb_api_cb;
	innobase_hton->is_reserved_db_name= innobase_check_reserved_file_name;
	innobase_hton->flush_changed_page_bitmaps
		= innobase_flush_changed_page_bitmaps;
	innobase_hton->purge_changed_page_bitmaps
		= innobase_purge_changed_page_bitmaps;
#ifdef WITH_WSREP
        innobase_hton->wsrep_abort_transaction=wsrep_abort_transaction;
        innobase_hton->wsrep_set_checkpoint=innobase_wsrep_set_checkpoint;
        innobase_hton->wsrep_get_checkpoint=innobase_wsrep_get_checkpoint;
        innobase_hton->wsrep_fake_trx_id=wsrep_fake_trx_id;
#endif /* WITH_WSREP */
	innobase_hton->is_fake_change = innobase_is_fake_change;
	innobase_hton->get_parent_fk_list = innobase_get_parent_fk_list;

	innobase_hton->kill_connection = innobase_kill_connection;

	innobase_hton->create_zip_dict = innobase_create_zip_dict;
	innobase_hton->drop_zip_dict = innobase_drop_zip_dict;
	innobase_hton->is_reserved_db_name= innobase_check_reserved_file_name;

	ut_a(DATA_MYSQL_TRUE_VARCHAR == (ulint)MYSQL_TYPE_VARCHAR);

#ifndef DBUG_OFF
	static const char	test_filename[] = "-@";
	char			test_tablename[sizeof test_filename
				+ sizeof(srv_mysql50_table_name_prefix) - 1];
	if ((sizeof(test_tablename)) - 1
			!= filename_to_tablename(test_filename,
						 test_tablename,
						 sizeof(test_tablename), true)
			|| strncmp(test_tablename,
				   srv_mysql50_table_name_prefix,
				   sizeof(srv_mysql50_table_name_prefix) - 1)
			|| strcmp(test_tablename
				  + sizeof(srv_mysql50_table_name_prefix) - 1,
				  test_filename)) {

		sql_print_error("tablename encoding has been changed");

		goto error;
	}
#endif /* DBUG_OFF */

	srv_log_block_size = 0;
	if (innobase_log_block_size != (1 << 9)) { /*!=512*/
		uint	n_shift;

		fprintf(stderr,
			"InnoDB: Warning: innodb_log_block_size has been "
			"changed from default value 512. (###EXPERIMENTAL### "
			"operation)\n");
		for (n_shift = 9; n_shift <= UNIV_PAGE_SIZE_SHIFT_MAX;
		     n_shift++) {
			if (innobase_log_block_size == ((ulong)1 << n_shift)) {
				srv_log_block_size = (1 << n_shift);
				fprintf(stderr,
					"InnoDB: The log block size is set to "
					ULINTPF ".\n",srv_log_block_size);
				break;
			}
		}
	} else {
		srv_log_block_size = 512;
	}
	ut_ad (srv_log_block_size >= OS_MIN_LOG_BLOCK_SIZE);

	if (!srv_log_block_size) {
		fprintf(stderr,
			"InnoDB: Error: %lu is not a valid value for "
			"innodb_log_block_size.\n"
			"InnoDB: Error: A valid value for "
			"innodb_log_block_size is\n"
			"InnoDB: Error: a power of 2 from 512 to 16384.\n",
			innobase_log_block_size);
		goto error;
	}

	/* Check that values don't overflow on 32-bit systems. */
	if (sizeof(ulint) == 4) {
		if (innobase_buffer_pool_size > UINT_MAX32) {
			sql_print_error(
				"innobase_buffer_pool_size can't be over 4GB"
				" on 32-bit systems");

			goto error;
		}
	}

	os_innodb_umask = (ulint) my_umask;

	/* First calculate the default path for innodb_data_home_dir etc.,
	in case the user has not given any value.

	Note that when using the embedded server, the datadirectory is not
	necessarily the current directory of this program. */

	if (mysqld_embedded) {
		default_path = mysql_real_data_home;
		fil_path_to_mysql_datadir = mysql_real_data_home;
	} else {
		/* It's better to use current lib, to keep paths short */
		current_dir[0] = FN_CURLIB;
		current_dir[1] = FN_LIBCHAR;
		current_dir[2] = 0;
		default_path = current_dir;
	}

	ut_a(default_path);

	/* Set InnoDB initialization parameters according to the values
	read from MySQL .cnf file */

	/*--------------- Data files -------------------------*/

	/* The default dir for data files is the datadir of MySQL */

	srv_data_home = (innobase_data_home_dir ? innobase_data_home_dir :
			 default_path);

	/* Set default InnoDB data file size to 12 MB and let it be
	auto-extending. Thus users can use InnoDB in >= 4.0 without having
	to specify any startup options. */

	if (!innobase_data_file_path) {
		innobase_data_file_path = (char*) "ibdata1:12M:autoextend";
	}

	/* Since InnoDB edits the argument in the next call, we make another
	copy of it: */

	internal_innobase_data_file_path = my_strdup(innobase_data_file_path,
						   MYF(MY_FAE));

	ret = (bool) srv_parse_data_file_paths_and_sizes(
		internal_innobase_data_file_path);
	if (ret == FALSE) {
		sql_print_error(
			"InnoDB: syntax error in innodb_data_file_path"
			" or size specified is less than 1 megabyte");
mem_free_and_error:
		srv_free_paths_and_sizes();
		my_free(internal_innobase_data_file_path);
		goto error;
	}

	/* -------------- All log files ---------------------------*/

	/* The default dir for log files is the datadir of MySQL */

	if (!srv_log_group_home_dir) {
		srv_log_group_home_dir = default_path;
	}

#ifdef UNIV_LOG_ARCHIVE
	if (!innobase_log_arch_dir) {
		innobase_log_arch_dir = srv_log_group_home_dir;
	}
	srv_arch_dir = innobase_log_arch_dir;
#endif /* UNIG_LOG_ARCHIVE */

	srv_normalize_path_for_win(srv_log_group_home_dir);

	if (strchr(srv_log_group_home_dir, ';')) {
		sql_print_error("syntax error in innodb_log_group_home_dir");
		goto mem_free_and_error;
	}

	if (innobase_mirrored_log_groups == 1) {
		sql_print_warning(
			"innodb_mirrored_log_groups is an unimplemented "
			"feature and the variable will be completely "
			"removed in a future version.");
	}

	if (innobase_mirrored_log_groups > 1) {
		sql_print_error(
		"innodb_mirrored_log_groups is an unimplemented feature and "
		"the variable will be completely removed in a future version. "
		"Using values other than 1 is not supported.");
		goto mem_free_and_error;
	}

	if (innobase_mirrored_log_groups == 0) {
		/* To throw a deprecation warning message when the option is
		passed, the default was changed to '0' (as a workaround). Since
		the only value accepted for this option is '1', reset it to 1 */
		innobase_mirrored_log_groups = 1;
	}

	/* Validate the file format by animal name */
	if (innobase_file_format_name != NULL) {

		format_id = innobase_file_format_name_lookup(
			innobase_file_format_name);

		if (format_id > UNIV_FORMAT_MAX) {

			sql_print_error("InnoDB: wrong innodb_file_format.");

			goto mem_free_and_error;
		}
	} else {
		/* Set it to the default file format id. Though this
		should never happen. */
		format_id = 0;
	}

	srv_file_format = format_id;

	/* Given the type of innobase_file_format_name we have little
	choice but to cast away the constness from the returned name.
	innobase_file_format_name is used in the MySQL set variable
	interface and so can't be const. */

	innobase_file_format_name =
		(char*) trx_sys_file_format_id_to_name(format_id);

	/* Check innobase_file_format_check variable */
	if (!innobase_file_format_check) {

		/* Set the value to disable checking. */
		srv_max_file_format_at_startup = UNIV_FORMAT_MAX + 1;

	} else {

		/* Set the value to the lowest supported format. */
		srv_max_file_format_at_startup = UNIV_FORMAT_MIN;
	}

	/* Did the user specify a format name that we support?
	As a side effect it will update the variable
	srv_max_file_format_at_startup */
	if (innobase_file_format_validate_and_set(
			innobase_file_format_max) < 0) {

		sql_print_error("InnoDB: invalid "
				"innodb_file_format_max value: "
				"should be any value up to %s or its "
				"equivalent numeric id",
				trx_sys_file_format_id_to_name(
					UNIV_FORMAT_MAX));

		goto mem_free_and_error;
	}

	if (innobase_change_buffering) {
		ulint	use;

		for (use = 0;
		     use < UT_ARR_SIZE(innobase_change_buffering_values);
		     use++) {
			if (!innobase_strcasecmp(
				    innobase_change_buffering,
				    innobase_change_buffering_values[use])) {
				ibuf_use = (ibuf_use_t) use;
				goto innobase_change_buffering_inited_ok;
			}
		}

		sql_print_error("InnoDB: invalid value "
				"innodb_change_buffering=%s",
				innobase_change_buffering);
		goto mem_free_and_error;
	}

innobase_change_buffering_inited_ok:
	ut_a((ulint) ibuf_use < UT_ARR_SIZE(innobase_change_buffering_values));
	innobase_change_buffering = (char*)
		innobase_change_buffering_values[ibuf_use];

	/* Check that interdependent parameters have sane values. */
	if (srv_max_buf_pool_modified_pct < srv_max_dirty_pages_pct_lwm) {
		sql_print_warning("InnoDB: innodb_max_dirty_pages_pct_lwm"
				  " cannot be set higher than"
				  " innodb_max_dirty_pages_pct.\n"
				  "InnoDB: Setting"
				  " innodb_max_dirty_pages_pct_lwm to %lu\n",
				  srv_max_buf_pool_modified_pct);

		srv_max_dirty_pages_pct_lwm = srv_max_buf_pool_modified_pct;
	}

	if (srv_max_io_capacity == SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT) {

		if (srv_io_capacity >= SRV_MAX_IO_CAPACITY_LIMIT / 2) {
			/* Avoid overflow. */
			srv_max_io_capacity = SRV_MAX_IO_CAPACITY_LIMIT;
		} else {
			/* The user has not set the value. We should
			set it based on innodb_io_capacity. */
			srv_max_io_capacity = static_cast<ulong>(
				ut_max(2 * srv_io_capacity, 2000));
		}

	} else if (srv_max_io_capacity < srv_io_capacity) {
		sql_print_warning("InnoDB: innodb_io_capacity"
				  " cannot be set higher than"
				  " innodb_io_capacity_max.\n"
				  "InnoDB: Setting"
				  " innodb_io_capacity to %lu\n",
				  srv_max_io_capacity);

		srv_io_capacity = srv_max_io_capacity;
	}

	if (!is_filename_allowed(srv_buf_dump_filename,
				 strlen(srv_buf_dump_filename), FALSE)) {
		sql_print_error("InnoDB: innodb_buffer_pool_filename"
			" cannot have colon (:) in the file name.");
		goto mem_free_and_error;
	}

	/* --------------------------------------------------*/

	srv_file_flush_method_str = innobase_file_flush_method;

	srv_log_file_size = (ib_uint64_t) innobase_log_file_size;

#ifdef UNIV_LOG_ARCHIVE
	srv_log_archive_on = (ulint) innobase_log_archive;
#endif /* UNIV_LOG_ARCHIVE */

	/* Check that the value of system variable innodb_page_size was
	set correctly.  Its value was put into srv_page_size. If valid,
	return the associated srv_page_size_shift.*/
	srv_page_size_shift = innodb_page_size_validate(srv_page_size);
	if (!srv_page_size_shift) {
		sql_print_error("InnoDB: Invalid page size=%lu.\n",
				srv_page_size);
		goto mem_free_and_error;
	}
	if (UNIV_PAGE_SIZE_DEF != srv_page_size) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: innodb-page-size has been changed"
			" from the default value %d to %lu.\n",
			UNIV_PAGE_SIZE_DEF, srv_page_size);
	}

	srv_log_buffer_size = (ulint) innobase_log_buffer_size;

	if (innobase_buffer_pool_instances == 0) {
		innobase_buffer_pool_instances = 8;

#if defined(__WIN__) && !defined(_WIN64)
		if (innobase_buffer_pool_size > 1331 * 1024 * 1024) {
			innobase_buffer_pool_instances
				= ut_min(MAX_BUFFER_POOLS,
					(long) (innobase_buffer_pool_size
					/ (128 * 1024 * 1024)));
		}
#endif /* defined(__WIN__) && !defined(_WIN64) */
	}
	srv_buf_pool_size = (ulint) innobase_buffer_pool_size;
	srv_buf_pool_instances = (ulint) innobase_buffer_pool_instances;

	srv_mem_pool_size = (ulint) innobase_additional_mem_pool_size;

	if (innobase_additional_mem_pool_size
	    != 8*1024*1024L /* the default */ ) {

		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Warning: Using "
			"innodb_additional_mem_pool_size is DEPRECATED. "
			"This option may be removed in future releases, "
			"together with the option innodb_use_sys_malloc "
			"and with the InnoDB's internal memory "
			"allocator.\n");
	}

	if (!srv_use_sys_malloc ) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Warning: Setting "
			"innodb_use_sys_malloc to FALSE is DEPRECATED. "
			"This option may be removed in future releases, "
			"together with the InnoDB's internal memory "
			"allocator.\n");
	}

	srv_n_file_io_threads = (ulint) innobase_file_io_threads;
	srv_n_read_io_threads = (ulint) innobase_read_io_threads;
	srv_n_write_io_threads = (ulint) innobase_write_io_threads;

	srv_use_doublewrite_buf = (ibool) innobase_use_doublewrite;

	if (!innobase_use_checksums) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Warning: Setting "
			"innodb_checksums to OFF is DEPRECATED. "
			"This option may be removed in future releases. "
			"You should set innodb_checksum_algorithm=NONE "
			"instead.\n");
		srv_checksum_algorithm = SRV_CHECKSUM_ALGORITHM_NONE;
	}

	innodb_log_checksum_func_update(srv_log_checksum_algorithm);

#ifdef HAVE_LARGE_PAGES
	if ((os_use_large_pages = (ibool) my_use_large_pages)) {
		os_large_page_size = (ulint) opt_large_page_size;
	}
#endif

	row_rollback_on_timeout = (ibool) innobase_rollback_on_timeout;

	srv_locks_unsafe_for_binlog = (ibool) innobase_locks_unsafe_for_binlog;
	if (innobase_locks_unsafe_for_binlog) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Warning: Using "
			"innodb_locks_unsafe_for_binlog is DEPRECATED. "
			"This option may be removed in future releases. "
			"Please use READ COMMITTED transaction isolation "
			"level instead, see " REFMAN "set-transaction.html.\n");
	}

	if (innobase_open_files < 10) {
		innobase_open_files = 300;
		if (srv_file_per_table && table_cache_size > 300) {
			innobase_open_files = table_cache_size;
		}
	}

	if (innobase_open_files > (long) open_files_limit) {
		fprintf(stderr,
                       "innodb_open_files should not be greater"
                       " than the open_files_limit.\n");
		if (innobase_open_files > (long) table_cache_size) {
			innobase_open_files = table_cache_size;
		}
	}

	srv_max_n_open_files = (ulint) innobase_open_files;
	srv_innodb_status = (ibool) innobase_create_status_file;

	srv_print_verbose_log = mysqld_embedded ? 0 : 1;

	/* Round up fts_sort_pll_degree to nearest power of 2 number */
	for (num_pll_degree = 1;
	     num_pll_degree < fts_sort_pll_degree;
	     num_pll_degree <<= 1) {

		/* No op */
	}

	fts_sort_pll_degree = num_pll_degree;

	/* Store the default charset-collation number of this MySQL
	installation */

	data_mysql_default_charset_coll = (ulint) default_charset_info->number;

	ut_a(DATA_MYSQL_LATIN1_SWEDISH_CHARSET_COLL ==
					my_charset_latin1.number);
	ut_a(DATA_MYSQL_BINARY_CHARSET_COLL == my_charset_bin.number);

	/* Store the latin1_swedish_ci character ordering table to InnoDB. For
	non-latin1_swedish_ci charsets we use the MySQL comparison functions,
	and consequently we do not need to know the ordering internally in
	InnoDB. */

	ut_a(0 == strcmp(my_charset_latin1.name, "latin1_swedish_ci"));
	srv_latin1_ordering = my_charset_latin1.sort_order;

	innobase_commit_concurrency_init_default();

	/* Do not enable backoff algorithm for small buffer pool. */
	if (!innodb_empty_free_list_algorithm_allowed(
			static_cast<srv_empty_free_list_t>(
				srv_empty_free_list_algorithm))) {
		sql_print_information(
				"InnoDB: innodb_empty_free_list_algorithm "
				"has been changed to legacy "
				"because of small buffer pool size. "
				"In order to use backoff, "
				"increase buffer pool at least up to 20MB.\n");
			srv_empty_free_list_algorithm
				= SRV_EMPTY_FREE_LIST_LEGACY;
	}

	srv_use_atomic_writes = (ibool) innobase_use_atomic_writes;
	if (innobase_use_atomic_writes) {
		ib_logf(IB_LOG_LEVEL_INFO, "using atomic writes.");

		/* Force doublewrite buffer off, atomic writes replace it. */
		if (srv_use_doublewrite_buf) {
			ib_logf(IB_LOG_LEVEL_INFO, "switching off doublewrite "
				"buffer because of atomic writes.");
			innobase_use_doublewrite = FALSE;
			srv_use_doublewrite_buf	= FALSE;
		}

		/* Force O_DIRECT on Unixes (on Windows writes are always
		unbuffered)*/
#ifndef _WIN32
		if(!innobase_file_flush_method ||
		   !strstr(innobase_file_flush_method, "O_DIRECT")) {
			innobase_file_flush_method =
				srv_file_flush_method_str = (char*)"O_DIRECT";
			ib_logf(IB_LOG_LEVEL_INFO,
				"using O_DIRECT due to atomic writes.");
		}
#endif
#ifdef HAVE_POSIX_FALLOCATE
		/* Due to a bug in directFS, using atomics needs
		posix_fallocate() to extend the file, because pwrite() past the
		end of the file won't work */
		srv_use_posix_fallocate = TRUE;
#endif
	}

#ifdef HAVE_PSI_INTERFACE
	/* Register keys with MySQL performance schema */
	int	count;

	count = array_elements(all_pthread_mutexes);
 	mysql_mutex_register("innodb", all_pthread_mutexes, count);

# ifdef UNIV_PFS_MUTEX
	count = array_elements(all_innodb_mutexes);
	mysql_mutex_register("innodb", all_innodb_mutexes, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
	count = array_elements(all_innodb_rwlocks);
	mysql_rwlock_register("innodb", all_innodb_rwlocks, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_THREAD
	count = array_elements(all_innodb_threads);
	mysql_thread_register("innodb", all_innodb_threads, count);
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
	count = array_elements(all_innodb_files);
	mysql_file_register("innodb", all_innodb_files, count);
# endif /* UNIV_PFS_IO */

	count = array_elements(all_innodb_conds);
	mysql_cond_register("innodb", all_innodb_conds, count);
#endif /* HAVE_PSI_INTERFACE */

	/* Since we in this module access directly the fields of a trx
	struct, and due to different headers and flags it might happen that
	ib_mutex_t has a different size in this module and in InnoDB
	modules, we check at run time that the size is the same in
	these compilation modules. */

	err = innobase_start_or_create_for_mysql();

	if (err != DB_SUCCESS) {
		goto mem_free_and_error;
	}

	/* Adjust the innodb_undo_logs config object */
	innobase_undo_logs_init_default_max();

	innobase_old_blocks_pct = static_cast<uint>(
		buf_LRU_old_ratio_update(innobase_old_blocks_pct, TRUE));

	ibuf_max_size_update(innobase_change_buffer_max_size);

	innobase_open_tables = hash_create(200);
	mysql_mutex_init(innobase_share_mutex_key,
			 &innobase_share_mutex,
			 MY_MUTEX_INIT_FAST);
	mysql_mutex_init(commit_cond_mutex_key,
			 &commit_cond_m, MY_MUTEX_INIT_FAST);
	mysql_cond_init(commit_cond_key, &commit_cond, NULL);
	innodb_inited= 1;
#ifdef MYSQL_DYNAMIC_PLUGIN
	if (innobase_hton != p) {
		innobase_hton = reinterpret_cast<handlerton*>(p);
		*innobase_hton = *innodb_hton_ptr;
	}
#endif /* MYSQL_DYNAMIC_PLUGIN */

	/* Get the current high water mark format. */
	innobase_file_format_max = (char*) trx_sys_file_format_max_get();

	/* Currently, monitor counter information are not persistent. */
	memset(monitor_set_tbl, 0, sizeof monitor_set_tbl);

	memset(innodb_counter_value, 0, sizeof innodb_counter_value);

	/* Do this as late as possible so server is fully starts up,
	since  we might get some initial stats if user choose to turn
	on some counters from start up */
	if (innobase_enable_monitor_counter) {
		innodb_enable_monitor_at_startup(
			innobase_enable_monitor_counter);
	}

	/* Turn on monitor counters that are default on */
	srv_mon_default_on();

#ifndef UNIV_HOTBACKUP
#ifdef _WIN32
	if (ut_win_init_time()) {
		goto mem_free_and_error;
	}
#endif /* _WIN32 */
#endif /* !UNIV_HOTBACKUP */

	DBUG_RETURN(FALSE);
error:
	DBUG_RETURN(TRUE);
}

/*******************************************************************//**
Closes an InnoDB database.
@return	TRUE if error */
static
int
innobase_end(
/*=========*/
	handlerton*		hton,	/*!< in/out: InnoDB handlerton */
	ha_panic_function	type MY_ATTRIBUTE((unused)))
					/*!< in: ha_panic() parameter */
{
	int	err= 0;

	DBUG_ENTER("innobase_end");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (innodb_inited) {

		srv_fast_shutdown = (ulint) innobase_fast_shutdown;

		innodb_inited = 0;
		hash_table_free(innobase_open_tables);
		innobase_open_tables = NULL;
		if (innobase_shutdown_for_mysql() != DB_SUCCESS) {
			err = 1;
		}
		srv_free_paths_and_sizes();
		my_free(internal_innobase_data_file_path);
		mysql_mutex_destroy(&innobase_share_mutex);
		mysql_mutex_destroy(&commit_cond_m);
		mysql_cond_destroy(&commit_cond);
	}

	DBUG_RETURN(err);
}

/****************************************************************//**
Flushes InnoDB logs to disk and makes a checkpoint. Really, a commit flushes
the logs, and the name of this function should be innobase_checkpoint.
@return	TRUE if error */
static
bool
innobase_flush_logs(
/*================*/
	handlerton*	hton)	/*!< in/out: InnoDB handlerton */
{
	bool	result = 0;

	DBUG_ENTER("innobase_flush_logs");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (!srv_read_only_mode) {
		log_buffer_flush_to_disk();
	}

	DBUG_RETURN(result);
}

/************************************************************//**
Synchronously read and parse the redo log up to the last
checkpoint to write the changed page bitmap.
@return 0 to indicate success.  Current implementation cannot fail. */
static
my_bool
innobase_flush_changed_page_bitmaps()
/*=================================*/
{
	if (srv_track_changed_pages) {
		os_event_reset(srv_checkpoint_completed_event);
		log_online_follow_redo_log();
	}
	return FALSE;
}

/************************************************************//**
Delete all the bitmap files for data less than the specified LSN.
If called with lsn == IB_ULONGLONG_MAX (i.e. set by RESET request),
restart the bitmap file sequence, otherwise continue it.
@return 0 to indicate success, 1 for failure. */
static
my_bool
innobase_purge_changed_page_bitmaps(
/*================================*/
	ulonglong lsn)	/*!< in: LSN to purge files up to */
{
	return (my_bool)log_online_purge_changed_page_bitmaps(lsn);
}

/** Creates a new compression dictionary. */
static
handler_create_zip_dict_result
innobase_create_zip_dict(
	handlerton*	hton,	/*!< in: innobase handlerton */
	THD*		thd,	/*!< in: handle to the MySQL thread */
	const char*	name,	/*!< in: zip dictionary name */
	ulint*		name_len,
				/*!< in/out: zip dictionary name length */
	const char*	data,	/*!< in: zip dictionary data */
	ulint*		data_len)
				/*!< in/out: zip dictionary data length */
{
	handler_create_zip_dict_result result =
		HA_CREATE_ZIP_DICT_UNKNOWN_ERROR;

	DBUG_ENTER("innobase_create_zip_dict");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (UNIV_UNLIKELY(high_level_read_only)) {
		DBUG_RETURN(HA_CREATE_ZIP_DICT_READ_ONLY);
	}
	
	if (UNIV_UNLIKELY(*name_len > ZIP_DICT_MAX_NAME_LENGTH)) {
		*name_len = ZIP_DICT_MAX_NAME_LENGTH;
		DBUG_RETURN(HA_CREATE_ZIP_DICT_NAME_TOO_LONG);
	}

	if (UNIV_UNLIKELY(*data_len > ZIP_DICT_MAX_DATA_LENGTH)) {
		*data_len = ZIP_DICT_MAX_DATA_LENGTH;
		DBUG_RETURN(HA_CREATE_ZIP_DICT_DATA_TOO_LONG);
	}

	switch (dict_create_zip_dict(name, *name_len, data, *data_len)) {
		case DB_SUCCESS:
			result = HA_CREATE_ZIP_DICT_OK;
			break;
		case DB_DUPLICATE_KEY:
			result = HA_CREATE_ZIP_DICT_ALREADY_EXISTS;
			break;
		case DB_OUT_OF_MEMORY:
			result = HA_CREATE_ZIP_DICT_OUT_OF_MEMORY;
			break;
		case DB_OUT_OF_FILE_SPACE:
			result = HA_CREATE_ZIP_DICT_OUT_OF_FILE_SPACE;
			break;
		case DB_TOO_MANY_CONCURRENT_TRXS:
			result = HA_CREATE_ZIP_DICT_TOO_MANY_CONCURRENT_TRXS;
			break;
		default:
			ut_ad(0);
			result = HA_CREATE_ZIP_DICT_UNKNOWN_ERROR;
	}
	DBUG_RETURN(result);
}

/** Drops a existing compression dictionary. */
static
handler_drop_zip_dict_result
innobase_drop_zip_dict(
	handlerton*	hton,	/*!< in: innobase handlerton */
	THD*		thd,	/*!< in: handle to the MySQL thread */
	const char*	name,	/*!< in: zip dictionary name */
	ulint*		name_len)
				/*!< in/out: zip dictionary name length */
{
	handler_drop_zip_dict_result result = HA_DROP_ZIP_DICT_UNKNOWN_ERROR;

	DBUG_ENTER("innobase_drop_zip_dict");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (UNIV_UNLIKELY(high_level_read_only)) {
		DBUG_RETURN(HA_DROP_ZIP_DICT_READ_ONLY);
	}

	switch (dict_drop_zip_dict(name, *name_len)) {
		case DB_SUCCESS:
			result = HA_DROP_ZIP_DICT_OK;
			break;
		case DB_RECORD_NOT_FOUND:
			result = HA_DROP_ZIP_DICT_DOES_NOT_EXIST;
			break;
		case DB_ROW_IS_REFERENCED:
			result = HA_DROP_ZIP_DICT_IS_REFERENCED;
			break;
		case DB_OUT_OF_MEMORY:
			result = HA_DROP_ZIP_DICT_OUT_OF_MEMORY;
			break;
		case DB_OUT_OF_FILE_SPACE:
			result = HA_DROP_ZIP_DICT_OUT_OF_FILE_SPACE;
			break;
		case DB_TOO_MANY_CONCURRENT_TRXS:
			result = HA_DROP_ZIP_DICT_TOO_MANY_CONCURRENT_TRXS;
			break;
		default:
			ut_ad(0);
			result = HA_DROP_ZIP_DICT_UNKNOWN_ERROR;
	}
	DBUG_RETURN(result);
}

/*****************************************************************//**
Check whether this is a fake change transaction.
@return TRUE if a fake change transaction */
static
my_bool
innobase_is_fake_change(
/*====================*/
	handlerton	*hton MY_ATTRIBUTE((unused)),
				/*!< in: InnoDB handlerton */
	THD*		thd)	/*!< in: MySQL thread handle of the user for
				whom the transaction is being committed */
{
	trx_t*	trx = check_trx_exists(thd);
	return UNIV_UNLIKELY(trx->fake_changes);
}

/*****************************************************************//**
Commits a transaction in an InnoDB database. */
static
void
innobase_commit_low(
/*================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef WITH_WSREP
	THD* thd = (THD*)trx->mysql_thd;
	const char* tmp = 0;
	if (wsrep_on((void*)thd)) {
#ifdef WSREP_PROC_INFO
		char info[64];
		info[sizeof(info) - 1] = '\0';
		snprintf(info, sizeof(info) - 1,
			 "innobase_commit_low():trx_commit_for_mysql(%lld)",
			 (long long) wsrep_thd_trx_seqno(thd));
		tmp = thd_proc_info(thd, info);

#else
		tmp = thd_proc_info(thd, "innobase_commit_low()");
#endif /* WSREP_PROC_INFO */
	}
#endif /* WITH_WSREP */
	if (trx_is_started(trx)) {

		trx_commit_for_mysql(trx);
	}
#ifdef WITH_WSREP
	if (wsrep_on((void*)thd)) { thd_proc_info(thd, tmp); }
#endif /* WITH_WSREP */
}

/*****************************************************************//**
Stores the current binlog coordinates in the trx system header. */
static
int
innobase_store_binlog_info(
/*=======================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	THD*		thd)	/*!< in: MySQL thread handle */
{
	const char*			file_name;
	unsigned long long 	pos;
	mtr_t			mtr;
#ifdef WITH_WSREP
        trx_sysf_t* sys_header;
#endif /* WITH_WSREP */

	DBUG_ENTER("innobase_store_binlog_info");

	thd_binlog_pos(thd, &file_name, &pos);

	mtr_start(&mtr);

#ifdef WITH_WSREP
        sys_header = trx_sysf_get(&mtr);
#endif /* WITH_WSREP */

	trx_sys_update_mysql_binlog_offset(file_name, pos,
#ifdef WITH_WSREP
					   TRX_SYS_MYSQL_LOG_INFO, sys_header, &mtr);
#else
					   TRX_SYS_MYSQL_LOG_INFO, &mtr);
#endif

	mtr_commit(&mtr);

	innobase_flush_logs(hton);

	DBUG_RETURN(0);
}

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return	0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	handlerton*	hton,	/*!< in: Innodb handlerton */
	THD*		thd)	/*!< in: MySQL thread handle of the user for
				whom the transaction should be committed */
{
	trx_t*	trx;

	DBUG_ENTER("innobase_start_trx_and_assign_read_view");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* Create a new trx struct for thd, if it does not yet have one */

	trx = check_trx_exists(thd);

	/* This is just to play safe: release a possible FIFO ticket and
	search latch. Since we can potentially reserve the trx_sys->mutex,
	we have to release the search system latch first to obey the latching
	order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* If the transaction is not started yet, start it */

	trx_start_if_not_started_xa(trx);

	/* Assign a read view if the transaction does not have it yet.
	Do this only if transaction is using REPEATABLE READ isolation
	level. */
	trx->isolation_level = innobase_map_isolation_level(
		thd_get_trx_isolation(thd));

	if (trx->isolation_level == TRX_ISO_REPEATABLE_READ) {
		trx_assign_read_view(trx);
	} else {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: WITH CONSISTENT SNAPSHOT "
				    "was ignored because this phrase "
				    "can only be used with "
				    "REPEATABLE READ isolation level.");
	}

	/* Set the MySQL flag to mark that there is an active transaction */

	innobase_register_trx(hton, current_thd, trx);

	DBUG_RETURN(0);
}


/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
clones snapshot for a consistent read from another session, if it has one.
@return	0 */
static
int
innobase_start_trx_and_clone_read_view(
/*====================================*/
	handlerton*	hton,		/* in: Innodb handlerton */
	THD*		thd,		/* in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	THD*		from_thd)	/* in: MySQL thread handle of the
					user session from which the consistent
					read should be cloned */
{
	trx_t*	trx;
	trx_t*	from_trx;

	DBUG_ENTER("innobase_start_trx_and_clone_read_view");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* Get transaction handle from the donor session */

	from_trx = thd_to_trx(from_thd);

	if (!from_trx) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: WITH CONSISTENT SNAPSHOT "
				    "FROM SESSION was ignored because the "
				    "specified session does not have an open "
				    "transaction inside InnoDB.");

		DBUG_RETURN(0);
	}

	/* Create a new trx struct for thd, if it does not yet have one */

	trx = check_trx_exists(thd);

	/* This is just to play safe: release a possible FIFO ticket and
	search latch. Since we can potentially reserve the trx_sys->mutex,
	we have to release the search system latch first to obey the latching
	order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* If the transaction is not started yet, start it */

	trx_start_if_not_started_xa(trx);

	/* Clone the read view from the donor transaction.  Do this only if
	transaction is using REPEATABLE READ isolation level. */
	trx->isolation_level = innobase_map_isolation_level(
		thd_get_trx_isolation(thd));

	if (trx->isolation_level != TRX_ISO_REPEATABLE_READ) {

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: WITH CONSISTENT SNAPSHOT "
				    "was ignored because this phrase "
				    "can only be used with "
				    "REPEATABLE READ isolation level.");
	} else {

		lock_mutex_enter();
		mutex_enter(&trx_sys->mutex);
		trx_mutex_enter(from_trx);

		if (!trx_clone_read_view(trx, from_trx)) {

			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    HA_ERR_UNSUPPORTED,
					    "InnoDB: WITH CONSISTENT SNAPSHOT "
					    "FROM SESSION was ignored because "
					    "the target transaction has not been "
					    "assigned a read view.");
		}

		trx_mutex_exit(from_trx);
		mutex_exit(&trx_sys->mutex);
		lock_mutex_exit();
	}

	/* Set the MySQL flag to mark that there is an active transaction */

	innobase_register_trx(hton, current_thd, trx);

	DBUG_RETURN(0);
}

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return	0 */
static
int
innobase_commit(
/*============*/
	handlerton*	hton,		/*!< in: Innodb handlerton */
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx)	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */
{
	trx_t*		trx;

	DBUG_ENTER("innobase_commit");
	DBUG_ASSERT(hton == innodb_hton_ptr);
	DBUG_PRINT("trans", ("ending transaction"));

	trx = check_trx_exists(thd);

	/* Since we will reserve the trx_sys->mutex, we have to release
	the search system latch first to obey the latching order. */

	/* No-op in XtraDB */
	trx_search_latch_release_if_reserved(trx);

	/* If fake-changes mode = ON then allow
	SELECT (they are read-only) and
	CREATE ... SELECT * from table (Well this doesn't open up DDL for InnoDB
	as ha_innobase::create will return appropriate error if fake-change = ON
	but if create is trying to use other SE and SELECT is executing on
	InnoDB table then we allow SELECT to proceed.
	Ideally, statement like this should be marked CREATE_SELECT like
	INSERT_SELECT but unfortunately it doesn't). */
	if (UNIV_UNLIKELY(trx->fake_changes
			  && (thd_sql_command(thd) != SQLCOM_SELECT
			      && thd_sql_command(thd) != SQLCOM_CREATE_TABLE)
			  && (commit_trx || (!thd_test_options(thd,
				OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))))) {

		/* rollback implicitly */
		innobase_rollback(hton, thd, commit_trx);

		/* because debug assertion code complains, if something left */
		thd->get_stmt_da()->reset_diagnostics_area();

		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}
	/* Transaction is deregistered only in a commit or a rollback. If
	it is deregistered we know there cannot be resources to be freed
	and we could return immediately.  For the time being, we play safe
	and do the cleanup though there should be nothing to clean up. */

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {

		sql_print_error("Transaction not registered for MySQL 2PC, "
				"but transaction is active");
	}

	if (commit_trx
	    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		DBUG_EXECUTE_IF("crash_innodb_before_commit",
				DBUG_SUICIDE(););

		/* We were instructed to commit the whole transaction, or
		this is an SQL statement end and autocommit is on */

		/* We need current binlog position for mysqlbackup to work. */
retry:
		if (innobase_commit_concurrency > 0) {
			mysql_mutex_lock(&commit_cond_m);
			commit_threads++;

			if (commit_threads > innobase_commit_concurrency) {
				commit_threads--;
				mysql_cond_wait(&commit_cond,
					&commit_cond_m);
				mysql_mutex_unlock(&commit_cond_m);
				goto retry;
			}
			else {
				mysql_mutex_unlock(&commit_cond_m);
			}
		}

		/* The following call read the binary log position of
		the transaction being committed.

                Binary logging of other engines is not relevant to
		InnoDB as all InnoDB requires is that committing
		InnoDB transactions appear in the same order in the
		MySQL binary log as they appear in InnoDB logs, which
		is guaranteed by the server.

                If the binary log is not enabled, or the transaction
                is not written to the binary log, the file name will
                be a NULL pointer. */
                unsigned long long pos;
                thd_binlog_pos(thd, &trx->mysql_log_file_name, &pos);
                trx->mysql_log_offset= static_cast<ib_int64_t>(pos);
		/* Don't do write + flush right now. For group commit
		to work we want to do the flush later. */
		trx->flush_log_later = TRUE;
		innobase_commit_low(trx);
		trx->flush_log_later = FALSE;

		if (innobase_commit_concurrency > 0) {
			mysql_mutex_lock(&commit_cond_m);
			commit_threads--;
			mysql_cond_signal(&commit_cond);
			mysql_mutex_unlock(&commit_cond_m);
		}

		trx_deregister_from_2pc(trx);

		/* Now do a write + flush of logs. */
		trx_commit_complete_for_mysql(trx);
	} else {
		/* We just mark the SQL statement ended and do not do a
		transaction commit */

		/* If we had reserved the auto-inc lock for some
		table in this SQL statement we release it now */

		lock_unlock_table_autoinc(trx);

		/* Store the current undo_no of the transaction so that we
		know where to roll back if we have to roll back the next
		SQL statement */

		trx_mark_sql_stat_end(trx);
	}

	trx->n_autoinc_rows = 0; /* Reset the number AUTO-INC rows required */

	/* This is a statement level variable. */
	trx->fts_next_doc_id = 0;

	innobase_srv_conc_force_exit_innodb(trx);

	DBUG_RETURN(0);
}

/*****************************************************************//**
Rolls back a transaction or the latest SQL statement.
@return	0 or error number */
static
int
innobase_rollback(
/*==============*/
	handlerton*	hton,		/*!< in: Innodb handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx)	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */
{
	dberr_t	error;
	trx_t*	trx;

	DBUG_ENTER("innobase_rollback");
	DBUG_ASSERT(hton == innodb_hton_ptr);
	DBUG_PRINT("trans", ("aborting transaction"));

	trx = check_trx_exists(thd);

	/* Release a possible FIFO ticket and search latch. Since we will
	reserve the trx_sys->mutex, we have to release the search system
	latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	trx->n_autoinc_rows = 0; /* Reset the number AUTO-INC rows required */

	/* If we had reserved the auto-inc lock for some table (if
	we come here to roll back the latest SQL statement) we
	release it now before a possibly lengthy rollback */

	lock_unlock_table_autoinc(trx);

	/* This is a statement level variable. */
	trx->fts_next_doc_id = 0;

	if (rollback_trx
	    || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

		error = trx_rollback_for_mysql(trx);
		trx_deregister_from_2pc(trx);
	} else {
		error = trx_rollback_last_sql_stat_for_mysql(trx);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Rolls back a transaction
@return	0 or error number */
static
int
innobase_rollback_trx(
/*==================*/
	trx_t*	trx)	/*!< in: transaction */
{
	dberr_t	error = DB_SUCCESS;

	DBUG_ENTER("innobase_rollback_trx");
	DBUG_PRINT("trans", ("aborting transaction"));

	/* Release a possible FIFO ticket and search latch. Since we will
	reserve the trx_sys->mutex, we have to release the search system
	latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* If we had reserved the auto-inc lock for some table (if
	we come here to roll back the latest SQL statement) we
	release it now before a possibly lengthy rollback */

	lock_unlock_table_autoinc(trx);

	if (!trx->read_only) {
		error = trx_rollback_for_mysql(trx);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	handlerton*	hton,		/*!< in: Innodb handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
	void*		savepoint)	/*!< in: savepoint data */
{
	ib_int64_t	mysql_binlog_cache_pos;
	dberr_t		error;
	trx_t*		trx;
	char		name[64];

	DBUG_ENTER("innobase_rollback_to_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = check_trx_exists(thd);

	/* Release a possible FIFO ticket and search latch. Since we will
	reserve the trx_sys->mutex, we have to release the search system
	latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* TODO: use provided savepoint data area to store savepoint data */

	longlong2str((ulint) savepoint, name, 36);

	error = trx_rollback_to_savepoint_for_mysql(
		trx, name, &mysql_binlog_cache_pos);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_rollback(trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
When binlog is on, MDL locks acquired after savepoint unit are not
released if there are any locks held in InnoDB.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd)		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
{
	trx_t*		trx;

	DBUG_ENTER("innobase_rollback_to_savepoint_can_release_mdl");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = check_trx_exists(thd);
	ut_ad(trx);

        /* If transaction has not acquired any locks then it is safe
	   to release MDL after rollback to savepoint */
	if (!(UT_LIST_GET_LEN(trx->lock.trx_locks))) {
		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/*****************************************************************//**
Release transaction savepoint name.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_release_savepoint(
/*=======================*/
	handlerton*	hton,		/*!< in: handlerton for Innodb */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction's
					savepoint should be released */
	void*		savepoint)	/*!< in: savepoint data */
{
	dberr_t		error;
	trx_t*		trx;
	char		name[64];

	DBUG_ENTER("innobase_release_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = check_trx_exists(thd);

	/* TODO: use provided savepoint data area to store savepoint data */

	longlong2str((ulint) savepoint, name, 36);

	error = trx_release_savepoint_for_mysql(trx, name);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_release(trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Sets a transaction savepoint.
@return	always 0, that is, always succeeds */
static
int
innobase_savepoint(
/*===============*/
	handlerton*	hton,	/*!< in: handle to the Innodb handlerton */
	THD*	thd,		/*!< in: handle to the MySQL thread */
	void*	savepoint)	/*!< in: savepoint data */
{
	dberr_t	error;
	trx_t*	trx;

	DBUG_ENTER("innobase_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* In the autocommit mode there is no sense to set a savepoint
	(unless we are in sub-statement), so SQL layer ensures that
	this method is never called in such situation.  */

	trx = check_trx_exists(thd);

	/* Release a possible FIFO ticket and search latch. Since we will
	reserve the trx_sys->mutex, we have to release the search system
	latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* Cannot happen outside of transaction */
	DBUG_ASSERT(trx_is_registered_for_2pc(trx));

	/* TODO: use provided savepoint data area to store savepoint data */
	char name[64];
	longlong2str((ulint) savepoint,name,36);

	error = trx_savepoint_for_mysql(trx, name, (ib_int64_t)0);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_take(trx, trx->fts_trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Frees a possible InnoDB trx object associated with the current THD.
@return	0 or error number */
static
int
innobase_close_connection(
/*======================*/
	handlerton*	hton,	/*!< in: innobase handlerton */
	THD*		thd)	/*!< in: handle to the MySQL thread of the user
				whose resources should be free'd */
{
	trx_t*	trx;

	DBUG_ENTER("innobase_close_connection");
	DBUG_ASSERT(hton == innodb_hton_ptr);
	trx = thd_to_trx(thd);

	ut_a(trx);

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {

		sql_print_error("Transaction not registered for MySQL 2PC, "
				"but transaction is active");
	}

	if (trx_is_started(trx) && log_warnings) {

		sql_print_warning(
			"MySQL is closing a connection that has an active "
			"InnoDB transaction.  " TRX_ID_FMT " row modifications "
			"will roll back.",
			trx->undo_no);
	}

	innobase_rollback_trx(trx);

	trx_free_for_mysql(trx);

	DBUG_RETURN(0);
}

/*****************************************************************//**
Frees a possible InnoDB trx object associated with the current THD.
@return	0 or error number */
UNIV_INTERN
int
innobase_close_thd(
/*===============*/
	THD*		thd)	/*!< in: handle to the MySQL thread of the user
				whose resources should be free'd */
{
	trx_t*	trx = thd_to_trx(thd);

	if (!trx) {
		return(0);
	}

	return(innobase_close_connection(innodb_hton_ptr, thd));
}

/*************************************************************************//**
** InnoDB database tables
*****************************************************************************/

/****************************************************************//**
Get the record format from the data dictionary.
@return one of ROW_TYPE_REDUNDANT, ROW_TYPE_COMPACT,
ROW_TYPE_COMPRESSED, ROW_TYPE_DYNAMIC */
UNIV_INTERN
enum row_type
ha_innobase::get_row_type() const
/*=============================*/
{
	if (prebuilt && prebuilt->table) {
		const ulint	flags = prebuilt->table->flags;

		switch (dict_tf_get_rec_format(flags)) {
		case REC_FORMAT_REDUNDANT:
			return(ROW_TYPE_REDUNDANT);
		case REC_FORMAT_COMPACT:
			return(ROW_TYPE_COMPACT);
		case REC_FORMAT_COMPRESSED:
			return(ROW_TYPE_COMPRESSED);
		case REC_FORMAT_DYNAMIC:
			return(ROW_TYPE_DYNAMIC);
		}
	}
	ut_ad(0);
	return(ROW_TYPE_NOT_USED);
}

/*****************************************************************//**
Cancel any pending lock request associated with the current THD. */
static
void
innobase_kill_connection(
/*======================*/
        handlerton*	hton,	/*!< in:  innobase handlerton */
	THD*	thd)	/*!< in: handle to the MySQL thread being killed */
{
        ut_ad(!lock_mutex_own());
	trx_t*	trx;

	DBUG_ENTER("innobase_kill_connection");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	lock_mutex_enter();

	trx = thd_to_trx(thd);

	if (trx)
	{
		trx_mutex_enter(trx);

		/* Cancel a pending lock request. */
		if (trx->lock.wait_lock)
			lock_cancel_waiting_and_release(trx->lock.wait_lock);

		trx_mutex_exit(trx);
	}

	lock_mutex_exit();

	DBUG_VOID_RETURN;
}



/****************************************************************//**
Get the table flags to use for the statement.
@return	table flags */
UNIV_INTERN
handler::Table_flags
ha_innobase::table_flags() const
/*============================*/
{
	/* Need to use tx_isolation here since table flags is (also)
	called before prebuilt is inited. */
	ulong const tx_isolation = thd_tx_isolation(ha_thd());

	if (tx_isolation <= ISO_READ_COMMITTED) {
		return(int_table_flags);
	}

	return(int_table_flags | HA_BINLOG_STMT_CAPABLE);
}

/****************************************************************//**
Gives the file extension of an InnoDB single-table tablespace. */
static const char* ha_innobase_exts[] = {
	".ibd",
	".isl",
	NullS
};

/****************************************************************//**
Returns the table type (storage engine name).
@return	table type */
UNIV_INTERN
const char*
ha_innobase::table_type() const
/*===========================*/
{
	return(innobase_hton_name);
}

/****************************************************************//**
Returns the index type.
@return index type */
UNIV_INTERN
const char*
ha_innobase::index_type(
/*====================*/
	uint	keynr)		/*!< : index number */
{
	dict_index_t*	index = innobase_get_index(keynr);

	if (index && index->type & DICT_FTS) {
		return("FULLTEXT");
	} else {
		return("BTREE");
	}
}

/****************************************************************//**
Returns the table file name extension.
@return	file extension string */
UNIV_INTERN
const char**
ha_innobase::bas_ext() const
/*========================*/
{
	return(ha_innobase_exts);
}

/****************************************************************//**
Returns the operations supported for indexes.
@return	flags of supported operations */
UNIV_INTERN
ulong
ha_innobase::index_flags(
/*=====================*/
	uint	key,
	uint,
	bool) const
{
	return((table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT)
		 ? 0
		 : (HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER
		  | HA_READ_RANGE | HA_KEYREAD_ONLY
		  | HA_DO_INDEX_COND_PUSHDOWN));
}

/****************************************************************//**
Returns the maximum number of keys.
@return	MAX_KEY */
UNIV_INTERN
uint
ha_innobase::max_supported_keys() const
/*===================================*/
{
	return(MAX_KEY);
}

/****************************************************************//**
Returns the maximum key length.
@return	maximum supported key length, in bytes */
UNIV_INTERN
uint
ha_innobase::max_supported_key_length() const
/*=========================================*/
{
	/* An InnoDB page must store >= 2 keys; a secondary key record
	must also contain the primary key value.  Therefore, if both
	the primary key and the secondary key are at this maximum length,
	it must be less than 1/4th of the free space on a page including
	record overhead.

	MySQL imposes its own limit to this number; MAX_KEY_LENGTH = 3072.

	For page sizes = 16k, InnoDB historically reported 3500 bytes here,
	But the MySQL limit of 3072 was always used through the handler
	interface. */

	switch (UNIV_PAGE_SIZE) {
	case 4096:
		return(768);
	case 8192:
		return(1536);
	default:
#ifdef WITH_WSREP
		return(3500);
#else
		return(3500);
#endif
	}
}

/****************************************************************//**
Returns the key map of keys that are usable for scanning.
@return	key_map_full */
UNIV_INTERN
const key_map*
ha_innobase::keys_to_use_for_scanning()
/*===================================*/
{
	return(&key_map_full);
}

/****************************************************************//**
Determines if table caching is supported.
@return	HA_CACHE_TBL_ASKTRANSACT */
UNIV_INTERN
uint8
ha_innobase::table_cache_type()
/*===========================*/
{
	return(HA_CACHE_TBL_ASKTRANSACT);
}

/****************************************************************//**
Determines if the primary key is clustered index.
@return	true */
UNIV_INTERN
bool
ha_innobase::primary_key_is_clustered()
/*===================================*/
{
	return(true);
}

/*****************************************************************//**
Normalizes a table name string. A normalized name consists of the
database name catenated to '/' and table name. Example: test/mytable.
On Windows normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE. */
static
void
normalize_table_name_low(
/*=====================*/
	char*		norm_name,	/*!< out: normalized name as a
					null-terminated string */
	const char*	name,		/*!< in: table name string */
	ibool		set_lower_case)	/*!< in: TRUE if we want to set name
					to lower case */
{
	char*	name_ptr;
	ulint	name_len;
	char*	db_ptr;
	ulint	db_len;
	char*	ptr;
	ulint	norm_len;

	/* Scan name from the end */

	ptr = strend(name) - 1;

	/* seek to the last path separator */
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
	}

	name_ptr = ptr + 1;
	name_len = strlen(name_ptr);

	/* skip any number of path separators */
	while (ptr >= name && (*ptr == '\\' || *ptr == '/')) {
		ptr--;
	}

	DBUG_ASSERT(ptr >= name);

	/* seek to the last but one path separator or one char before
	the beginning of name */
	db_len = 0;
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
		db_len++;
	}

	db_ptr = ptr + 1;

	norm_len = db_len + name_len + sizeof "/";
	ut_a(norm_len < FN_REFLEN - 1);

	memcpy(norm_name, db_ptr, db_len);

	norm_name[db_len] = '/';

	/* Copy the name and null-byte. */
	memcpy(norm_name + db_len + 1, name_ptr, name_len + 1);

	if (set_lower_case) {
		innobase_casedn_str(norm_name);
	}
}

#if !defined(DBUG_OFF)
/*********************************************************************
Test normalize_table_name_low(). */
static
void
test_normalize_table_name_low()
/*===========================*/
{
	char		norm_name[FN_REFLEN];
	const char*	test_data[][2] = {
		/* input, expected result */
		{"./mysqltest/t1", "mysqltest/t1"},
		{"./test/#sql-842b_2", "test/#sql-842b_2"},
		{"./test/#sql-85a3_10", "test/#sql-85a3_10"},
		{"./test/#sql2-842b-2", "test/#sql2-842b-2"},
		{"./test/bug29807", "test/bug29807"},
		{"./test/foo", "test/foo"},
		{"./test/innodb_bug52663", "test/innodb_bug52663"},
		{"./test/t", "test/t"},
		{"./test/t1", "test/t1"},
		{"./test/t10", "test/t10"},
		{"/a/b/db/table", "db/table"},
		{"/a/b/db///////table", "db/table"},
		{"/a/b////db///////table", "db/table"},
		{"/var/tmp/mysqld.1/#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db/table", "db/table"},
		{"ddd/t", "ddd/t"},
		{"d/ttt", "d/ttt"},
		{"d/t", "d/t"},
		{".\\mysqltest\\t1", "mysqltest/t1"},
		{".\\test\\#sql-842b_2", "test/#sql-842b_2"},
		{".\\test\\#sql-85a3_10", "test/#sql-85a3_10"},
		{".\\test\\#sql2-842b-2", "test/#sql2-842b-2"},
		{".\\test\\bug29807", "test/bug29807"},
		{".\\test\\foo", "test/foo"},
		{".\\test\\innodb_bug52663", "test/innodb_bug52663"},
		{".\\test\\t", "test/t"},
		{".\\test\\t1", "test/t1"},
		{".\\test\\t10", "test/t10"},
		{"C:\\a\\b\\db\\table", "db/table"},
		{"C:\\a\\b\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\a\\b\\\\\\\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\var\\tmp\\mysqld.1\\#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db\\table", "db/table"},
		{"ddd\\t", "ddd/t"},
		{"d\\ttt", "d/ttt"},
		{"d\\t", "d/t"},
	};

	for (size_t i = 0; i < UT_ARR_SIZE(test_data); i++) {
		printf("test_normalize_table_name_low(): "
		       "testing \"%s\", expected \"%s\"... ",
		       test_data[i][0], test_data[i][1]);

		normalize_table_name_low(norm_name, test_data[i][0], FALSE);

		if (strcmp(norm_name, test_data[i][1]) == 0) {
			printf("ok\n");
		} else {
			printf("got \"%s\"\n", norm_name);
			ut_error;
		}
	}
}

/*********************************************************************
Test ut_format_name(). */
static
void
test_ut_format_name()
/*=================*/
{
	char		buf[NAME_LEN * 3];

	struct {
		const char*	name;
		ibool		is_table;
		ulint		buf_size;
		const char*	expected;
	} test_data[] = {
		{"test/t1",	TRUE,	sizeof(buf),	"\"test\".\"t1\""},
		{"test/t1",	TRUE,	12,		"\"test\".\"t1\""},
		{"test/t1",	TRUE,	11,		"\"test\".\"t1"},
		{"test/t1",	TRUE,	10,		"\"test\".\"t"},
		{"test/t1",	TRUE,	9,		"\"test\".\""},
		{"test/t1",	TRUE,	8,		"\"test\"."},
		{"test/t1",	TRUE,	7,		"\"test\""},
		{"test/t1",	TRUE,	6,		"\"test"},
		{"test/t1",	TRUE,	5,		"\"tes"},
		{"test/t1",	TRUE,	4,		"\"te"},
		{"test/t1",	TRUE,	3,		"\"t"},
		{"test/t1",	TRUE,	2,		"\""},
		{"test/t1",	TRUE,	1,		""},
		{"test/t1",	TRUE,	0,		"BUF_NOT_CHANGED"},
		{"table",	TRUE,	sizeof(buf),	"\"table\""},
		{"ta'le",	TRUE,	sizeof(buf),	"\"ta'le\""},
		{"ta\"le",	TRUE,	sizeof(buf),	"\"ta\"\"le\""},
		{"ta`le",	TRUE,	sizeof(buf),	"\"ta`le\""},
		{"index",	FALSE,	sizeof(buf),	"\"index\""},
		{"ind/ex",	FALSE,	sizeof(buf),	"\"ind/ex\""},
	};

	for (size_t i = 0; i < UT_ARR_SIZE(test_data); i++) {

		memcpy(buf, "BUF_NOT_CHANGED", strlen("BUF_NOT_CHANGED") + 1);

		char*	ret;

		ret = ut_format_name(test_data[i].name,
				     test_data[i].is_table,
				     buf,
				     test_data[i].buf_size);

		ut_a(ret == buf);

		if (strcmp(buf, test_data[i].expected) == 0) {
			fprintf(stderr,
				"ut_format_name(%s, %s, buf, %lu), "
				"expected %s, OK\n",
				test_data[i].name,
				test_data[i].is_table ? "TRUE" : "FALSE",
				test_data[i].buf_size,
				test_data[i].expected);
		} else {
			fprintf(stderr,
				"ut_format_name(%s, %s, buf, %lu), "
				"expected %s, ERROR: got %s\n",
				test_data[i].name,
				test_data[i].is_table ? "TRUE" : "FALSE",
				test_data[i].buf_size,
				test_data[i].expected,
				buf);
			ut_error;
		}
	}
}
#endif /* !DBUG_OFF */

/********************************************************************//**
Get the upper limit of the MySQL integral and floating-point type.
@return maximum allowed value for the field */
UNIV_INTERN
ulonglong
innobase_get_int_col_max_value(
/*===========================*/
	const Field*	field)	/*!< in: MySQL field */
{
	ulonglong	max_value = 0;

	switch (field->key_type()) {
	/* TINY */
	case HA_KEYTYPE_BINARY:
		max_value = 0xFFULL;
		break;
	case HA_KEYTYPE_INT8:
		max_value = 0x7FULL;
		break;
	/* SHORT */
	case HA_KEYTYPE_USHORT_INT:
		max_value = 0xFFFFULL;
		break;
	case HA_KEYTYPE_SHORT_INT:
		max_value = 0x7FFFULL;
		break;
	/* MEDIUM */
	case HA_KEYTYPE_UINT24:
		max_value = 0xFFFFFFULL;
		break;
	case HA_KEYTYPE_INT24:
		max_value = 0x7FFFFFULL;
		break;
	/* LONG */
	case HA_KEYTYPE_ULONG_INT:
		max_value = 0xFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONG_INT:
		max_value = 0x7FFFFFFFULL;
		break;
	/* BIG */
	case HA_KEYTYPE_ULONGLONG:
		max_value = 0xFFFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONGLONG:
		max_value = 0x7FFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_FLOAT:
		/* We use the maximum as per IEEE754-2008 standard, 2^24 */
		max_value = 0x1000000ULL;
		break;
	case HA_KEYTYPE_DOUBLE:
		/* We use the maximum as per IEEE754-2008 standard, 2^53 */
		max_value = 0x20000000000000ULL;
		break;
	default:
		ut_error;
	}

	return(max_value);
}

/*******************************************************************//**
This function checks whether the index column information
is consistent between KEY info from mysql and that from innodb index.
@return TRUE if all column types match. */
static
ibool
innobase_match_index_columns(
/*=========================*/
	const KEY*		key_info,	/*!< in: Index info
						from mysql */
	const dict_index_t*	index_info)	/*!< in: Index info
						from Innodb */
{
	const KEY_PART_INFO*	key_part;
	const KEY_PART_INFO*	key_end;
	const dict_field_t*	innodb_idx_fld;
	const dict_field_t*	innodb_idx_fld_end;

	DBUG_ENTER("innobase_match_index_columns");

	/* Check whether user defined index column count matches */
	if (key_info->user_defined_key_parts !=
		index_info->n_user_defined_cols) {
		DBUG_RETURN(FALSE);
	}

	key_part = key_info->key_part;
	key_end = key_part + key_info->user_defined_key_parts;
	innodb_idx_fld = index_info->fields;
	innodb_idx_fld_end = index_info->fields + index_info->n_fields;

	/* Check each index column's datatype. We do not check
	column name because there exists case that index
	column name got modified in mysql but such change does not
	propagate to InnoDB.
	One hidden assumption here is that the index column sequences
	are matched up between those in mysql and Innodb. */
	for (; key_part != key_end; ++key_part) {
		ulint	col_type;
		ibool	is_unsigned;
		ulint	mtype = innodb_idx_fld->col->mtype;

		/* Need to translate to InnoDB column type before
		comparison. */
		col_type = get_innobase_type_from_mysql_type(&is_unsigned,
							     key_part->field);

		/* Ignore Innodb specific system columns. */
		while (mtype == DATA_SYS) {
			innodb_idx_fld++;

			if (innodb_idx_fld >= innodb_idx_fld_end) {
				DBUG_RETURN(FALSE);
			}
		}

		if (col_type != mtype) {
			/* Column Type mismatches */
			DBUG_RETURN(FALSE);
		}

		innodb_idx_fld++;
	}

	DBUG_RETURN(TRUE);
}

/*******************************************************************//**
This function builds a translation table in INNOBASE_SHARE
structure for fast index location with mysql array number from its
table->key_info structure. This also provides the necessary translation
between the key order in mysql key_info and Innodb ib_table->indexes if
they are not fully matched with each other.
Note we do not have any mutex protecting the translation table
building based on the assumption that there is no concurrent
index creation/drop and DMLs that requires index lookup. All table
handle will be closed before the index creation/drop.
@return TRUE if index translation table built successfully */
UNIV_INTERN
ibool
innobase_build_index_translation(
/*=============================*/
	const TABLE*		table,	/*!< in: table in MySQL data
					dictionary */
	dict_table_t*		ib_table,/*!< in: table in Innodb data
					dictionary */
	INNOBASE_SHARE*		share)	/*!< in/out: share structure
					where index translation table
					will be constructed in. */
{
	ulint		mysql_num_index;
	ulint		ib_num_index;
	dict_index_t**	index_mapping;
	ibool		ret = TRUE;

	DBUG_ENTER("innobase_build_index_translation");

	mutex_enter(&dict_sys->mutex);

	mysql_num_index = table->s->keys;
	ib_num_index = UT_LIST_GET_LEN(ib_table->indexes);

	index_mapping = share->idx_trans_tbl.index_mapping;

	/* If there exists inconsistency between MySQL and InnoDB dictionary
	(metadata) information, the number of index defined in MySQL
	could exceed that in InnoDB, do not build index translation
	table in such case */
	if (UNIV_UNLIKELY(ib_num_index < mysql_num_index)) {
		ret = FALSE;
		goto func_exit;
	}

	/* If index entry count is non-zero, nothing has
	changed since last update, directly return TRUE */
	if (share->idx_trans_tbl.index_count) {
		/* Index entry count should still match mysql_num_index */
		ut_a(share->idx_trans_tbl.index_count == mysql_num_index);
		goto func_exit;
	}

	/* The number of index increased, rebuild the mapping table */
	if (mysql_num_index > share->idx_trans_tbl.array_size) {
		index_mapping = (dict_index_t**) my_realloc(index_mapping,
							mysql_num_index *
							sizeof(*index_mapping),
							MYF(MY_ALLOW_ZERO_PTR));

		if (!index_mapping) {
			/* Report an error if index_mapping continues to be
			NULL and mysql_num_index is a non-zero value */
			sql_print_error("InnoDB: fail to allocate memory for "
					"index translation table. Number of "
					"Index:%lu, array size:%lu",
					mysql_num_index,
					share->idx_trans_tbl.array_size);
			ret = FALSE;
			goto func_exit;
		}

		share->idx_trans_tbl.array_size = mysql_num_index;
	}

	/* For each index in the mysql key_info array, fetch its
	corresponding InnoDB index pointer into index_mapping
	array. */
	for (ulint count = 0; count < mysql_num_index; count++) {

		/* Fetch index pointers into index_mapping according to mysql
		index sequence */
		index_mapping[count] = dict_table_get_index_on_name(
			ib_table, table->key_info[count].name);

		if (!index_mapping[count]) {
			sql_print_error("Cannot find index %s in InnoDB "
					"index dictionary.",
					table->key_info[count].name);
			ret = FALSE;
			goto func_exit;
		}

		/* Double check fetched index has the same
		column info as those in mysql key_info. */
		if (!innobase_match_index_columns(&table->key_info[count],
					          index_mapping[count])) {
			sql_print_error("Found index %s whose column info "
					"does not match that of MySQL.",
					table->key_info[count].name);
			ret = FALSE;
			goto func_exit;
		}
	}

	/* Successfully built the translation table */
	share->idx_trans_tbl.index_count = mysql_num_index;

func_exit:
	if (!ret) {
		/* Build translation table failed. */
		my_free(index_mapping);

		share->idx_trans_tbl.array_size = 0;
		share->idx_trans_tbl.index_count = 0;
		index_mapping = NULL;
	}

	share->idx_trans_tbl.index_mapping = index_mapping;

	mutex_exit(&dict_sys->mutex);

	DBUG_RETURN(ret);
}

/** This function checks if all the compression dictionaries referenced
in table->fields exist in SYS_ZIP_DICT InnoDB system table.
@return true if all referenced dictionaries exist */
UNIV_INTERN
bool
innobase_check_zip_dicts(
	const TABLE*	table,		/*!< in: table in MySQL data
					dictionary */
	ulint*		dict_ids,	/*!< out: identified zip dict ids
					(at least n_fields long) */
	trx_t*		trx,		/*!< in: transaction */
	const char**	err_dict_name)	/*!< out: the name of the
					zip_dict which does not exist. */
{
	DBUG_ENTER("innobase_check_zip_dicts");

	bool res = true;
	dberr_t err = DB_SUCCESS;
	const size_t n_fields = table->s->fields;

	Field* field_ptr;
	for (size_t field_idx = 0; err == DB_SUCCESS && field_idx < n_fields;
		++field_idx)
	{
		field_ptr = table->field[field_idx];
		if (field_ptr->has_associated_compression_dictionary()) {
			err = dict_create_get_zip_dict_id_by_name(
				field_ptr->zip_dict_name.str,
				field_ptr->zip_dict_name.length,
				&dict_ids[field_idx],
				trx);
			ut_a(err == DB_SUCCESS || err == DB_RECORD_NOT_FOUND);
		}
		else {
			dict_ids[field_idx] = ULINT_UNDEFINED;
		}

	}

	if (err != DB_SUCCESS) {
		res = false;
		*err_dict_name = field_ptr->zip_dict_name.str;
	}

	DBUG_RETURN(res);
}

/** This function creates compression dictionary references in
SYS_ZIP_DICT_COLS InnoDB system table for table_id based on info
in table->fields and provided zip dict ids. */
UNIV_INTERN
void
innobase_create_zip_dict_references(
	const TABLE*	table,		/*!< in: table in MySQL data
					dictionary */
	table_id_t	ib_table_id,	/*!< in: table ID in Innodb data
					dictionary */
	ulint*		zip_dict_ids,	/*!< in: zip dict ids
					(at least n_fields long) */
	trx_t*		trx)		/*!< in: transaction */
{
	DBUG_ENTER("innobase_create_zip_dict_references");

	dberr_t err = DB_SUCCESS;
	const size_t n_fields = table->s->fields;

	for (size_t field_idx = 0; err == DB_SUCCESS && field_idx < n_fields;
		++field_idx)
	{
		if (zip_dict_ids[field_idx] != ULINT_UNDEFINED) {
			err = dict_create_add_zip_dict_reference(ib_table_id,
				table->field[field_idx]->field_index,
				zip_dict_ids[field_idx], trx);
			ut_a(err == DB_SUCCESS);
		}
	}

	DBUG_VOID_RETURN;
}

/*******************************************************************//**
This function uses index translation table to quickly locate the
requested index structure.
Note we do not have mutex protection for the index translatoin table
access, it is based on the assumption that there is no concurrent
translation table rebuild (fter create/drop index) and DMLs that
require index lookup.
@return dict_index_t structure for requested index. NULL if
fail to locate the index structure. */
static
dict_index_t*
innobase_index_lookup(
/*==================*/
	INNOBASE_SHARE*	share,	/*!< in: share structure for index
				translation table. */
	uint		keynr)	/*!< in: index number for the requested
				index */
{
	if (!share->idx_trans_tbl.index_mapping
	    || keynr >= share->idx_trans_tbl.index_count) {
		return(NULL);
	}

	return(share->idx_trans_tbl.index_mapping[keynr]);
}

/************************************************************************
Set the autoinc column max value. This should only be called once from
ha_innobase::open(). Therefore there's no need for a covering lock. */
UNIV_INTERN
void
ha_innobase::innobase_initialize_autoinc()
/*======================================*/
{
	ulonglong	auto_inc;
	const Field*	field = table->found_next_number_field;

	if (field != NULL) {
		auto_inc = innobase_get_int_col_max_value(field);
	} else {
		/* We have no idea what's been passed in to us as the
		autoinc column. We set it to the 0, effectively disabling
		updates to the table. */
		auto_inc = 0;

		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: Unable to determine the AUTOINC "
				"column name\n");
	}

	if (srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE) {
		/* If the recovery level is set so high that writes
		are disabled we force the AUTOINC counter to 0
		value effectively disabling writes to the table.
		Secondly, we avoid reading the table in case the read
		results in failure due to a corrupted table/index.

		We will not return an error to the client, so that the
		tables can be dumped with minimal hassle.  If an error
		were returned in this case, the first attempt to read
		the table would fail and subsequent SELECTs would succeed. */
		auto_inc = 0;
	} else if (field == NULL) {
		/* This is a far more serious error, best to avoid
		opening the table and return failure. */
		my_error(ER_AUTOINC_READ_FAILED, MYF(0));
	} else {
		dict_index_t*	index;
		const char*	col_name;
		ib_uint64_t	read_auto_inc;
		ulint		err;

		update_thd(ha_thd());

		ut_a(prebuilt->trx == thd_to_trx(user_thd));

		col_name = field->field_name;
		index = innobase_get_index(table->s->next_number_index);

		/* Execute SELECT MAX(col_name) FROM TABLE; */
		err = row_search_max_autoinc(index, col_name, &read_auto_inc);

		switch (err) {
		case DB_SUCCESS: {
			ulonglong	col_max_value;

			col_max_value = innobase_get_int_col_max_value(field);

			/* At the this stage we do not know the increment
			nor the offset, so use a default increment of 1. */

			auto_inc = innobase_next_autoinc(
				read_auto_inc, 1, 1, 0, col_max_value);

			break;
		}
		case DB_RECORD_NOT_FOUND:
			ut_print_timestamp(stderr);
			fprintf(stderr, "  InnoDB: MySQL and InnoDB data "
				"dictionaries are out of sync.\n"
				"InnoDB: Unable to find the AUTOINC column "
				"%s in the InnoDB table %s.\n"
				"InnoDB: We set the next AUTOINC column "
				"value to 0,\n"
				"InnoDB: in effect disabling the AUTOINC "
				"next value generation.\n"
				"InnoDB: You can either set the next "
				"AUTOINC value explicitly using ALTER TABLE\n"
				"InnoDB: or fix the data dictionary by "
				"recreating the table.\n",
				col_name, index->table->name);

			/* This will disable the AUTOINC generation. */
			auto_inc = 0;

			/* We want the open to succeed, so that the user can
			take corrective action. ie. reads should succeed but
			updates should fail. */
			err = DB_SUCCESS;
			break;
		default:
			/* row_search_max_autoinc() should only return
			one of DB_SUCCESS or DB_RECORD_NOT_FOUND. */
			ut_error;
		}
	}

	dict_table_autoinc_initialize(prebuilt->table, auto_inc);
}

/*****************************************************************//**
Creates and opens a handle to a table which already exists in an InnoDB
database.
@return	1 if error, 0 if success */
UNIV_INTERN
int
ha_innobase::open(
/*==============*/
	const char*		name,		/*!< in: table name */
	int			mode,		/*!< in: not used */
	uint			test_if_locked)	/*!< in: not used */
{
	dict_table_t*		ib_table;
	char			norm_name[FN_REFLEN];
	THD*			thd;
	char*			is_part = NULL;
	ibool			par_case_name_set = FALSE;
	char			par_case_name[FN_REFLEN];
	dict_err_ignore_t	ignore_err = DICT_ERR_IGNORE_NONE;

	DBUG_ENTER("ha_innobase::open");

	UT_NOT_USED(mode);
	UT_NOT_USED(test_if_locked);

	thd = ha_thd();

	/* No-op in XtraDB */
	innobase_release_temporary_latches(ht, thd);

	normalize_table_name(norm_name, name);

	user_thd = NULL;

	if (!(share=get_share(name))) {

		DBUG_RETURN(1);
	}

	if (UNIV_UNLIKELY(share->ib_table && share->ib_table->is_corrupt &&
			  srv_pass_corrupt_table <= 1)) {
		free_share(share);

		DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
	}

	/* Will be allocated if it is needed in ::update_row() */
	upd_buf = NULL;
	upd_buf_size = 0;

	/* We look for pattern #P# to see if the table is partitioned
	MySQL table. */
#ifdef __WIN__
	is_part = strstr(norm_name, "#p#");
#else
	is_part = strstr(norm_name, "#P#");
#endif /* __WIN__ */

	/* Check whether FOREIGN_KEY_CHECKS is set to 0. If so, the table
	can be opened even if some FK indexes are missing. If not, the table
	can't be opened in the same situation */
	if (thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS)) {
		ignore_err = DICT_ERR_IGNORE_FK_NOKEY;
	}

	/* Get pointer to a table object in InnoDB dictionary cache */
	ib_table = dict_table_open_on_name(norm_name, FALSE, TRUE, ignore_err);

	if (ib_table
	    && ((!DICT_TF2_FLAG_IS_SET(ib_table, DICT_TF2_FTS_HAS_DOC_ID)
		 && table->s->fields != dict_table_get_n_user_cols(ib_table))
		|| (DICT_TF2_FLAG_IS_SET(ib_table, DICT_TF2_FTS_HAS_DOC_ID)
		    && (table->s->fields
			!= dict_table_get_n_user_cols(ib_table) - 1)))) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"table %s contains %lu user defined columns "
			"in InnoDB, but %lu columns in MySQL. Please "
			"check INFORMATION_SCHEMA.INNODB_SYS_COLUMNS and "
			REFMAN "innodb-troubleshooting.html "
			"for how to resolve it",
			norm_name, (ulong) dict_table_get_n_user_cols(ib_table),
			(ulong) table->s->fields);

		/* Mark this table as corrupted, so the drop table
		or force recovery can still use it, but not others. */
		ib_table->corrupted = true;
		dict_table_close(ib_table, FALSE, FALSE);
		ib_table = NULL;
		is_part = NULL;
	}

	if (UNIV_UNLIKELY(ib_table && ib_table->is_corrupt &&
			  srv_pass_corrupt_table <= 1)) {
		free_share(share);
		my_free(upd_buf);
		upd_buf = NULL;
		upd_buf_size = 0;

		DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
	}

	share->ib_table = ib_table;

	if (NULL == ib_table) {
		if (is_part) {
			/* MySQL partition engine hard codes the file name
			separator as "#P#". The text case is fixed even if
			lower_case_table_names is set to 1 or 2. This is true
			for sub-partition names as well. InnoDB always
			normalises file names to lower case on Windows, this
			can potentially cause problems when copying/moving
			tables between platforms.

			1) If boot against an installation from Windows
			platform, then its partition table name could
			be in lower case in system tables. So we will
			need to check lower case name when load table.

			2) If we boot an installation from other case
			sensitive platform in Windows, we might need to
			check the existence of table name without lower
			case in the system table. */
			if (innobase_get_lower_case_table_names() == 1) {

				if (!par_case_name_set) {
#ifndef __WIN__
					/* Check for the table using lower
					case name, including the partition
					separator "P" */
					strcpy(par_case_name, norm_name);
					innobase_casedn_str(par_case_name);
#else
					/* On Windows platfrom, check
					whether there exists table name in
					system table whose name is
					not being normalized to lower case */
					normalize_table_name_low(
						par_case_name, name, FALSE);
#endif
					par_case_name_set = TRUE;
				}

				ib_table = dict_table_open_on_name(
					par_case_name, FALSE, TRUE,
					ignore_err);
			}

			if (ib_table) {
#ifndef __WIN__
				sql_print_warning("Partition table %s opened "
						  "after converting to lower "
						  "case. The table may have "
						  "been moved from a case "
						  "in-sensitive file system. "
						  "Please recreate table in "
						  "the current file system\n",
						  norm_name);
#else
				sql_print_warning("Partition table %s opened "
						  "after skipping the step to "
						  "lower case the table name. "
						  "The table may have been "
						  "moved from a case sensitive "
						  "file system. Please "
						  "recreate table in the "
						  "current file system\n",
						  norm_name);
#endif
				goto table_opened;
			}
		}

		if (is_part) {
			sql_print_error("Failed to open table %s.\n",
					norm_name);
		}

		ib_logf(IB_LOG_LEVEL_WARN,
			"Cannot open table %s from the internal data "
			"dictionary of InnoDB though the .frm file "
			"for the table exists. See "
			REFMAN "innodb-troubleshooting.html for how "
			"you can resolve the problem.", norm_name);

		free_share(share);
		my_errno = ENOENT;

		DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
	}

table_opened:

	innobase_copy_frm_flags_from_table_share(ib_table, table->s);

	dict_stats_init(ib_table);

	MONITOR_INC(MONITOR_TABLE_OPEN);

	bool	no_tablespace;

	if (dict_table_is_discarded(ib_table)) {

		ib_senderrf(thd,
			IB_LOG_LEVEL_WARN, ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		/* Allow an open because a proper DISCARD should have set
		all the flags and index root page numbers to FIL_NULL that
		should prevent any DML from running but it should allow DDL
		operations. */

		no_tablespace = false;

	} else if (ib_table->ibd_file_missing) {

		ib_senderrf(
			thd, IB_LOG_LEVEL_WARN,
			ER_TABLESPACE_MISSING, norm_name);

		/* This means we have no idea what happened to the tablespace
		file, best to play it safe. */

		no_tablespace = true;
	} else {
		no_tablespace = false;
	}

	if (!thd_tablespace_op(thd) && no_tablespace) {
		free_share(share);
		my_errno = ENOENT;

		dict_table_close(ib_table, FALSE, FALSE);

		DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
	}

	prebuilt = row_create_prebuilt(ib_table, table->s->reclength);

	prebuilt->default_rec = table->s->default_values;
	ut_ad(prebuilt->default_rec);

	prebuilt->mysql_handler = this;

	/* Looks like MySQL-3.23 sometimes has primary key number != 0 */
	primary_key = table->s->primary_key;
	key_used_on_scan = primary_key;

	if (!innobase_build_index_translation(table, ib_table, share)) {
		  sql_print_error("Build InnoDB index translation table for"
				  " Table %s failed", name);
	}

	/* Allocate a buffer for a 'row reference'. A row reference is
	a string of bytes of length ref_length which uniquely specifies
	a row in our table. Note that MySQL may also compare two row
	references for equality by doing a simple memcmp on the strings
	of length ref_length! */

	if (!row_table_got_default_clust_index(ib_table)) {

		prebuilt->clust_index_was_generated = FALSE;

		if (UNIV_UNLIKELY(primary_key >= MAX_KEY)) {
			sql_print_error("Table %s has a primary key in "
					"InnoDB data dictionary, but not "
					"in MySQL!", name);

			/* This mismatch could cause further problems
			if not attended, bring this to the user's attention
			by printing a warning in addition to log a message
			in the errorlog */
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_NO_SUCH_INDEX,
					    "InnoDB: Table %s has a "
					    "primary key in InnoDB data "
					    "dictionary, but not in "
					    "MySQL!", name);

			/* If primary_key >= MAX_KEY, its (primary_key)
			value could be out of bound if continue to index
			into key_info[] array. Find InnoDB primary index,
			and assign its key_length to ref_length.
			In addition, since MySQL indexes are sorted starting
			with primary index, unique index etc., initialize
			ref_length to the first index key length in
			case we fail to find InnoDB cluster index.

			Please note, this will not resolve the primary
			index mismatch problem, other side effects are
			possible if users continue to use the table.
			However, we allow this table to be opened so
			that user can adopt necessary measures for the
			mismatch while still being accessible to the table
			date. */
			if (!table->key_info) {
				ut_ad(!table->s->keys);
				ref_length = 0;
			} else {
				ref_length = table->key_info[0].key_length;
			}

			/* Find corresponding cluster index
			key length in MySQL's key_info[] array */
			for (uint i = 0; i < table->s->keys; i++) {
				dict_index_t*	index;
				index = innobase_get_index(i);
				if (dict_index_is_clust(index)) {
					ref_length =
						 table->key_info[i].key_length;
				}
			}
		} else {
			/* MySQL allocates the buffer for ref.
			key_info->key_length includes space for all key
			columns + one byte for each column that may be
			NULL. ref_length must be as exact as possible to
			save space, because all row reference buffers are
			allocated based on ref_length. */

			ref_length = table->key_info[primary_key].key_length;
		}
	} else {
		if (primary_key != MAX_KEY) {
			sql_print_error(
				"Table %s has no primary key in InnoDB data "
				"dictionary, but has one in MySQL! If you "
				"created the table with a MySQL version < "
				"3.23.54 and did not define a primary key, "
				"but defined a unique key with all non-NULL "
				"columns, then MySQL internally treats that "
				"key as the primary key. You can fix this "
				"error by dump + DROP + CREATE + reimport "
				"of the table.", name);

			/* This mismatch could cause further problems
			if not attended, bring this to the user attention
			by printing a warning in addition to log a message
			in the errorlog */
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_NO_SUCH_INDEX,
					    "InnoDB: Table %s has no "
					    "primary key in InnoDB data "
					    "dictionary, but has one in "
					    "MySQL!", name);
		}

		prebuilt->clust_index_was_generated = TRUE;

		ref_length = DATA_ROW_ID_LEN;

		/* If we automatically created the clustered index, then
		MySQL does not know about it, and MySQL must NOT be aware
		of the index used on scan, to make it avoid checking if we
		update the column of the index. That is why we assert below
		that key_used_on_scan is the undefined value MAX_KEY.
		The column is the row id in the automatical generation case,
		and it will never be updated anyway. */

		if (key_used_on_scan != MAX_KEY) {
			sql_print_warning(
				"Table %s key_used_on_scan is %lu even "
				"though there is no primary key inside "
				"InnoDB.", name, (ulong) key_used_on_scan);
		}
	}

	/* Index block size in InnoDB: used by MySQL in query optimization */
	stats.block_size = UNIV_PAGE_SIZE;

	/* Init table lock structure */
	thr_lock_data_init(&share->lock,&lock,(void*) 0);

	if (prebuilt->table) {
		/* We update the highest file format in the system table
		space, if this table has higher file format setting. */

		trx_sys_file_format_max_upgrade(
			(const char**) &innobase_file_format_max,
			dict_table_get_format(prebuilt->table));
	}

	/* Only if the table has an AUTOINC column. */
	if (prebuilt->table != NULL
	    && !prebuilt->table->ibd_file_missing
	    && table->found_next_number_field != NULL) {
		dict_table_autoinc_lock(prebuilt->table);

		/* Since a table can already be "open" in InnoDB's internal
		data dictionary, we only init the autoinc counter once, the
		first time the table is loaded. We can safely reuse the
		autoinc value from a previous MySQL open. */
		if (dict_table_autoinc_read(prebuilt->table) == 0) {

			innobase_initialize_autoinc();
		}

		dict_table_autoinc_unlock(prebuilt->table);
	}

	info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

	DBUG_RETURN(0);
}

UNIV_INTERN
handler*
ha_innobase::clone(
/*===============*/
	const char*	name,		/*!< in: table name */
	MEM_ROOT*	mem_root)	/*!< in: memory context */
{
	ha_innobase* new_handler;

	DBUG_ENTER("ha_innobase::clone");

	new_handler = static_cast<ha_innobase*>(handler::clone(name,
							       mem_root));
	if (new_handler) {
		DBUG_ASSERT(new_handler->prebuilt != NULL);

		new_handler->prebuilt->select_lock_type
			= prebuilt->select_lock_type;
	}

	DBUG_RETURN(new_handler);
}

UNIV_INTERN
uint
ha_innobase::max_supported_key_part_length() const
/*==============================================*/
{
	/* A table format specific index column length check will be performed
	at ha_innobase::add_index() and row_create_index_for_mysql() */
	return(innobase_large_prefix
		? REC_VERSION_56_MAX_INDEX_COL_LEN
		: REC_ANTELOPE_MAX_INDEX_COL_LEN - 1);
}

/******************************************************************//**
Closes a handle to an InnoDB table.
@return	0 */
UNIV_INTERN
int
ha_innobase::close()
/*================*/
{
	THD*	thd;

	DBUG_ENTER("ha_innobase::close");

	thd = ha_thd();

	/* No-op in XtraDB */
	innobase_release_temporary_latches(ht, thd);

	row_prebuilt_free(prebuilt, FALSE);

	if (upd_buf != NULL) {
		ut_ad(upd_buf_size != 0);
		my_free(upd_buf);
		upd_buf = NULL;
		upd_buf_size = 0;
	}

	free_share(share);

	MONITOR_INC(MONITOR_TABLE_CLOSE);

	/* Tell InnoDB server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	DBUG_RETURN(0);
}

/* The following accessor functions should really be inside MySQL code! */

/**************************************************************//**
Gets field offset for a field in a table.
@return	offset */
static inline
uint
get_field_offset(
/*=============*/
	const TABLE*	table,	/*!< in: MySQL table object */
	const Field*	field)	/*!< in: MySQL field object */
{
	return((uint) (field->ptr - table->record[0]));
}
#ifdef WITH_WSREP
UNIV_INTERN
int
wsrep_innobase_mysql_sort(
					/* out: str contains sort string */
	int		mysql_type,	/* in: MySQL type */
	uint		charset_number,	/* in: number of the charset */
	unsigned char*	str,		/* in: data field */
	unsigned int	str_length,	/* in: data field length,
					not UNIV_SQL_NULL */
	unsigned int	buf_length)	/* in: total str buffer length */

{
	CHARSET_INFO*		charset;
	enum_field_types	mysql_tp;
	int ret_length =	str_length;

	DBUG_ASSERT(str_length != UNIV_SQL_NULL);

	mysql_tp = (enum_field_types) mysql_type;

	switch (mysql_tp) {

	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
	{
		uchar tmp_str[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		uint tmp_length = REC_VERSION_56_MAX_INDEX_COL_LEN;

		/* Use the charset number to pick the right charset struct for
		the comparison. Since the MySQL function get_charset may be
		slow before Bar removes the mutex operation there, we first
		look at 2 common charsets directly. */

		if (charset_number == default_charset_info->number) {
			charset = default_charset_info;
		} else if (charset_number == my_charset_latin1.number) {
			charset = &my_charset_latin1;
		} else {
			charset = get_charset(charset_number, MYF(MY_WME));

			if (charset == NULL) {
			  sql_print_error("InnoDB needs charset %lu for doing "
					  "a comparison, but MySQL cannot "
					  "find that charset.",
					  (ulong) charset_number);
				ut_a(0);
			}
		}

		ut_a(str_length <= tmp_length);
		memcpy(tmp_str, str, str_length);

		if (wsrep_protocol_version < 3) {
			tmp_length = charset->coll->strnxfrm(
				charset, str, str_length,
				str_length, tmp_str, tmp_length, 0);
			DBUG_ASSERT(tmp_length <= str_length);
		} else {
			/* strnxfrm will expand the destination string,
			   protocols < 3 truncated the sorted sring
			   protocols >= 3 gets full sorted sring
			*/
			tmp_length = charset->coll->strnxfrm(
				charset, str, buf_length,
				str_length, tmp_str, str_length, 0);
			DBUG_ASSERT(tmp_length <= buf_length);
			ret_length = tmp_length;
		}
 
		break;
	}
	case MYSQL_TYPE_DECIMAL :
	case MYSQL_TYPE_TINY :
	case MYSQL_TYPE_SHORT :
	case MYSQL_TYPE_LONG :
	case MYSQL_TYPE_FLOAT :
	case MYSQL_TYPE_DOUBLE :
	case MYSQL_TYPE_NULL :
	case MYSQL_TYPE_TIMESTAMP :
	case MYSQL_TYPE_LONGLONG :
	case MYSQL_TYPE_INT24 :
	case MYSQL_TYPE_DATE :
	case MYSQL_TYPE_TIME :
	case MYSQL_TYPE_DATETIME :
	case MYSQL_TYPE_YEAR :
	case MYSQL_TYPE_NEWDATE :
	case MYSQL_TYPE_NEWDECIMAL :
	case MYSQL_TYPE_ENUM :
	case MYSQL_TYPE_SET :
	case MYSQL_TYPE_GEOMETRY :
		break;
	default:
		break;
	}

	return ret_length;
}
#endif // WITH_WSREP

/*************************************************************//**
InnoDB uses this function to compare two data fields for which the data type
is such that we must use MySQL code to compare them. NOTE that the prototype
of this function is in rem0cmp.cc in InnoDB source code! If you change this
function, remember to update the prototype there!
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
UNIV_INTERN
int
innobase_mysql_cmp(
/*===============*/
	int		mysql_type,	/*!< in: MySQL type */
	uint		charset_number,	/*!< in: number of the charset */
	const unsigned char* a,		/*!< in: data field */
	unsigned int	a_length,	/*!< in: data field length,
					not UNIV_SQL_NULL */
	const unsigned char* b,		/*!< in: data field */
	unsigned int	b_length)	/*!< in: data field length,
					not UNIV_SQL_NULL */
{
	CHARSET_INFO*		charset;
	enum_field_types	mysql_tp;
	int			ret;

	DBUG_ASSERT(a_length != UNIV_SQL_NULL);
	DBUG_ASSERT(b_length != UNIV_SQL_NULL);

	mysql_tp = (enum_field_types) mysql_type;

	switch (mysql_tp) {

	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
		/* Use the charset number to pick the right charset struct for
		the comparison. Since the MySQL function get_charset may be
		slow before Bar removes the mutex operation there, we first
		look at 2 common charsets directly. */

		if (charset_number == default_charset_info->number) {
			charset = default_charset_info;
		} else if (charset_number == my_charset_latin1.number) {
			charset = &my_charset_latin1;
		} else {
			charset = get_charset(charset_number, MYF(MY_WME));

			if (charset == NULL) {
			  sql_print_error("InnoDB needs charset %lu for doing "
					  "a comparison, but MySQL cannot "
					  "find that charset.",
					  (ulong) charset_number);
				ut_a(0);
			}
		}

		/* Starting from 4.1.3, we use strnncollsp() in comparisons of
		non-latin1_swedish_ci strings. NOTE that the collation order
		changes then: 'b\0\0...' is ordered BEFORE 'b  ...'. Users
		having indexes on such data need to rebuild their tables! */

		ret = charset->coll->strnncollsp(
			charset, a, a_length, b, b_length, 0);

		if (ret < 0) {
			return(-1);
		} else if (ret > 0) {
			return(1);
		} else {
			return(0);
		}
	default:
		ut_error;
	}

	return(0);
}


/*************************************************************//**
Get the next token from the given string and store it in *token. */
UNIV_INTERN
CHARSET_INFO*
innobase_get_fts_charset(
/*=====================*/
	int		mysql_type,	/*!< in: MySQL type */
	uint		charset_number)	/*!< in: number of the charset */
{
	enum_field_types	mysql_tp;
	CHARSET_INFO*		charset;

	mysql_tp = (enum_field_types) mysql_type;

	switch (mysql_tp) {

	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
		/* Use the charset number to pick the right charset struct for
		the comparison. Since the MySQL function get_charset may be
		slow before Bar removes the mutex operation there, we first
		look at 2 common charsets directly. */

		if (charset_number == default_charset_info->number) {
			charset = default_charset_info;
		} else if (charset_number == my_charset_latin1.number) {
			charset = &my_charset_latin1;
		} else {
			charset = get_charset(charset_number, MYF(MY_WME));

			if (charset == NULL) {
			  sql_print_error("InnoDB needs charset %lu for doing "
					  "a comparison, but MySQL cannot "
					  "find that charset.",
					  (ulong) charset_number);
				ut_a(0);
			}
		}
		break;
	default:
		ut_error;
	}

	return(charset);
}

/*************************************************************//**
InnoDB uses this function to compare two data fields for which the data type
is such that we must use MySQL code to compare them. NOTE that the prototype
of this function is in rem0cmp.c in InnoDB source code! If you change this
function, remember to update the prototype there!
@return	1, 0, -1, if a is greater, equal, less than b, respectively */
UNIV_INTERN
int
innobase_mysql_cmp_prefix(
/*======================*/
	int		mysql_type,	/*!< in: MySQL type */
	uint		charset_number,	/*!< in: number of the charset */
	const unsigned char* a,		/*!< in: data field */
	unsigned int	a_length,	/*!< in: data field length,
					not UNIV_SQL_NULL */
	const unsigned char* b,		/*!< in: data field */
	unsigned int	b_length)	/*!< in: data field length,
					not UNIV_SQL_NULL */
{
	CHARSET_INFO*		charset;
	int			result;

	charset = innobase_get_fts_charset(mysql_type, charset_number);

	result = ha_compare_text(charset, (uchar*) a, a_length,
				 (uchar*) b, b_length, 1, 0);

	return(result);
}
/******************************************************************//**
compare two character string according to their charset. */
UNIV_INTERN
int
innobase_fts_text_cmp(
/*==================*/
	const void*	cs,		/*!< in: Character set */
	const void*     p1,		/*!< in: key */
	const void*     p2)		/*!< in: node */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;

	return(ha_compare_text(
		charset, s1->f_str, static_cast<uint>(s1->f_len),
		s2->f_str, static_cast<uint>(s2->f_len), 0, 0));
}
/******************************************************************//**
compare two character string case insensitively according to their charset. */
UNIV_INTERN
int
innobase_fts_text_case_cmp(
/*=======================*/
	const void*	cs,		/*!< in: Character set */
	const void*     p1,		/*!< in: key */
	const void*     p2)		/*!< in: node */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;
	ulint			newlen;

	my_casedn_str(charset, (char*) s2->f_str);

	newlen = strlen((const char*) s2->f_str);

	return(ha_compare_text(
		charset, s1->f_str, static_cast<uint>(s1->f_len),
		s2->f_str, static_cast<uint>(newlen), 0, 0));
}
/******************************************************************//**
Get the first character's code position for FTS index partition. */
UNIV_INTERN
ulint
innobase_strnxfrm(
/*==============*/
	const CHARSET_INFO*
			cs,		/*!< in: Character set */
	const uchar*	str,		/*!< in: string */
	const ulint	len)		/*!< in: string length */
{
	uchar		mystr[2];
	ulint		value;

	if (!str || len == 0) {
		return(0);
	}

	my_strnxfrm(cs, (uchar*) mystr, 2, str, len);

	value = mach_read_from_2(mystr);

	if (value > 255) {
		value = value / 256;
	}

	return(value);
}

/******************************************************************//**
compare two character string according to their charset. */
UNIV_INTERN
int
innobase_fts_text_cmp_prefix(
/*=========================*/
	const void*	cs,		/*!< in: Character set */
	const void*	p1,		/*!< in: prefix key */
	const void*	p2)		/*!< in: value to compare */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;
	int			result;

	result = ha_compare_text(
		charset, s2->f_str, static_cast<uint>(s2->f_len),
		s1->f_str, static_cast<uint>(s1->f_len), 1, 0);

	/* We switched s1, s2 position in ha_compare_text. So we need
	to negate the result */
	return(-result);
}

/******************************************************************//**
Makes all characters in a string lower case. */
UNIV_INTERN
size_t
innobase_fts_casedn_str(
/*====================*/
	CHARSET_INFO*	cs,	/*!< in: Character set */
	char*		src,	/*!< in: string to put in lower case */
	size_t		src_len,/*!< in: input string length */
	char*		dst,	/*!< in: buffer for result string */
	size_t		dst_len)/*!< in: buffer size */
{
	if (cs->casedn_multiply == 1) {
		memcpy(dst, src, src_len);
		dst[src_len] = 0;
		my_casedn_str(cs, dst);

		return(strlen(dst));
	} else {
		return(cs->cset->casedn(cs, src, src_len, dst, dst_len));
	}
}

#define true_word_char(c, ch) ((c) & (_MY_U | _MY_L | _MY_NMR) || (ch) == '_')

#define misc_word_char(X)       0

/*************************************************************//**
Get the next token from the given string and store it in *token.
It is mostly copied from MyISAM's doc parsing function ft_simple_get_word()
@return length of string processed */
UNIV_INTERN
ulint
innobase_mysql_fts_get_token(
/*=========================*/
	CHARSET_INFO*	cs,		/*!< in: Character set */
	const byte*	start,		/*!< in: start of text */
	const byte*	end,		/*!< in: one character past end of
					text */
	fts_string_t*	token,		/*!< out: token's text */
	ulint*		offset)		/*!< out: offset to token,
					measured as characters from
					'start' */
{
	int		mbl;
	const uchar*	doc = start;

	ut_a(cs);

	token->f_n_char = token->f_len = 0;
	token->f_str = NULL;

	for (;;) {

		if (doc >= end) {
			return(doc - start);
		}

		int	ctype;

		mbl = cs->cset->ctype(
			cs, &ctype, doc, (const uchar*) end);

		if (true_word_char(ctype, *doc)) {
			break;
		}

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	ulint	mwc = 0;
	ulint	length = 0;

	token->f_str = const_cast<byte*>(doc);

	while (doc < end) {

		int	ctype;

		mbl = cs->cset->ctype(
			cs, &ctype, (uchar*) doc, (uchar*) end);
		if (true_word_char(ctype, *doc)) {
			mwc = 0;
		} else if (!misc_word_char(*doc) || mwc) {
			break;
		} else {
			++mwc;
		}

		++length;

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	token->f_len = (uint) (doc - token->f_str) - mwc;
	token->f_n_char = length;

	return(doc - start);
}

/**************************************************************//**
Converts a MySQL type to an InnoDB type. Note that this function returns
the 'mtype' of InnoDB. InnoDB differentiates between MySQL's old <= 4.1
VARCHAR and the new true VARCHAR in >= 5.0.3 by the 'prtype'.
@return	DATA_BINARY, DATA_VARCHAR, ... */
UNIV_INTERN
ulint
get_innobase_type_from_mysql_type(
/*==============================*/
	ulint*		unsigned_flag,	/*!< out: DATA_UNSIGNED if an
					'unsigned type';
					at least ENUM and SET,
					and unsigned integer
					types are 'unsigned types' */
	const void*	f)		/*!< in: MySQL Field */
{
	const class Field* field = reinterpret_cast<const class Field*>(f);

	/* The following asserts try to check that the MySQL type code fits in
	8 bits: this is used in ibuf and also when DATA_NOT_NULL is ORed to
	the type */

	DBUG_ASSERT((ulint)MYSQL_TYPE_STRING < 256);
	DBUG_ASSERT((ulint)MYSQL_TYPE_VAR_STRING < 256);
	DBUG_ASSERT((ulint)MYSQL_TYPE_DOUBLE < 256);
	DBUG_ASSERT((ulint)MYSQL_TYPE_FLOAT < 256);
	DBUG_ASSERT((ulint)MYSQL_TYPE_DECIMAL < 256);

	if (field->flags & UNSIGNED_FLAG) {

		*unsigned_flag = DATA_UNSIGNED;
	} else {
		*unsigned_flag = 0;
	}

	if (field->real_type() == MYSQL_TYPE_ENUM
		|| field->real_type() == MYSQL_TYPE_SET) {

		/* MySQL has field->type() a string type for these, but the
		data is actually internally stored as an unsigned integer
		code! */

		*unsigned_flag = DATA_UNSIGNED; /* MySQL has its own unsigned
						flag set to zero, even though
						internally this is an unsigned
						integer type */
		return(DATA_INT);
	}

	switch (field->type()) {
		/* NOTE that we only allow string types in DATA_MYSQL and
		DATA_VARMYSQL */
	case MYSQL_TYPE_VAR_STRING:	/* old <= 4.1 VARCHAR */
	case MYSQL_TYPE_VARCHAR:	/* new >= 5.0.3 true VARCHAR */
		if (field->binary()) {
			return(DATA_BINARY);
		} else if (strcmp(field->charset()->name,
				  "latin1_swedish_ci") == 0) {
			return(DATA_VARCHAR);
		} else {
			return(DATA_VARMYSQL);
		}
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING: if (field->binary()) {

			return(DATA_FIXBINARY);
		} else if (strcmp(field->charset()->name,
				  "latin1_swedish_ci") == 0) {
			return(DATA_CHAR);
		} else {
			return(DATA_MYSQL);
		}
	case MYSQL_TYPE_NEWDECIMAL:
		return(DATA_FIXBINARY);
	case MYSQL_TYPE_LONG:
	case MYSQL_TYPE_LONGLONG:
	case MYSQL_TYPE_TINY:
	case MYSQL_TYPE_SHORT:
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_DATE:
	case MYSQL_TYPE_YEAR:
	case MYSQL_TYPE_NEWDATE:
		return(DATA_INT);
	case MYSQL_TYPE_TIME:
	case MYSQL_TYPE_DATETIME:
	case MYSQL_TYPE_TIMESTAMP:
		switch (field->real_type()) {
		case MYSQL_TYPE_TIME:
		case MYSQL_TYPE_DATETIME:
		case MYSQL_TYPE_TIMESTAMP:
			return(DATA_INT);
		default: /* Fall through */
			DBUG_ASSERT((ulint)MYSQL_TYPE_DECIMAL < 256);
		case MYSQL_TYPE_TIME2:
		case MYSQL_TYPE_DATETIME2:
		case MYSQL_TYPE_TIMESTAMP2:
			return(DATA_FIXBINARY);
		}
	case MYSQL_TYPE_FLOAT:
		return(DATA_FLOAT);
	case MYSQL_TYPE_DOUBLE:
		return(DATA_DOUBLE);
	case MYSQL_TYPE_DECIMAL:
		return(DATA_DECIMAL);
	case MYSQL_TYPE_GEOMETRY:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
		return(DATA_BLOB);
	case MYSQL_TYPE_NULL:
		/* MySQL currently accepts "NULL" datatype, but will
		reject such datatype in the next release. We will cope
		with it and not trigger assertion failure in 5.1 */
		break;
	default:
		ut_error;
	}

	return(0);
}

/*******************************************************************//**
Writes an unsigned integer value < 64k to 2 bytes, in the little-endian
storage format. */
static inline
void
innobase_write_to_2_little_endian(
/*==============================*/
	byte*	buf,	/*!< in: where to store */
	ulint	val)	/*!< in: value to write, must be < 64k */
{
	ut_a(val < 256 * 256);

	buf[0] = (byte)(val & 0xFF);
	buf[1] = (byte)(val / 256);
}

/*******************************************************************//**
Reads an unsigned integer value < 64k from 2 bytes, in the little-endian
storage format.
@return	value */
static inline
uint
innobase_read_from_2_little_endian(
/*===============================*/
	const uchar*	buf)	/*!< in: from where to read */
{
	return((uint) ((ulint)(buf[0]) + 256 * ((ulint)(buf[1]))));
}

/*******************************************************************//**
Stores a key value for a row to a buffer.
@return	key value length as stored in buff */
#ifdef WITH_WSREP
UNIV_INTERN
uint
wsrep_store_key_val_for_row(
/*========================*/
	THD* 		thd,
	TABLE*		table,
	uint		keynr,		/*!< in: key number */
	char*		buff,		/*!< in/out: buffer for the key value
					in MySQL format) */
	uint		buff_len,	/*!< in: buffer length */
	const uchar*	record,
	ibool*          key_is_null,	/*!< out: full key was null */
	row_prebuilt_t*	prebuilt)	/*!< prebuilt struct in InnoDB */
{
	KEY*		key_info	= table->key_info + keynr;
	KEY_PART_INFO*	key_part	= key_info->key_part;
	KEY_PART_INFO*	end		=
		key_part + key_info->user_defined_key_parts;
	char*		buff_start	= buff;
	enum_field_types mysql_type;
	Field*		field;
	uint buff_space = buff_len;

	DBUG_ENTER("wsrep_store_key_val_for_row");

	memset(buff, 0, buff_len);
	*key_is_null = TRUE;

	for (; key_part != end; key_part++) {

		uchar sorted[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		ibool part_is_null = FALSE;

		if (key_part->null_bit) {
			if (buff_space > 0) {
				if (record[key_part->null_offset]
				    & key_part->null_bit) {
					*buff = 1;
					part_is_null = TRUE;
				} else {
					*buff = 0;
				}
				buff++;
				buff_space--;
			} else {
				fprintf (stderr, "WSREP: key truncated: %s\n",
					 wsrep_thd_query(thd));
			}
		}
		if (!part_is_null)  *key_is_null = FALSE;
		
		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_VARCHAR) {
						/* >= 5.0.3 true VARCHAR */
			ulint		lenlen;
			ulint		len;
			const byte*	data;
			ulint		key_len;
			ulint		true_len;
			const CHARSET_INFO* cs;
			int		error=0;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
				continue;
			}
			cs = field->charset();

			lenlen = (ulint)
				(((Field_varstring*)field)->length_bytes);

			data = row_mysql_read_true_varchar(&len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
				lenlen);

			true_len = len;

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) cs->cset->well_formed_len(cs,
						(const char *) data,
						(const char *) data + len,
                                                (uint) (key_len / cs->mbmaxlen),
						&error);
			}

			/* In a column prefix index, we may need to truncate
			the stored value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			memcpy(sorted, data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len, 
				REC_VERSION_56_MAX_INDEX_COL_LEN);

			if (wsrep_protocol_version > 1) {
			/* Note that we always reserve the maximum possible
			   length of the true VARCHAR in the key value, though
			   only len first bytes after the 2 length bytes contain
			   actual data. The rest of the space was reset to zero
			   in the bzero() call above. */
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
 				memcpy(buff, sorted, true_len);
                                buff       += true_len;
				buff_space -= true_len;
                        } else {
                                buff += key_len;
                        }
		} else if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB
			/* MYSQL_TYPE_GEOMETRY data is treated
			as BLOB data in innodb. */
			|| mysql_type == MYSQL_TYPE_GEOMETRY) {

			const CHARSET_INFO* cs;
			ulint		key_len;
			ulint		true_len;
			int		error=0;
			ulint		blob_len;
			const byte*	blob_data;

			ut_a(key_part->key_part_flag & HA_PART_KEY_SEG);

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			cs = field->charset();

			blob_data = row_mysql_read_blob_ref(&blob_len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
					(ulint) field->pack_length(),
				field->column_format() ==
					COLUMN_FORMAT_TYPE_COMPRESSED,
				reinterpret_cast<const byte*>(
					field->zip_dict_data.str),
				field->zip_dict_data.length, prebuilt);

			true_len = blob_len;

			ut_a(get_field_offset(table, field)
				== key_part->offset);

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (blob_len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) cs->cset->well_formed_len(cs,
						(const char *) blob_data,
						(const char *) blob_data
							+ blob_len,
                                                (uint) (key_len / cs->mbmaxlen),
						&error);
			}

			/* All indexes on BLOB and TEXT are column prefix
			indexes, and we may need to truncate the data to be
			stored in the key value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			memcpy(sorted, blob_data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len,
				REC_VERSION_56_MAX_INDEX_COL_LEN);


			/* Note that we always reserve the maximum possible
			length of the BLOB prefix in the key value. */
                        if (wsrep_protocol_version > 1) {
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
			} else {
				buff += key_len;
			}
			memcpy(buff, sorted, true_len);
		} else {
			/* Here we handle all other data types except the
			true VARCHAR, BLOB and TEXT. Note that the column
			value we store may be also in a column prefix
			index. */

			const CHARSET_INFO*	cs = NULL;
			ulint			true_len;
			ulint			key_len;
			const uchar*		src_start;
			int			error=0;
			enum_field_types	real_type;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			src_start = record + key_part->offset;
			real_type = field->real_type();
			true_len = key_len;

			/* Character set for the field is defined only
			to fields whose type is string and real field
			type is not enum or set. For these fields check
			if character set is multi byte. */

			if (real_type != MYSQL_TYPE_ENUM
				&& real_type != MYSQL_TYPE_SET
				&& ( mysql_type == MYSQL_TYPE_VAR_STRING
					|| mysql_type == MYSQL_TYPE_STRING)) {

				cs = field->charset();

				/* For multi byte character sets we need to
				calculate the true length of the key */

				if (key_len > 0 && cs->mbmaxlen > 1) {

					true_len = (ulint)
						cs->cset->well_formed_len(cs,
							(const char *)src_start,
							(const char *)src_start
								+ key_len,
                                                        (uint) (key_len /
                                                                cs->mbmaxlen),
							&error);
				}
				memcpy(sorted, src_start, true_len);
				true_len = wsrep_innobase_mysql_sort(
					mysql_type, cs->number, sorted, true_len,
					REC_VERSION_56_MAX_INDEX_COL_LEN);

				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len   = buff_space;
				}
				memcpy(buff, sorted, true_len);
			} else {
				memcpy(buff, src_start, true_len);
			}
			buff       += true_len;
			buff_space -= true_len;
		}
	}

	ut_a(buff <= buff_start + buff_len);

	DBUG_RETURN((uint)(buff - buff_start));
}
#endif /* WITH_WSREP */
UNIV_INTERN
uint
ha_innobase::store_key_val_for_row(
/*===============================*/
	uint		keynr,	/*!< in: key number */
	char*		buff,	/*!< in/out: buffer for the key value (in MySQL
				format) */
	uint		buff_len,/*!< in: buffer length */
	const uchar*	record)/*!< in: row in MySQL format */
{
	KEY*		key_info	= table->key_info + keynr;
	KEY_PART_INFO*	key_part	= key_info->key_part;
	KEY_PART_INFO*	end		=
		key_part + key_info->user_defined_key_parts;
	char*		buff_start	= buff;
	enum_field_types mysql_type;
	Field*		field;
	ibool		is_null;

	DBUG_ENTER("store_key_val_for_row");

	/* The format for storing a key field in MySQL is the following:

	1. If the column can be NULL, then in the first byte we put 1 if the
	field value is NULL, 0 otherwise.

	2. If the column is of a BLOB type (it must be a column prefix field
	in this case), then we put the length of the data in the field to the
	next 2 bytes, in the little-endian format. If the field is SQL NULL,
	then these 2 bytes are set to 0. Note that the length of data in the
	field is <= column prefix length.

	3. In a column prefix field, prefix_len next bytes are reserved for
	data. In a normal field the max field length next bytes are reserved
	for data. For a VARCHAR(n) the max field length is n. If the stored
	value is the SQL NULL then these data bytes are set to 0.

	4. We always use a 2 byte length for a true >= 5.0.3 VARCHAR. Note that
	in the MySQL row format, the length is stored in 1 or 2 bytes,
	depending on the maximum allowed length. But in the MySQL key value
	format, the length always takes 2 bytes.

	We have to zero-fill the buffer so that MySQL is able to use a
	simple memcmp to compare two key values to determine if they are
	equal. MySQL does this to compare contents of two 'ref' values. */

	memset(buff, 0, buff_len);

	for (; key_part != end; key_part++) {
		is_null = FALSE;

		if (key_part->null_bit) {
			if (record[key_part->null_offset]
						& key_part->null_bit) {
				*buff = 1;
				is_null = TRUE;
			} else {
				*buff = 0;
			}
			buff++;
		}

		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_VARCHAR) {
						/* >= 5.0.3 true VARCHAR */
			ulint		lenlen;
			ulint		len;
			const byte*	data;
			ulint		key_len;
			ulint		true_len;
			const CHARSET_INFO* cs;
			int		error=0;

			key_len = key_part->length;

			if (is_null) {
				buff += key_len + 2;

				continue;
			}
			cs = field->charset();

			lenlen = (ulint)
				(((Field_varstring*) field)->length_bytes);

			data = row_mysql_read_true_varchar(&len,
				(byte*) (record
				+ (ulint) get_field_offset(table, field)),
				lenlen);

			true_len = len;

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) cs->cset->well_formed_len(cs,
						(const char*) data,
						(const char*) data + len,
						(uint) (key_len / cs->mbmaxlen),
						&error);
			}

			/* In a column prefix index, we may need to truncate
			the stored value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			/* The length in a key value is always stored in 2
			bytes */

			row_mysql_store_true_var_len((byte*) buff, true_len, 2);
			buff += 2;

			memcpy(buff, data, true_len);

			/* Note that we always reserve the maximum possible
			length of the true VARCHAR in the key value, though
			only len first bytes after the 2 length bytes contain
			actual data. The rest of the space was reset to zero
			in the memset() call above. */

			buff += key_len;

		} else if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB
			/* MYSQL_TYPE_GEOMETRY data is treated
			as BLOB data in innodb. */
			|| mysql_type == MYSQL_TYPE_GEOMETRY) {

			const CHARSET_INFO* cs;
			ulint		key_len;
			ulint		true_len;
			int		error=0;
			ulint		blob_len;
			const byte*	blob_data;

			ut_a(key_part->key_part_flag & HA_PART_KEY_SEG);

			key_len = key_part->length;

			if (is_null) {
				buff += key_len + 2;

				continue;
			}

			cs = field->charset();

			blob_data = row_mysql_read_blob_ref(&blob_len,
				(byte*) (record
				+ (ulint) get_field_offset(table, field)),
				(ulint) field->pack_length(),
				field->column_format() ==
					COLUMN_FORMAT_TYPE_COMPRESSED,
				reinterpret_cast<const byte*>(
					field->zip_dict_data.str),
				field->zip_dict_data.length, prebuilt);

			true_len = blob_len;

			ut_a(get_field_offset(table, field)
				== key_part->offset);

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (blob_len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) cs->cset->well_formed_len(cs,
						(const char*) blob_data,
						(const char*) blob_data
							+ blob_len,
						(uint) (key_len / cs->mbmaxlen),
						&error);
			}

			/* All indexes on BLOB and TEXT are column prefix
			indexes, and we may need to truncate the data to be
			stored in the key value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			/* MySQL reserves 2 bytes for the length and the
			storage of the number is little-endian */

			innobase_write_to_2_little_endian(
					(byte*) buff, true_len);
			buff += 2;

			memcpy(buff, blob_data, true_len);

			/* Note that we always reserve the maximum possible
			length of the BLOB prefix in the key value. */

			buff += key_len;
		} else {
			/* Here we handle all other data types except the
			true VARCHAR, BLOB and TEXT. Note that the column
			value we store may be also in a column prefix
			index. */

			const CHARSET_INFO*	cs = NULL;
			ulint			true_len;
			ulint			key_len;
			const uchar*		src_start;
			int			error=0;
			enum_field_types	real_type;

			key_len = key_part->length;

			if (is_null) {
				 buff += key_len;

				 continue;
			}

			src_start = record + key_part->offset;
			real_type = field->real_type();
			true_len = key_len;

			/* Character set for the field is defined only
			to fields whose type is string and real field
			type is not enum or set. For these fields check
			if character set is multi byte. */

			if (real_type != MYSQL_TYPE_ENUM
				&& real_type != MYSQL_TYPE_SET
				&& ( mysql_type == MYSQL_TYPE_VAR_STRING
					|| mysql_type == MYSQL_TYPE_STRING)) {

				cs = field->charset();

				/* For multi byte character sets we need to
				calculate the true length of the key */

				if (key_len > 0 && cs->mbmaxlen > 1) {

					true_len = (ulint)
						cs->cset->well_formed_len(cs,
							(const char*) src_start,
							(const char*) src_start
								+ key_len,
							(uint) (key_len
								/ cs->mbmaxlen),
							&error);
				}
			}

			memcpy(buff, src_start, true_len);
			buff += true_len;

			/* Pad the unused space with spaces. */

			if (true_len < key_len) {
				ulint	pad_len = key_len - true_len;
				ut_a(cs != NULL);
				ut_a(!(pad_len % cs->mbminlen));

				cs->cset->fill(cs, buff, pad_len,
					       0x20 /* space */);
				buff += pad_len;
			}
		}
	}

	ut_a(buff <= buff_start + buff_len);

	DBUG_RETURN((uint)(buff - buff_start));
}

/**************************************************************//**
Determines if a field is needed in a prebuilt struct 'template'.
@return field to use, or NULL if the field is not needed */
static
const Field*
build_template_needs_field(
/*=======================*/
	ibool		index_contains,	/*!< in:
					dict_index_contains_col_or_prefix(
					index, i) */
	ibool		read_just_key,	/*!< in: TRUE when MySQL calls
					ha_innobase::extra with the
					argument HA_EXTRA_KEYREAD; it is enough
					to read just columns defined in
					the index (i.e., no read of the
					clustered index record necessary) */
	ibool		fetch_all_in_key,
					/*!< in: true=fetch all fields in
					the index */
	ibool		fetch_primary_key_cols,
					/*!< in: true=fetch the
					primary key columns */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	const TABLE*	table,		/*!< in: MySQL table object */
	ulint		i)		/*!< in: field index in InnoDB table */
{
	const Field*	field	= table->field[i];

	ut_ad(index_contains == dict_index_contains_col_or_prefix(index, i));

	if (!index_contains) {
		if (read_just_key) {
			/* If this is a 'key read', we do not need
			columns that are not in the key */

			return(NULL);
		}
	} else if (fetch_all_in_key) {
		/* This field is needed in the query */

		return(field);
	}

	if (bitmap_is_set(table->read_set, static_cast<uint>(i))
	    || bitmap_is_set(table->write_set, static_cast<uint>(i))) {
		/* This field is needed in the query */

		return(field);
	}

	if (fetch_primary_key_cols
	    && dict_table_col_in_clustered_key(index->table, i)) {
		/* This field is needed in the query */

		return(field);
	}

	/* This field is not needed in the query, skip it */

	return(NULL);
}

/**************************************************************//**
Determines if a field is needed in a prebuilt struct 'template'.
@return whether the field is needed for index condition pushdown */
inline
bool
build_template_needs_field_in_icp(
/*==============================*/
	const dict_index_t*	index,	/*!< in: InnoDB index */
	const row_prebuilt_t*	prebuilt,/*!< in: row fetch template */
	bool			contains,/*!< in: whether the index contains
					column i */
	ulint			i)	/*!< in: column number */
{
	ut_ad(contains == dict_index_contains_col_or_prefix(index, i));

	return(index == prebuilt->index
	       ? contains
	       : dict_index_contains_col_or_prefix(prebuilt->index, i));
}

/**************************************************************//**
Adds a field to a prebuilt struct 'template'.
@return the field template */
static
mysql_row_templ_t*
build_template_field(
/*=================*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: template */
	dict_index_t*	clust_index,	/*!< in: InnoDB clustered index */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	TABLE*		table,		/*!< in: MySQL table object */
	const Field*	field,		/*!< in: field in MySQL table */
	ulint		i)		/*!< in: field index in InnoDB table */
{
	mysql_row_templ_t*	templ;
	const dict_col_t*	col;

	ut_ad(field == table->field[i]);
	ut_ad(clust_index->table == index->table);

	col = dict_table_get_nth_col(index->table, i);

	templ = prebuilt->mysql_template + prebuilt->n_template++;
	UNIV_MEM_INVALID(templ, sizeof *templ);
	templ->col_no = i;
	templ->clust_rec_field_no = dict_col_get_clust_pos(col, clust_index);
	ut_a(templ->clust_rec_field_no != ULINT_UNDEFINED);

	if (dict_index_is_clust(index)) {
		templ->rec_field_is_prefix = false;
		templ->rec_field_no = templ->clust_rec_field_no;
		templ->rec_prefix_field_no = ULINT_UNDEFINED;
	} else {
		/* If we're in a secondary index, keep track of the original
		index position even if this is just a prefix index; we will use
		this later to avoid a cluster index lookup in some cases.*/

		templ->rec_field_no = dict_index_get_nth_col_pos(index, i,
						&templ->rec_prefix_field_no);
		templ->rec_field_is_prefix
			= (templ->rec_field_no == ULINT_UNDEFINED)
			  && (templ->rec_prefix_field_no != ULINT_UNDEFINED);
#ifdef UNIV_DEBUG
		if (templ->rec_prefix_field_no != ULINT_UNDEFINED)
		{
			const dict_field_t* field = dict_index_get_nth_field(
				index,
				templ->rec_prefix_field_no);
			ut_ad(templ->rec_field_is_prefix
			      == (field->prefix_len != 0));
		} else {
			ut_ad(!templ->rec_field_is_prefix);
		}
#endif
	}

	if (field->real_maybe_null()) {
		templ->mysql_null_byte_offset =
			field->null_offset();

		templ->mysql_null_bit_mask = (ulint) field->null_bit;
	} else {
		templ->mysql_null_bit_mask = 0;
	}

	templ->mysql_col_offset = (ulint) get_field_offset(table, field);

	templ->mysql_col_len = (ulint) field->pack_length();
	templ->type = col->mtype;
	templ->mysql_type = (ulint) field->type();

	if (templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
		templ->mysql_length_bytes = (ulint)
			(((Field_varstring*) field)->length_bytes);
	}

	templ->charset = dtype_get_charset_coll(col->prtype);
	templ->mbminlen = dict_col_get_mbminlen(col);
	templ->mbmaxlen = dict_col_get_mbmaxlen(col);
	templ->is_unsigned = col->prtype & DATA_UNSIGNED;
	templ->compressed = (field->column_format()
				== COLUMN_FORMAT_TYPE_COMPRESSED);
	templ->zip_dict_data = field->zip_dict_data;

	if (!dict_index_is_clust(index)
	    && templ->rec_field_no == ULINT_UNDEFINED) {
		prebuilt->need_to_access_clustered = TRUE;
	}

	if (prebuilt->mysql_prefix_len < templ->mysql_col_offset
	    + templ->mysql_col_len) {
		prebuilt->mysql_prefix_len = templ->mysql_col_offset
			+ templ->mysql_col_len;
	}

	if (templ->type == DATA_BLOB) {
		prebuilt->templ_contains_blob = TRUE;
	}

	return(templ);
}

/**************************************************************//**
Builds a 'template' to the prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */
UNIV_INTERN
void
ha_innobase::build_template(
/*========================*/
	bool		whole_row)	/*!< in: true=ROW_MYSQL_WHOLE_ROW,
					false=ROW_MYSQL_REC_FIELDS */
{
	dict_index_t*	index;
	dict_index_t*	clust_index;
	ulint		n_fields;
	ibool		fetch_all_in_key	= FALSE;
	ibool		fetch_primary_key_cols	= FALSE;
	ulint		i;

	if (prebuilt->select_lock_type == LOCK_X) {
		/* We always retrieve the whole clustered index record if we
		use exclusive row level locks, for example, if the read is
		done in an UPDATE statement. */

		whole_row = true;
	} else if (!whole_row) {
		if (prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_ALL_COLS) {

			/* We know we must at least fetch all columns in the
			key, or all columns in the table */

			if (prebuilt->read_just_key) {
				/* MySQL has instructed us that it is enough
				to fetch the columns in the key; looks like
				MySQL can set this flag also when there is
				only a prefix of the column in the key: in
				that case we retrieve the whole column from
				the clustered index */

				fetch_all_in_key = TRUE;
			} else {
				whole_row = true;
			}
		} else if (prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_PRIMARY_KEY) {
			/* We must at least fetch all primary key cols. Note
			that if the clustered index was internally generated
			by InnoDB on the row id (no primary key was
			defined), then row_search_for_mysql() will always
			retrieve the row id to a special buffer in the
			prebuilt struct. */

			fetch_primary_key_cols = TRUE;
		}
	}

	clust_index = dict_table_get_first_index(prebuilt->table);

	index = whole_row ? clust_index : prebuilt->index;

	prebuilt->need_to_access_clustered = (index == clust_index);

	/* Either prebuilt->index should be a secondary index, or it
	should be the clustered index. */
	ut_ad(dict_index_is_clust(index) == (index == clust_index));

	/* Below we check column by column if we need to access
	the clustered index. */

	n_fields = (ulint) table->s->fields; /* number of columns */

	if (!prebuilt->mysql_template) {
		prebuilt->mysql_template = (mysql_row_templ_t*)
			mem_alloc(n_fields * sizeof(mysql_row_templ_t));
	}

	prebuilt->template_type = whole_row
		? ROW_MYSQL_WHOLE_ROW : ROW_MYSQL_REC_FIELDS;
	prebuilt->null_bitmap_len = table->s->null_bytes;

	/* Prepare to build prebuilt->mysql_template[]. */
	prebuilt->templ_contains_blob = FALSE;
	prebuilt->mysql_prefix_len = 0;
	prebuilt->n_template = 0;
	prebuilt->idx_cond_n_cols = 0;

	/* Note that in InnoDB, i is the column number in the table.
	MySQL calls columns 'fields'. */

	if (active_index != MAX_KEY && active_index == pushed_idx_cond_keyno) {
		/* Push down an index condition or an end_range check. */
		for (i = 0; i < n_fields; i++) {
			const ibool		index_contains
				= dict_index_contains_col_or_prefix(index, i);

			/* Test if an end_range or an index condition
			refers to the field. Note that "index" and
			"index_contains" may refer to the clustered index.
			Index condition pushdown is relative to prebuilt->index
			(the index that is being looked up first). */

			/* When join_read_always_key() invokes this
			code via handler::ha_index_init() and
			ha_innobase::index_init(), end_range is not
			yet initialized. Because of that, we must
			always check for index_contains, instead of
			the subset
			field->part_of_key.is_set(active_index)
			which would be acceptable if end_range==NULL. */
			if (build_template_needs_field_in_icp(
				    index, prebuilt, index_contains, i)) {
				/* Needed in ICP */
				const Field*		field;
				mysql_row_templ_t*	templ;

				if (whole_row) {
					field = table->field[i];
				} else {
					field = build_template_needs_field(
						index_contains,
						prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i);
					if (!field) {
						continue;
					}
				}

				templ = build_template_field(
					prebuilt, clust_index, index,
					table, field, i);
				prebuilt->idx_cond_n_cols++;
				ut_ad(prebuilt->idx_cond_n_cols
				      == prebuilt->n_template);

				if (index == prebuilt->index) {
					templ->icp_rec_field_no
						= templ->rec_field_no;
				} else {
					templ->icp_rec_field_no
						= dict_index_get_nth_col_pos(
							prebuilt->index, i,
							NULL);
				}

				if (dict_index_is_clust(prebuilt->index)) {
					ut_ad(templ->icp_rec_field_no
					      != ULINT_UNDEFINED);
					/* If the primary key includes
					a column prefix, use it in
					index condition pushdown,
					because the condition is
					evaluated before fetching any
					off-page (externally stored)
					columns. */
					if (templ->icp_rec_field_no
					    < prebuilt->index->n_uniq) {
						/* This is a key column;
						all set. */
						continue;
					}
				} else if (templ->icp_rec_field_no
					   != ULINT_UNDEFINED) {
					continue;
				}

				/* This is a column prefix index.
				The column prefix can be used in
				an end_range comparison. */

				templ->icp_rec_field_no
					= dict_index_get_nth_col_or_prefix_pos(
						prebuilt->index, i, TRUE, NULL);
				ut_ad(templ->icp_rec_field_no
				      != ULINT_UNDEFINED);

				/* Index condition pushdown can be used on
				all columns of a secondary index, and on
				the PRIMARY KEY columns. On the clustered
				index, it must never be used on other than
				PRIMARY KEY columns, because those columns
				may be stored off-page, and we will not
				fetch externally stored columns before
				checking the index condition. */
				/* TODO: test the above with an assertion
				like this. Note that index conditions are
				currently pushed down as part of the
				"optimizer phase" while end_range is done
				as part of the execution phase. Therefore,
				we were unable to use an accurate condition
				for end_range in the "if" condition above,
				and the following assertion would fail.
				ut_ad(!dict_index_is_clust(prebuilt->index)
				      || templ->rec_field_no
				      < prebuilt->index->n_uniq);
				*/
			}
		}

		ut_ad(prebuilt->idx_cond_n_cols > 0);
		ut_ad(prebuilt->idx_cond_n_cols == prebuilt->n_template);

		/* Include the fields that are not needed in index condition
		pushdown. */
		for (i = 0; i < n_fields; i++) {
			const ibool		index_contains
				= dict_index_contains_col_or_prefix(index, i);

			if (!build_template_needs_field_in_icp(
				    index, prebuilt, index_contains, i)) {
				/* Not needed in ICP */
				const Field*	field;

				if (whole_row) {
					field = table->field[i];
				} else {
					field = build_template_needs_field(
						index_contains,
						prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i);
					if (!field) {
						continue;
					}
				}

				build_template_field(prebuilt,
						     clust_index, index,
						     table, field, i);
			}
		}

		prebuilt->idx_cond = this;
	} else {
		/* No index condition pushdown */
		prebuilt->idx_cond = NULL;

		for (i = 0; i < n_fields; i++) {
			const Field*	field;

			if (whole_row) {
				field = table->field[i];
			} else {
				field = build_template_needs_field(
					dict_index_contains_col_or_prefix(
						index, i),
					prebuilt->read_just_key,
					fetch_all_in_key,
					fetch_primary_key_cols,
					index, table, i);
				if (!field) {
					continue;
				}
			}

			build_template_field(prebuilt, clust_index, index,
					     table, field, i);
		}
	}

	if (index != clust_index && prebuilt->need_to_access_clustered) {
		/* Change rec_field_no's to correspond to the clustered index
		record */
		for (i = 0; i < prebuilt->n_template; i++) {

			mysql_row_templ_t*	templ
				= &prebuilt->mysql_template[i];

			templ->rec_field_no = templ->clust_rec_field_no;
		}
	}
}

/********************************************************************//**
This special handling is really to overcome the limitations of MySQL's
binlogging. We need to eliminate the non-determinism that will arise in
INSERT ... SELECT type of statements, since MySQL binlog only stores the
min value of the autoinc interval. Once that is fixed we can get rid of
the special lock handling.
@return	DB_SUCCESS if all OK else error code */
UNIV_INTERN
dberr_t
ha_innobase::innobase_lock_autoinc(void)
/*====================================*/
{
	DBUG_ENTER("ha_innobase::innobase_lock_autoinc");
	dberr_t		error = DB_SUCCESS;

	ut_ad(!srv_read_only_mode);

	switch (innobase_autoinc_lock_mode) {
	case AUTOINC_NO_LOCKING:
		/* Acquire only the AUTOINC mutex. */
		dict_table_autoinc_lock(prebuilt->table);
		break;

	case AUTOINC_NEW_STYLE_LOCKING:
		/* For simple (single/multi) row INSERTs, we fallback to the
		old style only if another transaction has already acquired
		the AUTOINC lock on behalf of a LOAD FILE or INSERT ... SELECT
		etc. type of statement. */
		if (thd_sql_command(user_thd) == SQLCOM_INSERT
		    || thd_sql_command(user_thd) == SQLCOM_REPLACE) {
			dict_table_t*	ib_table = prebuilt->table;

			/* Acquire the AUTOINC mutex. */
			dict_table_autoinc_lock(ib_table);

			/* We need to check that another transaction isn't
			already holding the AUTOINC lock on the table. */
			if (ib_table->n_waiting_or_granted_auto_inc_locks) {
				/* Release the mutex to avoid deadlocks. */
				dict_table_autoinc_unlock(ib_table);
			} else {
				break;
			}
		}
		// fallthrough
		// to old style locking.

	case AUTOINC_OLD_STYLE_LOCKING:
		DBUG_EXECUTE_IF("die_if_autoinc_old_lock_style_used",
				ut_ad(0););
		error = row_lock_table_autoinc_for_mysql(prebuilt);

		if (error == DB_SUCCESS) {

			/* Acquire the AUTOINC mutex. */
			dict_table_autoinc_lock(prebuilt->table);
		}
		break;

	default:
		ut_error;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Reset the autoinc value in the table.
@return	DB_SUCCESS if all went well else error code */
UNIV_INTERN
dberr_t
ha_innobase::innobase_reset_autoinc(
/*================================*/
	ulonglong	autoinc)	/*!< in: value to store */
{
	dberr_t		error;

	error = innobase_lock_autoinc();

	if (error == DB_SUCCESS) {

		dict_table_autoinc_initialize(prebuilt->table, autoinc);

		dict_table_autoinc_unlock(prebuilt->table);
	}

	return(error);
}

/********************************************************************//**
Store the autoinc value in the table. The autoinc value is only set if
it's greater than the existing autoinc value in the table.
@return	DB_SUCCESS if all went well else error code */
UNIV_INTERN
dberr_t
ha_innobase::innobase_set_max_autoinc(
/*==================================*/
	ulonglong	auto_inc)	/*!< in: value to store */
{
	dberr_t		error;

	error = innobase_lock_autoinc();

	if (error == DB_SUCCESS) {

		dict_table_autoinc_update_if_greater(prebuilt->table, auto_inc);

		dict_table_autoinc_unlock(prebuilt->table);
	}

	return(error);
}

/********************************************************************//**
Stores a row in an InnoDB database, to the table specified in this
handle.
@return	error code */
UNIV_INTERN
int
ha_innobase::write_row(
/*===================*/
	uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
	int		error_result= 0;
	ibool		auto_inc_used= FALSE;
#ifdef WITH_WSREP
	ibool           auto_inc_inserted= FALSE; /* if NULL was inserted */
#endif
	ulint		sql_command;
	trx_t*		trx = thd_to_trx(user_thd);

	DBUG_ENTER("ha_innobase::write_row");

	DEBUG_SYNC(user_thd, "ha_innobase_write_row");

	if (high_level_read_only) {
		ib_senderrf(ha_thd(), IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (prebuilt->trx != trx) {
		sql_print_error("The transaction object for the table handle "
				"is at %p, but for the current thread it is at "
				"%p",
				(const void*) prebuilt->trx, (const void*) trx);

		fputs("InnoDB: Dump of 200 bytes around prebuilt: ", stderr);
		ut_print_buf(stderr, ((const byte*) prebuilt) - 100, 200);
		fputs("\n"
			"InnoDB: Dump of 200 bytes around ha_data: ",
			stderr);
		ut_print_buf(stderr, ((const byte*) trx) - 100, 200);
		putc('\n', stderr);
		ut_error;
	} else if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	ha_statistic_increment(&SSV::ha_write_count);

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	sql_command = thd_sql_command(user_thd);

	if ((sql_command == SQLCOM_ALTER_TABLE
	     || sql_command == SQLCOM_OPTIMIZE
	     || sql_command == SQLCOM_CREATE_INDEX
	     || sql_command == SQLCOM_DROP_INDEX)
	    && num_write_row >= 10000) {
		/* ALTER TABLE is COMMITted at every 10000 copied rows.
		The IX table lock for the original table has to be re-issued.
		As this method will be called on a temporary table where the
		contents of the original table is being copied to, it is
		a bit tricky to determine the source table.  The cursor
		position in the source table need not be adjusted after the
		intermediate COMMIT, since writes by other transactions are
		being blocked by a MySQL table lock TL_WRITE_ALLOW_READ. */

		dict_table_t*	src_table;
		enum lock_mode	mode;

		num_write_row = 0;

		/* Commit the transaction.  This will release the table
		locks, so they have to be acquired again. */

		/* Altering an InnoDB table */
		/* Get the source table. */
		src_table = lock_get_src_table(
				prebuilt->trx, prebuilt->table, &mode);
		if (!src_table) {
no_commit:
			/* Unknown situation: do not commit */
			/*
			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: ALTER TABLE is holding lock"
				" on %lu tables!\n",
				prebuilt->trx->mysql_n_tables_locked);
			*/
			;
		} else if (src_table == prebuilt->table) {
			/* Source table is not in InnoDB format:
			no need to re-acquire locks on it. */

			/* Altering to InnoDB format */
			innobase_commit(ht, user_thd, 1);
			/* Note that this transaction is still active. */
			trx_register_for_2pc(prebuilt->trx);
			/* We will need an IX lock on the destination table. */
			prebuilt->sql_stat_start = TRUE;
		} else {
			/* Ensure that there are no other table locks than
			LOCK_IX and LOCK_AUTO_INC on the destination table. */

			if (!lock_is_table_exclusive(prebuilt->table,
							prebuilt->trx)) {
				goto no_commit;
			}

			/* Commit the transaction.  This will release the table
			locks, so they have to be acquired again. */
			innobase_commit(ht, user_thd, 1);
			/* Note that this transaction is still active. */
			trx_register_for_2pc(prebuilt->trx);
			/* Re-acquire the table lock on the source table. */
			row_lock_table_for_mysql(prebuilt, src_table, mode);
			/* We will need an IX lock on the destination table. */
			prebuilt->sql_stat_start = TRUE;
		}
	}

#ifdef WITH_WSREP
	/* Order of condition is important to avoid over-evaluation and save
	on performance. */
	if (num_write_row >= 10000
	    && !(thd_test_options(user_thd,
				  OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
	    && sql_command == SQLCOM_LOAD
	    && wsrep_load_data_splitting
	    && wsrep_on(user_thd)) {

		/* User is trying to load data using LOAD DATA statement
		with autocommit = 1 and load data split = 1.
		Logic will now commit after every 10K rows. */

		WSREP_DEBUG("Forced transaction split for LOAD DATA: %s",
			    wsrep_thd_query(user_thd));

		num_write_row = 0;

		switch (wsrep_run_wsrep_commit(user_thd, wsrep_hton, true))
		{
			case WSREP_TRX_OK:
				break;
			case WSREP_TRX_SIZE_EXCEEDED:
			case WSREP_TRX_CERT_FAIL:
			case WSREP_TRX_ERROR:
				DBUG_RETURN(1);
		}

		if (tc_log->commit(user_thd, true)) {
			DBUG_RETURN(1);
		}
		wsrep_post_commit(user_thd, true);

		/* Commit the transaction.  This will release the table
		locks, so they have to be acquired again. */
		innobase_commit(ht, user_thd, 1);

		/* Note that this transaction is still active. */
		trx_register_for_2pc(prebuilt->trx);

		/* We will need an IX lock on the destination table. */
		prebuilt->sql_stat_start = TRUE;
	}
#endif /* WITH_WSREP */

	num_write_row++;

	/* This is the case where the table has an auto-increment column */
	if (table->next_number_field && record == table->record[0]) {

		/* Reset the error code before calling
		innobase_get_auto_increment(). */
		prebuilt->autoinc_error = DB_SUCCESS;

#ifdef WITH_WSREP
		auto_inc_inserted= (table->next_number_field->val_int() == 0);
#endif
		if ((error_result = update_auto_increment())) {
			/* We don't want to mask autoinc overflow errors. */

			/* Handle the case where the AUTOINC sub-system
			failed during initialization. */
			if (prebuilt->autoinc_error == DB_UNSUPPORTED) {
				error_result = ER_AUTOINC_READ_FAILED;
				/* Set the error message to report too. */
				my_error(ER_AUTOINC_READ_FAILED, MYF(0));
				goto func_exit;
			} else if (prebuilt->autoinc_error != DB_SUCCESS) {
				error = prebuilt->autoinc_error;
				goto report_error;
			}

			/* MySQL errors are passed straight back. */
			goto func_exit;
		}

		auto_inc_used = TRUE;
	}

	if (prebuilt->mysql_template == NULL
	    || prebuilt->template_type != ROW_MYSQL_WHOLE_ROW) {

		/* Build the template used in converting quickly between
		the two database formats */

		build_template(true);
	}

	/* debug sync point has a special significance given the location
	where-in auto-inc value is generated but row insert action is not yet
	started. */
	DEBUG_SYNC(user_thd, "pxc_autoinc_val_generated");

	innobase_srv_conc_enter_innodb(prebuilt->trx);

	error = row_insert_for_mysql((byte*) record, prebuilt);
	DEBUG_SYNC(user_thd, "ib_after_row_insert");

	/* Handle duplicate key errors */
	if (auto_inc_used) {
		ulonglong	auto_inc;
		ulonglong	col_max_value;

		/* Note the number of rows processed for this statement, used
		by get_auto_increment() to determine the number of AUTO-INC
		values to reserve. This is only useful for a mult-value INSERT
		and is a statement level counter.*/
		if (trx->n_autoinc_rows > 0) {
			--trx->n_autoinc_rows;
		}

		/* We need the upper limit of the col type to check for
		whether we update the table autoinc counter or not. */
		col_max_value = innobase_get_int_col_max_value(
			table->next_number_field);

		/* Get the value that MySQL attempted to store in the table.*/
		auto_inc = table->next_number_field->val_int();

		switch (error) {
		case DB_DUPLICATE_KEY:

			/* A REPLACE command and LOAD DATA INFILE REPLACE
			handle a duplicate key error themselves, but we
			must update the autoinc counter if we are performing
			those statements. */

			switch (sql_command) {
			case SQLCOM_LOAD:
				if (trx->duplicates) {

					goto set_max_autoinc;
				}
				break;

			case SQLCOM_REPLACE:
			case SQLCOM_INSERT_SELECT:
			case SQLCOM_REPLACE_SELECT:
				goto set_max_autoinc;

#ifdef WITH_WSREP
			/* workaround for LP bug #355000, retrying the insert */
			case SQLCOM_INSERT:

				{
				const dict_index_t*     err_index=
					trx_get_error_info(prebuilt->trx);
				WSREP_DEBUG("Found duplicate value for"
					" table (%s) - key (%s)"
					" THD %lu, Auto-Inc-Value: %llu"
					" Offset: %llu, Increment: %llu",
					prebuilt->table->name,
					(err_index ? err_index->name : "UNKNOWN"),
					wsrep_thd_thread_id(current_thd),
					auto_inc,
					prebuilt->autoinc_offset,
					prebuilt->autoinc_increment);

				int mysql_key =
					get_dup_key(HA_ERR_FOUND_DUPP_KEY);
				KEY* key =
					(mysql_key < 0 || mysql_key == MAX_KEY)
					? NULL : &table->key_info[mysql_key];

				char key_buff[MAX_KEY_LENGTH];
				String str(key_buff, sizeof(key_buff),
					   system_charset_info);
				key_unpack(&str, table, key);
				WSREP_DEBUG("Duplicate value: %s", str.c_ptr());
				}

				if (wsrep_on(current_thd)                     &&
				    auto_inc_inserted                         &&
				    wsrep_drupal_282555_workaround            &&
				    wsrep_thd_retry_counter(current_thd) == 0 &&
				    !thd_test_options(current_thd, 
						      OPTION_NOT_AUTOCOMMIT | 
						      OPTION_BEGIN)) {
					WSREP_DEBUG(
					    "retrying insert: %s",
					    (*wsrep_thd_query(current_thd)) ? 
						wsrep_thd_query(current_thd) : 
						(char *)"void");
					error= DB_SUCCESS;
					wsrep_thd_set_conflict_state(
						current_thd, MUST_ABORT);
                                        innobase_srv_conc_exit_innodb(
						prebuilt->trx);
                                        /* jump straight to func exit over
                                         * later wsrep hooks */
                                        goto func_exit;
				}
                                break;
#endif
			default:
				break;
			}

			break;

		case DB_SUCCESS:
			/* If the actual value inserted is greater than
			the upper limit of the interval, then we try and
			update the table upper limit. Note: last_value
			will be 0 if get_auto_increment() was not called.*/

			if (auto_inc >= prebuilt->autoinc_last_value) {
set_max_autoinc:
				/* This should filter out the negative
				values set explicitly by the user. */
				if (auto_inc <= col_max_value) {
					ut_a(prebuilt->autoinc_increment > 0);

					ulonglong	offset;
					ulonglong	increment;
					dberr_t		err;

					offset = prebuilt->autoinc_offset;
					increment = prebuilt->autoinc_increment;

#ifdef WITH_WSREP
					ulonglong prev_auto_inc = auto_inc;
#endif /* WITH_WSREP */

					auto_inc = innobase_next_autoinc(
						auto_inc,
						1, increment, offset,
						col_max_value);

#ifdef WITH_WSREP
					WSREP_DEBUG("Generating new auto-inc"
						" next-value (Current: %llu,"
						" New: %llu)",
						prev_auto_inc, auto_inc);
#endif /* WITH_WSREP */


					err = innobase_set_max_autoinc(
						auto_inc);

					if (err != DB_SUCCESS) {
						error = err;
					}
				}
			}
			break;
		default:
			break;
		}
	}

	innobase_srv_conc_exit_innodb(prebuilt->trx);

report_error:
	if (error == DB_TABLESPACE_DELETED) {
		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);
	}

#ifdef WITH_WSREP
	if (trx_is_interrupted(trx)) {
		error = DB_INTERRUPTED;
	}
#endif /* WITH_WSREP */

	error_result = convert_error_code_to_mysql(error,
						   prebuilt->table->flags,
						   user_thd);
#ifdef WITH_WSREP
	/* Append key
	a. there is no error in insert
	b. exec mode is local
	   (it is workload executor node and not replicator node)
	c. wsrep is enabled
	d. No consistency check enforced.
	e. Ensure that bin-logging is enabled.
	   Either mysql bin-logging or emulated bin logging.
	f. If db_type = PARTITION and InnoDB parts are created then MySQL flow
	   will disable bin-logging before inserting data to indivdual parts.
	   This is to avoid double bin-logging. Generic level bin-logging is
	   done at ha_partition level. This causes LDI on partition table
	   to fail as thd_binlog_format() will then return != ROW.
	   In order to take care of this situation we ORed that condition
	   with db_type
        g. Added condition to avoid appending keys if statement is CTAS.
           key is appended as part of select_create::send_eof.
	TODO: We allow replication even if binlog-format = STATEMENT.
	This is needed by pt-table-checksum. Now it is not a good idea
	to open this hook for pt-table-checksum but it exist like this for
	while now so to maintain compatibility we continue to provide it.
	With that there comes another existing dependency.
	Why not allow LDI operating with binlog-format = STATEMENT.
	There is no reason documented so will leave it as is for now. */
	if (!error_result
	    && wsrep_thd_exec_mode(user_thd) == LOCAL_STATE
	    && wsrep_on(user_thd)
	    && !wsrep_consistency_check(user_thd)
	    && (thd_binlog_format(user_thd) == BINLOG_FORMAT_ROW
		|| (thd_binlog_format(user_thd) == BINLOG_FORMAT_STMT
		    && sql_command != SQLCOM_LOAD)
	        || table->file->ht->db_type == DB_TYPE_PARTITION_DB)) {

		if (wsrep_append_keys(user_thd, WSREP_KEY_EXCLUSIVE, record,
				      NULL)) {
 			DBUG_PRINT("wsrep", ("row key failed"));
 			error_result = HA_ERR_INTERNAL_ERROR;
			goto wsrep_error;
		}
	}
wsrep_error:
#endif

	if (error_result == HA_FTS_INVALID_DOCID) {
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	}

	if (error_result == HA_FTS_INVALID_DOCID) {
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	}

func_exit:
	innobase_active_small();

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	DBUG_RETURN(error_result);
}
#ifdef WITH_WSREP
#if defined(HAVE_YASSL)
#include "my_config.h"
#include "md5.hpp"
#elif defined(HAVE_OPENSSL)
#include <openssl/md5.h>
#endif
static
int
wsrep_calc_row_hash(
	byte*		digest,		/*!< in/out: md5 sum */
	const uchar*	row,		/*!< in: row in MySQL format */
	TABLE*		table,		/*!< in: table in MySQL data
					dictionary */
	row_prebuilt_t*	prebuilt,	/*!< in: InnoDB prebuilt struct */
	THD*		thd)		/*!< in: user thread */
{
	Field*		field;
	enum_field_types field_mysql_type;
	uint		n_fields;
	ulint		len;
	const byte*	ptr;
	ulint		col_type;
	uint		i;


	void *ctx = wsrep_md5_init();

	n_fields = table->s->fields;

	for (i = 0; i < n_fields; i++) {
		byte null_byte=0;
		byte true_byte=1;

		field = table->field[i];

		ptr = (const byte*) row + get_field_offset(table, field);
		len = field->pack_length();

		field_mysql_type = field->type();

		col_type = prebuilt->table->cols[i].mtype;

		switch (col_type) {

		case DATA_BLOB:
                        ptr = row_mysql_read_blob_ref(&len, ptr, len,
				false, 0, 0, prebuilt);
			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field_mysql_type == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				ptr = row_mysql_read_true_varchar(
					&len, ptr,
					(ulint)
					(((Field_varstring*)field)->length_bytes));

			}

			break;
		default:
			;
		}

		if (field->is_null_in_record(row)) {
			wsrep_md5_update(ctx, (char*)&null_byte, 1);
		} else {
			wsrep_md5_update(ctx, (char*)&true_byte, 1);
			wsrep_md5_update(ctx, (char*)ptr, len);
		}
	}
	wsrep_compute_md5_hash((char*)digest, ctx);
	return(0);
}
#endif /* WITH_WSREP */
/**********************************************************************//**
Checks which fields have changed in a row and stores information
of them to an update vector.
@return	DB_SUCCESS or error code */
static
dberr_t
calc_row_difference(
/*================*/
	upd_t*		uvect,		/*!< in/out: update vector */
	uchar*		old_row,	/*!< in: old row in MySQL format */
	uchar*		new_row,	/*!< in: new row in MySQL format */
	TABLE*		table,		/*!< in: table in MySQL data
					dictionary */
	uchar*		upd_buff,	/*!< in: buffer to use */
	ulint		buff_len,	/*!< in: buffer length */
	row_prebuilt_t*	prebuilt,	/*!< in: InnoDB prebuilt struct */
	THD*		thd)		/*!< in: user thread */
{
	uchar*		original_upd_buff = upd_buff;
	Field*		field;
	enum_field_types field_mysql_type;
	uint		n_fields;
	ulint		o_len;
	ulint		n_len;
	ulint		col_pack_len;
	const byte*	new_mysql_row_col;
	const byte*	o_ptr;
	const byte*	n_ptr;
	byte*		buf;
	upd_field_t*	ufield;
	ulint		col_type;
	ulint		n_changed = 0;
	dfield_t	dfield;
	dict_index_t*	clust_index;
	uint		i;
	ibool		changes_fts_column = FALSE;
	ibool		changes_fts_doc_col = FALSE;
	trx_t*          trx = thd_to_trx(thd);
	doc_id_t	doc_id = FTS_NULL_DOC_ID;

	ut_ad(!srv_read_only_mode);

	n_fields = table->s->fields;
	clust_index = dict_table_get_first_index(prebuilt->table);

	/* We use upd_buff to convert changed fields */
	buf = (byte*) upd_buff;

	for (i = 0; i < n_fields; i++) {
		field = table->field[i];

		o_ptr = (const byte*) old_row + get_field_offset(table, field);
		n_ptr = (const byte*) new_row + get_field_offset(table, field);

		/* Use new_mysql_row_col and col_pack_len save the values */

		new_mysql_row_col = n_ptr;
		col_pack_len = field->pack_length();

		o_len = col_pack_len;
		n_len = col_pack_len;

		/* We use o_ptr and n_ptr to dig up the actual data for
		comparison. */

		field_mysql_type = field->type();

		col_type = prebuilt->table->cols[i].mtype;

		switch (col_type) {

		case DATA_BLOB:
			/* Do not compress blob column while comparing*/
			o_ptr = row_mysql_read_blob_ref(&o_len, o_ptr, o_len,
				false, 0, 0, prebuilt);
			n_ptr = row_mysql_read_blob_ref(&n_len, n_ptr, n_len,
				false, 0, 0, prebuilt);

			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field_mysql_type == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				o_ptr = row_mysql_read_true_varchar(
					&o_len, o_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));

				n_ptr = row_mysql_read_true_varchar(
					&n_len, n_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));
			}

			break;
		default:
			;
		}

		if (field_mysql_type == MYSQL_TYPE_LONGLONG
		    && prebuilt->table->fts
		    && innobase_strcasecmp(
			field->field_name, FTS_DOC_ID_COL_NAME) == 0) {
			doc_id = (doc_id_t) mach_read_from_n_little_endian(
				n_ptr, 8);
			if (doc_id == 0) {
				return(DB_FTS_INVALID_DOCID);
			}
		}


		if (field->real_maybe_null()) {
			if (field->is_null_in_record(old_row)) {
				o_len = UNIV_SQL_NULL;
			}

			if (field->is_null_in_record(new_row)) {
				n_len = UNIV_SQL_NULL;
			}
		}

		if (o_len != n_len || (o_len != UNIV_SQL_NULL &&
					0 != memcmp(o_ptr, n_ptr, o_len))) {
			/* The field has changed */

			ufield = uvect->fields + n_changed;
			UNIV_MEM_INVALID(ufield, sizeof *ufield);

			/* Let us use a dummy dfield to make the conversion
			from the MySQL column format to the InnoDB format */

			if (n_len != UNIV_SQL_NULL) {
				dict_col_copy_type(prebuilt->table->cols + i,
						   dfield_get_type(&dfield));

				buf = row_mysql_store_col_in_innobase_format(
					&dfield,
					(byte*) buf,
					TRUE,
					new_mysql_row_col,
					col_pack_len,
					dict_table_is_comp(prebuilt->table),
					field->column_format() ==
						COLUMN_FORMAT_TYPE_COMPRESSED,
					reinterpret_cast<const byte*>(
						field->zip_dict_data.str),
					field->zip_dict_data.length,
					prebuilt);
				dfield_copy(&ufield->new_val, &dfield);
			} else {
				dfield_set_null(&ufield->new_val);
			}

			ufield->exp = NULL;
			ufield->orig_len = 0;
			ufield->field_no = dict_col_get_clust_pos(
				&prebuilt->table->cols[i], clust_index);
			n_changed++;

			/* If an FTS indexed column was changed by this
			UPDATE then we need to inform the FTS sub-system.

			NOTE: Currently we re-index all FTS indexed columns
			even if only a subset of the FTS indexed columns
			have been updated. That is the reason we are
			checking only once here. Later we will need to
			note which columns have been updated and do
			selective processing. */
			if (prebuilt->table->fts != NULL) {
				ulint           offset;
				dict_table_t*   innodb_table;

				innodb_table = prebuilt->table;

				if (!changes_fts_column) {
					offset = row_upd_changes_fts_column(
						innodb_table, ufield);

					if (offset != ULINT_UNDEFINED) {
						changes_fts_column = TRUE;
					}
				}

				if (!changes_fts_doc_col) {
					changes_fts_doc_col =
					row_upd_changes_doc_id(
						innodb_table, ufield);
				}
			}
		}
	}

	/* If the update changes a column with an FTS index on it, we
	then add an update column node with a new document id to the
	other changes. We piggy back our changes on the normal UPDATE
	to reduce processing and IO overhead. */
	if (!prebuilt->table->fts) {
			trx->fts_next_doc_id = 0;
	} else if (changes_fts_column || changes_fts_doc_col) {
		dict_table_t*   innodb_table = prebuilt->table;

		ufield = uvect->fields + n_changed;

		if (!DICT_TF2_FLAG_IS_SET(
			innodb_table, DICT_TF2_FTS_HAS_DOC_ID)) {

			/* If Doc ID is managed by user, and if any
			FTS indexed column has been updated, its corresponding
			Doc ID must also be updated. Otherwise, return
			error */
			if (changes_fts_column && !changes_fts_doc_col) {
				ut_print_timestamp(stderr);
				fprintf(stderr, " InnoDB: A new Doc ID"
					" must be supplied while updating"
					" FTS indexed columns.\n");
				return(DB_FTS_INVALID_DOCID);
			}

			/* Doc ID must monotonically increase */
			ut_ad(innodb_table->fts->cache);
			if (doc_id < prebuilt->table->fts->cache->next_doc_id) {
				fprintf(stderr,
					"InnoDB: FTS Doc ID must be larger than"
					" " IB_ID_FMT " for table",
					innodb_table->fts->cache->next_doc_id
					- 1);
				ut_print_name(stderr, trx,
					      TRUE, innodb_table->name);
				putc('\n', stderr);

				return(DB_FTS_INVALID_DOCID);
			} else if ((doc_id
				    - prebuilt->table->fts->cache->next_doc_id)
				   >= FTS_DOC_ID_MAX_STEP) {
				fprintf(stderr,
					"InnoDB: Doc ID " UINT64PF " is too"
					" big. Its difference with largest"
					" Doc ID used " UINT64PF " cannot"
					" exceed or equal to %d\n",
					doc_id,
					prebuilt->table->fts->cache->next_doc_id - 1,
					FTS_DOC_ID_MAX_STEP);
			}


			trx->fts_next_doc_id = doc_id;
		} else {
			/* If the Doc ID is a hidden column, it can't be
			changed by user */
			ut_ad(!changes_fts_doc_col);

			/* Doc ID column is hidden, a new Doc ID will be
			generated by following fts_update_doc_id() call */
			trx->fts_next_doc_id = 0;
		}

		fts_update_doc_id(
			innodb_table, ufield, &trx->fts_next_doc_id);

		++n_changed;
	} else {
		/* We have a Doc ID column, but none of FTS indexed
		columns are touched, nor the Doc ID column, so set
		fts_next_doc_id to UINT64_UNDEFINED, which means do not
		update the Doc ID column */
		trx->fts_next_doc_id = UINT64_UNDEFINED;
	}

	uvect->n_fields = n_changed;
	uvect->info_bits = 0;

	ut_a(buf <= (byte*) original_upd_buff + buff_len);

	return(DB_SUCCESS);
}

/**********************************************************************//**
Updates a row given as a parameter to a new value. Note that we are given
whole rows, not just the fields which are updated: this incurs some
overhead for CPU when we check which fields are actually updated.
TODO: currently InnoDB does not prevent the 'Halloween problem':
in a searched update a single row can get updated several times
if its index columns are updated!
@return	error number or 0 */
UNIV_INTERN
int
ha_innobase::update_row(
/*====================*/
	const uchar*	old_row,	/*!< in: old row in MySQL format */
	uchar*		new_row)	/*!< in: new row in MySQL format */
{
	upd_t*		uvect;
	dberr_t		error;
	trx_t*		trx = thd_to_trx(user_thd);

	DBUG_ENTER("ha_innobase::update_row");

	DEBUG_SYNC(user_thd, "ha_innobase_update_row");

	ut_a(prebuilt->trx == trx);

	if (high_level_read_only) {
		ib_senderrf(ha_thd(), IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	if (upd_buf == NULL) {
		ut_ad(upd_buf_size == 0);

		/* Create a buffer for packing the fields of a record. Why
		table->reclength did not work here? Obviously, because char
		fields when packed actually became 1 byte longer, when we also
		stored the string length as the first byte. */

		upd_buf_size = table->s->reclength + table->s->max_key_length
			+ MAX_REF_PARTS * 3;
		upd_buf = (uchar*) my_malloc(upd_buf_size, MYF(MY_WME));
		if (upd_buf == NULL) {
			upd_buf_size = 0;
			DBUG_RETURN(HA_ERR_OUT_OF_MEM);
		}
	}

	ha_statistic_increment(&SSV::ha_update_count);

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (prebuilt->upd_node) {
		uvect = prebuilt->upd_node->update;
	} else {
		uvect = row_get_prebuilt_update_vector(prebuilt);
	}

	/* Build an update vector from the modified fields in the rows
	(uses upd_buf of the handle) */

	error = calc_row_difference(uvect, (uchar*) old_row, new_row, table,
				    upd_buf, upd_buf_size, prebuilt, user_thd);

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	/* This is not a delete */
	prebuilt->upd_node->is_delete = FALSE;

	ut_a(prebuilt->template_type == ROW_MYSQL_WHOLE_ROW);

	innobase_srv_conc_enter_innodb(trx);

	error = row_update_for_mysql((byte*) old_row, prebuilt);

	/* We need to do some special AUTOINC handling for the following case:

	INSERT INTO t (c1,c2) VALUES(x,y) ON DUPLICATE KEY UPDATE ...

	We need to use the AUTOINC counter that was actually used by
	MySQL in the UPDATE statement, which can be different from the
	value used in the INSERT statement.*/

	if (error == DB_SUCCESS
	    && table->next_number_field
	    && new_row == table->record[0]
	    && thd_sql_command(user_thd) == SQLCOM_INSERT
	    && trx->duplicates)  {

		ulonglong	auto_inc;
		ulonglong	col_max_value;

		auto_inc = table->next_number_field->val_int();

		/* We need the upper limit of the col type to check for
		whether we update the table autoinc counter or not. */
		col_max_value = innobase_get_int_col_max_value(
			table->next_number_field);

		if (auto_inc <= col_max_value && auto_inc != 0) {

			ulonglong	offset;
			ulonglong	increment;

			offset = prebuilt->autoinc_offset;
			increment = prebuilt->autoinc_increment;

			auto_inc = innobase_next_autoinc(
				auto_inc, 1, increment, offset, col_max_value);

			error = innobase_set_max_autoinc(auto_inc);
		}
	}

	innobase_srv_conc_exit_innodb(trx);

func_exit:
	int err = convert_error_code_to_mysql(error,
					    prebuilt->table->flags, user_thd);

	/* If success and no columns were updated. */
	if (err == 0 && uvect->n_fields == 0) {

		/* This is the same as success, but instructs
		MySQL that the row is not really updated and it
		should not increase the count of updated rows.
		This is fix for http://bugs.mysql.com/29157 */
		err = HA_ERR_RECORD_IS_THE_SAME;
	} else if (err == HA_FTS_INVALID_DOCID) {
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	}

	/* Tell InnoDB server that there might be work for
	utility threads: */

	innobase_active_small();

#ifdef WITH_WSREP
	/* Append key
	a. there is no error in update
	b. exec mode is local
	   (it is workload executor node and not replicator node)
	c. wsrep is enabled
	d. No consistency check enforced.
	e. Ensure that bin-logging is enabled.
	   Either mysql bin-logging or emulated bin logging.
	f. If db_type = PARTITION and InnoDB parts are created then MySQL flow
	   will disable bin-logging before inserting data to indivdual parts.
	   This is to avoid double bin-logging. Generic level bin-logging is
	   done at ha_partition level. This causes LDI on partition table
	   to fail as thd_binlog_format() will then return != ROW.
	   In order to take care of this situation we ORed that condition
	   with db_type
	TODO: We allow replication even if binlog-format = STATEMENT.
	This is needed by pt-table-checksum. Now it is not a good idea
	to open this hook for pt-table-checksum but it exist like this for
	while now so to maintain compatibility we continue to provide it.
	With that there comes another existing dependency. */
	if (!err
	    && wsrep_thd_exec_mode(user_thd) == LOCAL_STATE
	    && wsrep_on(user_thd)
	    && !wsrep_consistency_check(user_thd)
	    && (thd_binlog_format(user_thd) == BINLOG_FORMAT_ROW
		|| thd_binlog_format(user_thd) == BINLOG_FORMAT_STMT
	        || table->file->ht->db_type == DB_TYPE_PARTITION_DB)) {

		DBUG_PRINT("wsrep", ("update row key"));

		if (wsrep_append_keys(user_thd, WSREP_KEY_EXCLUSIVE, old_row,
				      new_row)) {
			DBUG_PRINT("wsrep", ("row key failed"));
			err = HA_ERR_INTERNAL_ERROR;
			goto wsrep_error;
		}
	}
wsrep_error:
#endif
	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	DBUG_RETURN(err);
}

/**********************************************************************//**
Deletes a row given as the parameter.
@return	error number or 0 */
UNIV_INTERN
int
ha_innobase::delete_row(
/*====================*/
	const uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
	trx_t*		trx = thd_to_trx(user_thd);

	DBUG_ENTER("ha_innobase::delete_row");

	ut_a(prebuilt->trx == trx);

	if (high_level_read_only) {
		ib_senderrf(ha_thd(), IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	ha_statistic_increment(&SSV::ha_delete_count);

	if (UNIV_UNLIKELY(share && share->ib_table
			  && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (!prebuilt->upd_node) {
		row_get_prebuilt_update_vector(prebuilt);
	}

	/* This is a delete */

	prebuilt->upd_node->is_delete = TRUE;

	innobase_srv_conc_enter_innodb(trx);

	error = row_update_for_mysql((byte*) record, prebuilt);

	innobase_srv_conc_exit_innodb(trx);

	/* Tell the InnoDB server that there might be work for
	utility threads: */

	innobase_active_small();

#ifdef WITH_WSREP
	/* Append key
	a. there is no error in delete
	b. exec mode is local
	   (it is workload executor node and not replicator node)
	c. wsrep is enabled
	d. No consistency check enforced.
	e. Ensure that bin-logging is enabled.
	   Either mysql bin-logging or emulated bin logging.
	f. If db_type = PARTITION and InnoDB parts are created then MySQL flow
	   will disable bin-logging before inserting data to indivdual parts.
	   This is to avoid double bin-logging. Generic level bin-logging is
	   done at ha_partition level. This causes LDI on partition table
	   to fail as thd_binlog_format() will then return != ROW.
	   In order to take care of this situation we ORed that condition
	   with db_type
	TODO: We allow replication even if binlog-format = STATEMENT.
	This is needed by pt-table-checksum. Now it is not a god idea
	to open this hook for pt-table-checksum but it exist like this for
	while now so to maintain compatibility we continue to provide it.
	With that there comes another existing dependency. */
	if (error == DB_SUCCESS
	    && wsrep_thd_exec_mode(user_thd) == LOCAL_STATE
	    && wsrep_on(user_thd)
	    && !wsrep_consistency_check(user_thd)
	    && (thd_binlog_format(user_thd) == BINLOG_FORMAT_ROW
		|| thd_binlog_format(user_thd) == BINLOG_FORMAT_STMT
	        || table->file->ht->db_type == DB_TYPE_PARTITION_DB)) {

		if (wsrep_append_keys(user_thd, WSREP_KEY_EXCLUSIVE, record,
				      NULL)) {
			DBUG_PRINT("wsrep", ("delete fail"));
			error = DB_ERROR;
			goto wsrep_error;
		}
	}

wsrep_error:
#endif
	if (UNIV_UNLIKELY(share && share->ib_table
			  && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	DBUG_RETURN(convert_error_code_to_mysql(
			    error, prebuilt->table->flags, user_thd));
}

/**********************************************************************//**
Removes a new lock set on a row, if it was not read optimistically. This can
be called after a row has been read in the processing of an UPDATE or a DELETE
query, if the option innodb_locks_unsafe_for_binlog is set. */
UNIV_INTERN
void
ha_innobase::unlock_row(void)
/*=========================*/
{
	DBUG_ENTER("ha_innobase::unlock_row");

	/* Consistent read does not take any locks, thus there is
	nothing to unlock. */

	if (prebuilt->select_lock_type == LOCK_NONE) {
		DBUG_VOID_RETURN;
	}

	/* Ideally, this assert must be in the beginning of the function.
	But there are some calls to this function from the SQL layer when the
	transaction is in state TRX_STATE_NOT_STARTED.  The check on
	prebuilt->select_lock_type above gets around this issue. */
	ut_ad(trx_state_eq(prebuilt->trx, TRX_STATE_ACTIVE));

	switch (prebuilt->row_read_type) {
	case ROW_READ_WITH_LOCKS:
		if (!srv_locks_unsafe_for_binlog
		    && prebuilt->trx->isolation_level
		    > TRX_ISO_READ_COMMITTED) {
			break;
		}
		/* fall through */
	case ROW_READ_TRY_SEMI_CONSISTENT:
		row_unlock_for_mysql(prebuilt, FALSE);
		break;
	case ROW_READ_DID_SEMI_CONSISTENT:
		prebuilt->row_read_type = ROW_READ_TRY_SEMI_CONSISTENT;
		break;
	}

	DBUG_VOID_RETURN;
}

/* See handler.h and row0mysql.h for docs on this function. */
UNIV_INTERN
bool
ha_innobase::was_semi_consistent_read(void)
/*=======================================*/
{
	return(prebuilt->row_read_type == ROW_READ_DID_SEMI_CONSISTENT);
}

/* See handler.h and row0mysql.h for docs on this function. */
UNIV_INTERN
void
ha_innobase::try_semi_consistent_read(bool yes)
/*===========================================*/
{
	ut_a(prebuilt->trx == thd_to_trx(ha_thd()));

	/* Row read type is set to semi consistent read if this was
	requested by the MySQL and either innodb_locks_unsafe_for_binlog
	option is used or this session is using READ COMMITTED isolation
	level. */

	if (yes
	    && (srv_locks_unsafe_for_binlog
		|| prebuilt->trx->isolation_level <= TRX_ISO_READ_COMMITTED)) {
		prebuilt->row_read_type = ROW_READ_TRY_SEMI_CONSISTENT;
	} else {
		prebuilt->row_read_type = ROW_READ_WITH_LOCKS;
	}
}

/******************************************************************//**
Initializes a handle to use an index.
@return	0 or error number */
UNIV_INTERN
int
ha_innobase::index_init(
/*====================*/
	uint	keynr,	/*!< in: key (index) number */
	bool sorted)	/*!< in: 1 if result MUST be sorted according to index */
{
	DBUG_ENTER("index_init");

	DBUG_RETURN(change_active_index(keynr));
}

/******************************************************************//**
Currently does nothing.
@return	0 */
UNIV_INTERN
int
ha_innobase::index_end(void)
/*========================*/
{
	int	error	= 0;
	DBUG_ENTER("index_end");
	active_index = MAX_KEY;
	in_range_check_pushed_down = FALSE;
	ds_mrr.dsmrr_close();
	DBUG_RETURN(error);
}

/*********************************************************************//**
Converts a search mode flag understood by MySQL to a flag understood
by InnoDB. */
static inline
ulint
convert_search_mode_to_innobase(
/*============================*/
	enum ha_rkey_function	find_flag)
{
	switch (find_flag) {
	case HA_READ_KEY_EXACT:
		/* this does not require the index to be UNIQUE */
		return(PAGE_CUR_GE);
	case HA_READ_KEY_OR_NEXT:
		return(PAGE_CUR_GE);
	case HA_READ_KEY_OR_PREV:
		return(PAGE_CUR_LE);
	case HA_READ_AFTER_KEY:
		return(PAGE_CUR_G);
	case HA_READ_BEFORE_KEY:
		return(PAGE_CUR_L);
	case HA_READ_PREFIX:
		return(PAGE_CUR_GE);
	case HA_READ_PREFIX_LAST:
		return(PAGE_CUR_LE);
	case HA_READ_PREFIX_LAST_OR_PREV:
		return(PAGE_CUR_LE);
		/* In MySQL-4.0 HA_READ_PREFIX and HA_READ_PREFIX_LAST always
		pass a complete-field prefix of a key value as the search
		tuple. I.e., it is not allowed that the last field would
		just contain n first bytes of the full field value.
		MySQL uses a 'padding' trick to convert LIKE 'abc%'
		type queries so that it can use as a search tuple
		a complete-field-prefix of a key value. Thus, the InnoDB
		search mode PAGE_CUR_LE_OR_EXTENDS is never used.
		TODO: when/if MySQL starts to use also partial-field
		prefixes, we have to deal with stripping of spaces
		and comparison of non-latin1 char type fields in
		innobase_mysql_cmp() to get PAGE_CUR_LE_OR_EXTENDS to
		work correctly. */
	case HA_READ_MBR_CONTAIN:
	case HA_READ_MBR_INTERSECT:
	case HA_READ_MBR_WITHIN:
	case HA_READ_MBR_DISJOINT:
	case HA_READ_MBR_EQUAL:
		return(PAGE_CUR_UNSUPP);
	/* do not use "default:" in order to produce a gcc warning:
	enumeration value '...' not handled in switch
	(if -Wswitch or -Wall is used) */
	}

	my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "this functionality");

	return(PAGE_CUR_UNSUPP);
}

/*
   BACKGROUND INFO: HOW A SELECT SQL QUERY IS EXECUTED
   ---------------------------------------------------
The following does not cover all the details, but explains how we determine
the start of a new SQL statement, and what is associated with it.

For each table in the database the MySQL interpreter may have several
table handle instances in use, also in a single SQL query. For each table
handle instance there is an InnoDB  'prebuilt' struct which contains most
of the InnoDB data associated with this table handle instance.

  A) if the user has not explicitly set any MySQL table level locks:

  1) MySQL calls ::external_lock to set an 'intention' table level lock on
the table of the handle instance. There we set
prebuilt->sql_stat_start = TRUE. The flag sql_stat_start should be set
true if we are taking this table handle instance to use in a new SQL
statement issued by the user. We also increment trx->n_mysql_tables_in_use.

  2) If prebuilt->sql_stat_start == TRUE we 'pre-compile' the MySQL search
instructions to prebuilt->template of the table handle instance in
::index_read. The template is used to save CPU time in large joins.

  3) In row_search_for_mysql, if prebuilt->sql_stat_start is true, we
allocate a new consistent read view for the trx if it does not yet have one,
or in the case of a locking read, set an InnoDB 'intention' table level
lock on the table.

  4) We do the SELECT. MySQL may repeatedly call ::index_read for the
same table handle instance, if it is a join.

  5) When the SELECT ends, MySQL removes its intention table level locks
in ::external_lock. When trx->n_mysql_tables_in_use drops to zero,
 (a) we execute a COMMIT there if the autocommit is on,
 (b) we also release possible 'SQL statement level resources' InnoDB may
have for this SQL statement. The MySQL interpreter does NOT execute
autocommit for pure read transactions, though it should. That is why the
table handler in that case has to execute the COMMIT in ::external_lock.

  B) If the user has explicitly set MySQL table level locks, then MySQL
does NOT call ::external_lock at the start of the statement. To determine
when we are at the start of a new SQL statement we at the start of
::index_read also compare the query id to the latest query id where the
table handle instance was used. If it has changed, we know we are at the
start of a new SQL statement. Since the query id can theoretically
overwrap, we use this test only as a secondary way of determining the
start of a new SQL statement. */


/**********************************************************************//**
Positions an index cursor to the index specified in the handle. Fetches the
row if any.
@return	0, HA_ERR_KEY_NOT_FOUND, or error number */
UNIV_INTERN
int
ha_innobase::index_read(
/*====================*/
	uchar*		buf,		/*!< in/out: buffer for the returned
					row */
	const uchar*	key_ptr,	/*!< in: key value; if this is NULL
					we position the cursor at the
					start or end of index; this can
					also contain an InnoDB row id, in
					which case key_len is the InnoDB
					row id length; the key value can
					also be a prefix of a full key value,
					and the last column can be a prefix
					of a full column */
	uint			key_len,/*!< in: key value length */
	enum ha_rkey_function find_flag)/*!< in: search flags from my_base.h */
{
	ulint		mode;
	dict_index_t*	index;
	ulint		match_mode	= 0;
	int		error;
	dberr_t		ret;

	DBUG_ENTER("index_read");
	DEBUG_SYNC_C("ha_innobase_index_read_begin");

	ut_a(prebuilt->trx == thd_to_trx(user_thd));
	ut_ad(key_len != 0 || find_flag != HA_READ_KEY_EXACT);

	ha_statistic_increment(&SSV::ha_read_key_count);

	if (UNIV_UNLIKELY(srv_pass_corrupt_table <= 1 && share
			  && share->ib_table && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	index = prebuilt->index;

	if (UNIV_UNLIKELY(index == NULL) || dict_index_is_corrupted(index)) {
		prebuilt->index_usable = FALSE;
		DBUG_RETURN(HA_ERR_CRASHED);
	}
	if (UNIV_UNLIKELY(!prebuilt->index_usable)) {
		DBUG_RETURN(dict_index_is_corrupted(index)
			    ? HA_ERR_INDEX_CORRUPT
			    : HA_ERR_TABLE_DEF_CHANGED);
	}

	if (index->type & DICT_FTS) {
		DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
	}

	/* Note that if the index for which the search template is built is not
	necessarily prebuilt->index, but can also be the clustered index */

	if (prebuilt->sql_stat_start) {
		build_template(false);
	}

	if (key_ptr) {
		/* Convert the search key value to InnoDB format into
		prebuilt->search_tuple */

		row_sel_convert_mysql_key_to_innobase(
			prebuilt->search_tuple,
			prebuilt->srch_key_val1,
			prebuilt->srch_key_val_len,
			index,
			(byte*) key_ptr,
			(ulint) key_len,
			prebuilt->trx);
		DBUG_ASSERT(prebuilt->search_tuple->n_fields > 0);
	} else {
		/* We position the cursor to the last or the first entry
		in the index */

		dtuple_set_n_fields(prebuilt->search_tuple, 0);
	}

	mode = convert_search_mode_to_innobase(find_flag);

	match_mode = 0;

	if (find_flag == HA_READ_KEY_EXACT) {

		match_mode = ROW_SEL_EXACT;

	} else if (find_flag == HA_READ_PREFIX
		   || find_flag == HA_READ_PREFIX_LAST) {

		match_mode = ROW_SEL_EXACT_PREFIX;
	}

	last_match_mode = (uint) match_mode;

	if (mode != PAGE_CUR_UNSUPP) {

		innobase_srv_conc_enter_innodb(prebuilt->trx);

		ret = row_search_for_mysql((byte*) buf, mode, prebuilt,
					   match_mode, 0);

		innobase_srv_conc_exit_innodb(prebuilt->trx);
	} else {

		ret = DB_UNSUPPORTED;
	}

	if (UNIV_UNLIKELY(srv_pass_corrupt_table <= 1 && share
			  && share->ib_table && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	switch (ret) {
	case DB_SUCCESS:
		error = 0;
		table->status = 0;
		srv_stats.n_rows_read.add((size_t) prebuilt->trx->id, 1);
		break;
	case DB_RECORD_NOT_FOUND:
		error = HA_ERR_KEY_NOT_FOUND;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_END_OF_INDEX:
		error = HA_ERR_KEY_NOT_FOUND;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_TABLESPACE_DELETED:

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_NO_SUCH_TABLE;
		break;
	case DB_TABLESPACE_NOT_FOUND:

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING, MYF(0),
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_NO_SUCH_TABLE;
		break;
	default:
		error = convert_error_code_to_mysql(
			ret, prebuilt->table->flags, user_thd);

		table->status = STATUS_NOT_FOUND;
		break;
	}

	DBUG_RETURN(error);
}

/*******************************************************************//**
The following functions works like index_read, but it find the last
row with the current key value or prefix.
@return	0, HA_ERR_KEY_NOT_FOUND, or an error code */
UNIV_INTERN
int
ha_innobase::index_read_last(
/*=========================*/
	uchar*		buf,	/*!< out: fetched row */
	const uchar*	key_ptr,/*!< in: key value, or a prefix of a full
				key value */
	uint		key_len)/*!< in: length of the key val or prefix
				in bytes */
{
	return(index_read(buf, key_ptr, key_len, HA_READ_PREFIX_LAST));
}

/********************************************************************//**
Get the index for a handle. Does not change active index.
@return	NULL or index instance. */
UNIV_INTERN
dict_index_t*
ha_innobase::innobase_get_index(
/*============================*/
	uint		keynr)	/*!< in: use this index; MAX_KEY means always
				clustered index, even if it was internally
				generated by InnoDB */
{
	KEY*		key = 0;
	dict_index_t*	index = 0;

	DBUG_ENTER("innobase_get_index");

	if (keynr != MAX_KEY && table->s->keys > 0) {
		key = table->key_info + keynr;

		index = innobase_index_lookup(share, keynr);

		if (index) {
			ut_a(ut_strcmp(index->name, key->name) == 0);
		} else {
			/* Can't find index with keynr in the translation
			table. Only print message if the index translation
			table exists */
			if (share->idx_trans_tbl.index_mapping) {
				sql_print_warning("InnoDB could not find "
						  "index %s key no %u for "
						  "table %s through its "
						  "index translation table",
						  key ? key->name : "NULL",
						  keynr,
						  prebuilt->table->name);
			}

			index = dict_table_get_index_on_name(prebuilt->table,
							     key->name);
		}
	} else {
		index = dict_table_get_first_index(prebuilt->table);
	}

	if (!index) {
		sql_print_error(
			"Innodb could not find key n:o %u with name %s "
			"from dict cache for table %s",
			keynr, key ? key->name : "NULL",
			prebuilt->table->name);
	}

	DBUG_RETURN(index);
}

/********************************************************************//**
Changes the active index of a handle.
@return	0 or error code */
UNIV_INTERN
int
ha_innobase::change_active_index(
/*=============================*/
	uint	keynr)	/*!< in: use this index; MAX_KEY means always clustered
			index, even if it was internally generated by
			InnoDB */
{
	DBUG_ENTER("change_active_index");

	if (UNIV_UNLIKELY(srv_pass_corrupt_table <= 1 && share
			  && share->ib_table && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	ut_ad(user_thd == ha_thd());
	ut_a(prebuilt->trx == thd_to_trx(user_thd));

	active_index = keynr;

	prebuilt->index = innobase_get_index(keynr);

	if (UNIV_UNLIKELY(!prebuilt->index)) {
		sql_print_warning("InnoDB: change_active_index(%u) failed",
				  keynr);
		prebuilt->index_usable = FALSE;
		DBUG_RETURN(1);
	}

	prebuilt->index_usable = row_merge_is_index_usable(prebuilt->trx,
							   prebuilt->index);

	if (UNIV_UNLIKELY(!prebuilt->index_usable)) {
		if (dict_index_is_corrupted(prebuilt->index)) {
			char index_name[MAX_FULL_NAME_LEN + 1];
			char table_name[MAX_FULL_NAME_LEN + 1];

			innobase_format_name(
				index_name, sizeof index_name,
				prebuilt->index->name, TRUE);

			innobase_format_name(
				table_name, sizeof table_name,
				prebuilt->index->table->name, FALSE);

			push_warning_printf(
				user_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_INDEX_CORRUPT,
				"InnoDB: Index %s for table %s is"
				" marked as corrupted",
				index_name, table_name);
			DBUG_RETURN(HA_ERR_INDEX_CORRUPT);
		} else {
			push_warning_printf(
				user_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_TABLE_DEF_CHANGED,
				"InnoDB: insufficient history for index %u",
				keynr);
		}

		/* The caller seems to ignore this.  Thus, we must check
		this again in row_search_for_mysql(). */
		DBUG_RETURN(HA_ERR_TABLE_DEF_CHANGED);
	}

	ut_a(prebuilt->search_tuple != 0);

	dtuple_set_n_fields(prebuilt->search_tuple, prebuilt->index->n_fields);

	dict_index_copy_types(prebuilt->search_tuple, prebuilt->index,
			      prebuilt->index->n_fields);

	/* MySQL changes the active index for a handle also during some
	queries, for example SELECT MAX(a), SUM(a) first retrieves the MAX()
	and then calculates the sum. Previously we played safe and used
	the flag ROW_MYSQL_WHOLE_ROW below, but that caused unnecessary
	copying. Starting from MySQL-4.1 we use a more efficient flag here. */

	build_template(false);

	DBUG_RETURN(0);
}

/**********************************************************************//**
Positions an index cursor to the index specified in keynr. Fetches the
row if any.
??? This is only used to read whole keys ???
@return	error number or 0 */
UNIV_INTERN
int
ha_innobase::index_read_idx(
/*========================*/
	uchar*		buf,		/*!< in/out: buffer for the returned
					row */
	uint		keynr,		/*!< in: use this index */
	const uchar*	key,		/*!< in: key value; if this is NULL
					we position the cursor at the
					start or end of index */
	uint		key_len,	/*!< in: key value length */
	enum ha_rkey_function find_flag)/*!< in: search flags from my_base.h */
{
	if (change_active_index(keynr)) {

		return(1);
	}

	return(index_read(buf, key, key_len, find_flag));
}

/***********************************************************************//**
Reads the next or previous row from a cursor, which must have previously been
positioned using index_read.
@return	0, HA_ERR_END_OF_FILE, or error number */
UNIV_INTERN
int
ha_innobase::general_fetch(
/*=======================*/
	uchar*	buf,		/*!< in/out: buffer for next row in MySQL
				format */
	uint	direction,	/*!< in: ROW_SEL_NEXT or ROW_SEL_PREV */
	uint	match_mode)	/*!< in: 0, ROW_SEL_EXACT, or
				ROW_SEL_EXACT_PREFIX */
{
	dberr_t	ret;
	int	error;

	DBUG_ENTER("general_fetch");

	if (UNIV_UNLIKELY(srv_pass_corrupt_table <= 1 && share
			  && share->ib_table && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	ut_a(prebuilt->trx == thd_to_trx(user_thd));

	innobase_srv_conc_enter_innodb(prebuilt->trx);

	ret = row_search_for_mysql(
		(byte*) buf, 0, prebuilt, match_mode, direction);

	innobase_srv_conc_exit_innodb(prebuilt->trx);

	if (UNIV_UNLIKELY(srv_pass_corrupt_table <= 1 && share
			  && share->ib_table && share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	switch (ret) {
	case DB_SUCCESS:
		error = 0;
		table->status = 0;
		srv_stats.n_rows_read.add((size_t) prebuilt->trx->id, 1);
		break;
	case DB_RECORD_NOT_FOUND:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_END_OF_INDEX:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_TABLESPACE_DELETED:

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_NO_SUCH_TABLE;
		break;
	case DB_TABLESPACE_NOT_FOUND:

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_NO_SUCH_TABLE;
		break;
	default:
		error = convert_error_code_to_mysql(
			ret, prebuilt->table->flags, user_thd);

		table->status = STATUS_NOT_FOUND;
		break;
	}

	DBUG_RETURN(error);
}

/***********************************************************************//**
Reads the next row from a cursor, which must have previously been
positioned using index_read.
@return	0, HA_ERR_END_OF_FILE, or error number */
UNIV_INTERN
int
ha_innobase::index_next(
/*====================*/
	uchar*		buf)	/*!< in/out: buffer for next row in MySQL
				format */
{
	ha_statistic_increment(&SSV::ha_read_next_count);

	return(general_fetch(buf, ROW_SEL_NEXT, 0));
}

/*******************************************************************//**
Reads the next row matching to the key value given as the parameter.
@return	0, HA_ERR_END_OF_FILE, or error number */
UNIV_INTERN
int
ha_innobase::index_next_same(
/*=========================*/
	uchar*		buf,	/*!< in/out: buffer for the row */
	const uchar*	key,	/*!< in: key value */
	uint		keylen)	/*!< in: key value length */
{
	ha_statistic_increment(&SSV::ha_read_next_count);

	return(general_fetch(buf, ROW_SEL_NEXT, last_match_mode));
}

/***********************************************************************//**
Reads the previous row from a cursor, which must have previously been
positioned using index_read.
@return	0, HA_ERR_END_OF_FILE, or error number */
UNIV_INTERN
int
ha_innobase::index_prev(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for previous row in MySQL format */
{
	ha_statistic_increment(&SSV::ha_read_prev_count);

	return(general_fetch(buf, ROW_SEL_PREV, 0));
}

/********************************************************************//**
Positions a cursor on the first record in an index and reads the
corresponding row to buf.
@return	0, HA_ERR_END_OF_FILE, or error code */
UNIV_INTERN
int
ha_innobase::index_first(
/*=====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	int	error;

	DBUG_ENTER("index_first");
	ha_statistic_increment(&SSV::ha_read_first_count);

	error = index_read(buf, NULL, 0, HA_READ_AFTER_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Positions a cursor on the last record in an index and reads the
corresponding row to buf.
@return	0, HA_ERR_END_OF_FILE, or error code */
UNIV_INTERN
int
ha_innobase::index_last(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	int	error;

	DBUG_ENTER("index_last");
	ha_statistic_increment(&SSV::ha_read_last_count);

	error = index_read(buf, NULL, 0, HA_READ_BEFORE_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/****************************************************************//**
Initialize a table scan.
@return	0 or error number */
UNIV_INTERN
int
ha_innobase::rnd_init(
/*==================*/
	bool	scan)	/*!< in: TRUE if table/index scan FALSE otherwise */
{
	int	err;

	/* Store the active index value so that we can restore the original
	value after a scan */

	if (prebuilt->clust_index_was_generated) {
		err = change_active_index(MAX_KEY);
	} else {
		err = change_active_index(primary_key);
	}

	/* Don't use semi-consistent read in random row reads (by position).
	This means we must disable semi_consistent_read if scan is false */

	if (!scan) {
		try_semi_consistent_read(0);
	}

	start_of_scan = 1;

	return(err);
}

/*****************************************************************//**
Ends a table scan.
@return	0 or error number */
UNIV_INTERN
int
ha_innobase::rnd_end(void)
/*======================*/
{
	return(index_end());
}

/*****************************************************************//**
Reads the next row in a table scan (also used to read the FIRST row
in a table scan).
@return	0, HA_ERR_END_OF_FILE, or error number */
UNIV_INTERN
int
ha_innobase::rnd_next(
/*==================*/
	uchar*	buf)	/*!< in/out: returns the row in this buffer,
			in MySQL format */
{
	int	error;

	DBUG_ENTER("rnd_next");
	ha_statistic_increment(&SSV::ha_read_rnd_next_count);

	if (start_of_scan) {
		error = index_first(buf);

		if (error == HA_ERR_KEY_NOT_FOUND) {
			error = HA_ERR_END_OF_FILE;
		}

		start_of_scan = 0;
	} else {
		error = general_fetch(buf, ROW_SEL_NEXT, 0);
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Fetches a row from the table based on a row reference.
@return	0, HA_ERR_KEY_NOT_FOUND, or error code */
UNIV_INTERN
int
ha_innobase::rnd_pos(
/*=================*/
	uchar*	buf,	/*!< in/out: buffer for the row */
	uchar*	pos)	/*!< in: primary key value of the row in the
			MySQL format, or the row id if the clustered
			index was internally generated by InnoDB; the
			length of data in pos has to be ref_length */
{
	int		error;
	DBUG_ENTER("rnd_pos");
	DBUG_DUMP("key", pos, ref_length);

	ha_statistic_increment(&SSV::ha_read_rnd_count);

	ut_a(prebuilt->trx == thd_to_trx(ha_thd()));

	/* Note that we assume the length of the row reference is fixed
	for the table, and it is == ref_length */

	error = index_read(buf, pos, ref_length, HA_READ_KEY_EXACT);

	if (error) {
		DBUG_PRINT("error", ("Got error: %d", error));
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Initialize FT index scan
@return 0 or error number */
UNIV_INTERN
int
ha_innobase::ft_init()
/*==================*/
{
	DBUG_ENTER("ft_init");

	trx_t*	trx = check_trx_exists(ha_thd());

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	DBUG_RETURN(rnd_init(false));
}

/**********************************************************************//**
Initialize FT index scan
@return FT_INFO structure if successful or NULL */
UNIV_INTERN
FT_INFO*
ha_innobase::ft_init_ext(
/*=====================*/
	uint			flags,	/* in: */
	uint			keynr,	/* in: */
	String*			key)	/* in: */
{
	trx_t*			trx;
	dict_table_t*		ft_table;
	dberr_t			error;
	byte*			query = (byte*) key->ptr();
	ulint			query_len = key->length();
	const CHARSET_INFO*	char_set = key->charset();
	NEW_FT_INFO*		fts_hdl = NULL;
	dict_index_t*		index;
	fts_result_t*		result;
	char			buf_tmp[8192];
	ulint			buf_tmp_used;
	uint			num_errors;

	if (fts_enable_diag_print) {
		fprintf(stderr, "keynr=%u, '%.*s'\n",
			keynr, (int) key->length(), (byte*) key->ptr());

		if (flags & FT_BOOL) {
			fprintf(stderr, "BOOL search\n");
		} else {
			fprintf(stderr, "NL search\n");
		}
	}

	/* FIXME: utf32 and utf16 are not compatible with some
	string function used. So to convert them to uft8 before
	proceed. */
	if (strcmp(char_set->csname, "utf32") == 0
	    || strcmp(char_set->csname, "utf16") == 0) {
		buf_tmp_used = innobase_convert_string(
			buf_tmp, sizeof(buf_tmp) - 1,
			&my_charset_utf8_general_ci,
			query, query_len, (CHARSET_INFO*) char_set,
			&num_errors);

		query = (byte*) buf_tmp;
		query_len = buf_tmp_used;
		query[query_len] = 0;
	}

	trx = prebuilt->trx;

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	ft_table = prebuilt->table;

	/* Table does not have an FTS index */
	if (!ft_table->fts || ib_vector_is_empty(ft_table->fts->indexes)) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	/* If tablespace is discarded, we should return here */
	if (dict_table_is_discarded(ft_table)) {
		my_error(ER_NO_SUCH_TABLE, MYF(0), table->s->db.str,
			 table->s->table_name.str);
		return(NULL);
	}

	if (keynr == NO_SUCH_KEY) {
		/* FIXME: Investigate the NO_SUCH_KEY usage */
		index = (dict_index_t*) ib_vector_getp(ft_table->fts->indexes, 0);
	} else {
		index = innobase_get_index(keynr);
	}

	if (!index || index->type != DICT_FTS) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	if (!(ft_table->fts->fts_status & ADDED_TABLE_SYNCED)) {
		fts_init_index(ft_table, FALSE);

		ft_table->fts->fts_status |= ADDED_TABLE_SYNCED;
	}

	error = fts_query(trx, index, flags, query, query_len, &result);

	if (error != DB_SUCCESS) {
		my_error(convert_error_code_to_mysql(error, 0, NULL),
			MYF(0));
		return(NULL);
	}

	/* Allocate FTS handler, and instantiate it before return */
	fts_hdl = static_cast<NEW_FT_INFO*>(my_malloc(sizeof(NEW_FT_INFO),
				   MYF(0)));

	fts_hdl->please = const_cast<_ft_vft*>(&ft_vft_result);
	fts_hdl->could_you = const_cast<_ft_vft_ext*>(&ft_vft_ext_result);
	fts_hdl->ft_prebuilt = prebuilt;
	fts_hdl->ft_result = result;

	/* FIXME: Re-evluate the condition when Bug 14469540
	is resolved */
	prebuilt->in_fts_query = true;

	return((FT_INFO*) fts_hdl);
}

/*****************************************************************//**
Copy a cached MySQL row.
If requested, also avoids overwriting non-read columns.
@param[out]     buf             Row in MySQL format.
@param[in]      cached_row      Which row to copy.
@param[in]	rec_len		Record length. */
void
ha_innobase::copy_cached_row(
	uchar*		buf,
	const uchar*    cached_row,
	uint		rec_len)
{
	if (prebuilt->keep_other_fields_on_keyread) {
                row_sel_copy_cached_fields_for_mysql(buf, cached_row,
                        prebuilt);
        } else {
                memcpy(buf, cached_row, rec_len);
        }
}


/*****************************************************************//**
Set up search tuple for a query through FTS_DOC_ID_INDEX on
supplied Doc ID. This is used by MySQL to retrieve the documents
once the search result (Doc IDs) is available */
static
void
innobase_fts_create_doc_id_key(
/*===========================*/
	dtuple_t*	tuple,		/* in/out: prebuilt->search_tuple */
	const dict_index_t*
			index,		/* in: index (FTS_DOC_ID_INDEX) */
	doc_id_t*	doc_id)		/* in/out: doc id to search, value
					could be changed to storage format
					used for search. */
{
	doc_id_t	temp_doc_id;
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);

	ut_a(dict_index_get_n_unique(index) == 1);
	dtuple_set_n_fields(tuple, index->n_fields);
	dict_index_copy_types(tuple, index, index->n_fields);

#ifdef UNIV_DEBUG
	/* The unique Doc ID field should be an eight-bytes integer */
	dict_field_t*	field = dict_index_get_nth_field(index, 0);
        ut_a(field->col->mtype == DATA_INT);
	ut_ad(sizeof(*doc_id) == field->fixed_len);
	ut_ad(innobase_strcasecmp(index->name, FTS_DOC_ID_INDEX_NAME) == 0);
#endif /* UNIV_DEBUG */

	/* Convert to storage byte order */
	mach_write_to_8(reinterpret_cast<byte*>(&temp_doc_id), *doc_id);
	*doc_id = temp_doc_id;
	dfield_set_data(dfield, doc_id, sizeof(*doc_id));

        dtuple_set_n_fields_cmp(tuple, 1);

	for (ulint i = 1; i < index->n_fields; i++) {
		dfield = dtuple_get_nth_field(tuple, i);
		dfield_set_null(dfield);
	}
}

/**********************************************************************//**
Fetch next result from the FT result set
@return error code */
UNIV_INTERN
int
ha_innobase::ft_read(
/*=================*/
	uchar*		buf)		/*!< in/out: buf contain result row */
{
	fts_result_t*	result;
	int		error;
	row_prebuilt_t*	ft_prebuilt;

	ft_prebuilt = ((NEW_FT_INFO*) ft_handler)->ft_prebuilt;

	ut_a(ft_prebuilt == prebuilt);

	result = ((NEW_FT_INFO*) ft_handler)->ft_result;

	if (result->current == NULL) {
		/* This is the case where the FTS query did not
		contain and matching documents. */
		if (result->rankings_by_id != NULL) {
			/* Now that we have the complete result, we
			need to sort the document ids on their rank
			calculation. */

			fts_query_sort_result_on_rank(result);

			result->current = const_cast<ib_rbt_node_t*>(
				rbt_first(result->rankings_by_rank));
		} else {
			ut_a(result->current == NULL);
		}
	} else {
		result->current = const_cast<ib_rbt_node_t*>(
			rbt_next(result->rankings_by_rank, result->current));
	}

next_record:

	if (result->current != NULL) {
		dict_index_t*	index;
		dtuple_t*	tuple = prebuilt->search_tuple;
		doc_id_t	search_doc_id;

		/* If we only need information from result we can return
		   without fetching the table row */
		if (ft_prebuilt->read_just_key) {
			table->status= 0;
			return(0);
		}

		index = dict_table_get_index_on_name(
			prebuilt->table, FTS_DOC_ID_INDEX_NAME);

		/* Must find the index */
		ut_a(index);

		/* Switch to the FTS doc id index */
		prebuilt->index = index;

		fts_ranking_t*	ranking = rbt_value(
			fts_ranking_t, result->current);

		search_doc_id = ranking->doc_id;

		/* We pass a pointer of search_doc_id because it will be
		converted to storage byte order used in the search
		tuple. */
		innobase_fts_create_doc_id_key(tuple, index, &search_doc_id);

		innobase_srv_conc_enter_innodb(prebuilt->trx);

		dberr_t ret = row_search_for_mysql(
			(byte*) buf, PAGE_CUR_GE, prebuilt, ROW_SEL_EXACT, 0);

		innobase_srv_conc_exit_innodb(prebuilt->trx);

		switch (ret) {
		case DB_SUCCESS:
			error = 0;
			table->status = 0;
			break;
		case DB_RECORD_NOT_FOUND:
			result->current = const_cast<ib_rbt_node_t*>(
				rbt_next(result->rankings_by_rank,
					 result->current));

			if (!result->current) {
				/* exhaust the result set, should return
				HA_ERR_END_OF_FILE just like
				ha_innobase::general_fetch() and/or
				ha_innobase::index_first() etc. */
				error = HA_ERR_END_OF_FILE;
				table->status = STATUS_NOT_FOUND;
			} else {
				goto next_record;
			}
			break;
		case DB_END_OF_INDEX:
			error = HA_ERR_END_OF_FILE;
			table->status = STATUS_NOT_FOUND;
			break;
		case DB_TABLESPACE_DELETED:

			ib_senderrf(
				prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_DISCARDED,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_NO_SUCH_TABLE;
			break;
		case DB_TABLESPACE_NOT_FOUND:

			ib_senderrf(
				prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_MISSING,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_NO_SUCH_TABLE;
			break;
		default:
			error = convert_error_code_to_mysql(
				ret, 0, user_thd);

			table->status = STATUS_NOT_FOUND;
			break;
		}

		return(error);
	}

	return(HA_ERR_END_OF_FILE);
}

/*************************************************************************
*/

void
ha_innobase::ft_end()
{
	fprintf(stderr, "ft_end()\n");

	rnd_end();
}
#ifdef WITH_WSREP
dict_index_t*
wsrep_dict_foreign_find_index(
	dict_table_t*	table,
	const char**	col_names,
	const char**	columns,
	ulint		n_cols,
	dict_index_t*	types_idx,
	ibool		check_charsets,
	ulint		check_null);

inline
const char*
wsrep_key_type_to_str(wsrep_key_type type)
{
	switch (type) {
	case WSREP_KEY_SHARED:
		return "shared";
	case WSREP_KEY_SEMI:
		return "semi";
	case WSREP_KEY_EXCLUSIVE:
		return "exclusive";
	};
	return "unknown";
}

extern
dberr_t
wsrep_append_foreign_key(
	trx_t*		trx,		/*!< in: trx */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	const rec_t*	rec,		/*!< in: clustered index record */
	dict_index_t*	index,		/*!< in: clustered index */
	ibool		referenced,	/*!< in: is check for referenced table */
	wsrep_key_type	key_type)	/*!< in: access type of this key
					(shared, exclusive, semi...) */
{
	THD* thd = (THD*)trx->mysql_thd;
	int rcode = 0;
	char cache_key[513] = {'\0'};
	int cache_key_len;
	bool const copy = true;
	ut_a(trx);

	if (!wsrep_on(trx->mysql_thd) ||
	    wsrep_thd_exec_mode(thd) != LOCAL_STATE)
		return DB_SUCCESS;

	if (!thd || !foreign ||
	    (!foreign->referenced_table && !foreign->foreign_table))
	{
		WSREP_INFO("FK: %s missing in: %s",
			(!thd)      ?  "thread"     :
			((!foreign) ?  "constraint" :
			((!foreign->referenced_table) ?
			     "referenced table" : "foreign table")),
			   (thd && wsrep_thd_query(thd)) ?
 			   wsrep_thd_query(thd) : "void");
		return DB_ERROR;
	}

	if ( !((referenced) ?
		foreign->referenced_table : foreign->foreign_table))
	{
		WSREP_DEBUG("pulling %s table into cache",
			    (referenced) ? "referenced" : "foreign");
		mutex_enter(&(dict_sys->mutex));
		if (referenced)
		{
			foreign->referenced_table =
				dict_table_get_low(
					foreign->referenced_table_name_lookup);
			if (foreign->referenced_table)
			{
				foreign->referenced_index =
					wsrep_dict_foreign_find_index(
						foreign->referenced_table, NULL,
						foreign->referenced_col_names,
						foreign->n_fields,
						foreign->foreign_index,
						TRUE, FALSE);
			}
		}
		else
		{
	  		foreign->foreign_table =
				dict_table_get_low(
					foreign->foreign_table_name_lookup);
			if (foreign->foreign_table)
			{
				foreign->foreign_index =
					wsrep_dict_foreign_find_index(
						foreign->foreign_table, NULL,
						foreign->foreign_col_names,
						foreign->n_fields,
						foreign->referenced_index,
						TRUE, FALSE);
			}
		}
		mutex_exit(&(dict_sys->mutex));
	}

	if (!((referenced) ?
		foreign->referenced_table : foreign->foreign_table))
	{
		WSREP_WARN("FK: %s missing in query: %s",
			   (!foreign->referenced_table) ?
			   "referenced table" : "foreign table",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void");
		return DB_ERROR;
	}
	byte  key[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
	ulint len = WSREP_MAX_SUPPORTED_KEY_LENGTH;

	dict_index_t *idx_target = (referenced) ?
		foreign->referenced_index : index;
	dict_index_t *idx = (referenced) ?
		UT_LIST_GET_FIRST(foreign->referenced_table->indexes) :
		UT_LIST_GET_FIRST(foreign->foreign_table->indexes);
	int i = 0;
	while (idx != NULL && idx != idx_target) {
		if (innobase_strcasecmp (idx->name, innobase_index_reserve_name) != 0) {
			i++;
		}
		idx = UT_LIST_GET_NEXT(indexes, idx);
	}
	ut_a(idx);
	key[0] = (char)i;

	rcode = wsrep_rec_get_foreign_key(
		&key[1], &len, rec, index, idx,
		wsrep_protocol_version > 1);
	if (rcode != DB_SUCCESS) {
		WSREP_ERROR(
			"FK key set failed: %d (%lu %s), index: %s %s, %s",
			rcode, referenced, wsrep_key_type_to_str(key_type),
			(index && index->name)       ? index->name :
				"void index",
			(index && index->table_name) ? index->table_name :
				"void table",
			wsrep_thd_query(thd));
		return DB_ERROR;
	}
	strncpy(cache_key,
		(wsrep_protocol_version > 1) ?
		((referenced) ?
			foreign->referenced_table->name :
			foreign->foreign_table->name) :
		foreign->foreign_table->name, sizeof(cache_key) - 1);
	cache_key_len = strlen(cache_key);
// #define WSREP_DEBUG_PRINT
#ifdef WSREP_DEBUG_PRINT
	ulint j;
	fprintf(stderr, "FK parent key, table: %s %s len: %lu ",
		cache_key, wsrep_key_type_to_str(key_type), len+1);
	for (j=0; j<len+1; j++) {
		fprintf(stderr, " %hhX, ", key[j]);
	}
	fprintf(stderr, "\n");
#endif
	char *p = strchr(cache_key, '/');
	if (p) {
		*p = '\0';
	} else {
		WSREP_WARN("unexpected foreign key table %s %s",
			   foreign->referenced_table->name,
			   foreign->foreign_table->name);
	}

	wsrep_buf_t wkey_part[3];
        wsrep_key_t wkey = {wkey_part, 3};
	if (!wsrep_prepare_key_for_innodb(
		(const uchar*)cache_key,
		cache_key_len +  1,
		(const uchar*)key, len+1,
		wkey_part,
		&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for cascaded FK: %s",
			   (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void");
		return DB_ERROR;
	}
	rcode = wsrep->append_key(
		wsrep,
		wsrep_ws_handle(thd, trx),
		&wkey,
		1,
		key_type,
		copy);
	if (rcode) {
		DBUG_PRINT("wsrep", ("row key failed: %d", rcode));
		WSREP_ERROR("Appending cascaded fk row key failed: %s, %d",
			    (wsrep_thd_query(thd)) ?
			     wsrep_thd_query(thd) : "void", rcode);
		return DB_ERROR;
	}

	return DB_SUCCESS;
}

static int
wsrep_append_key(
	THD		*thd,
	trx_t 		*trx,
	TABLE_SHARE 	*table_share,
	TABLE 		*table,
	const char*	key,
	uint16_t        key_len,
	wsrep_key_type	key_type	/*!< in: access type of this key
					(shared, exclusive, semi...) */
)
{
	DBUG_ENTER("wsrep_append_key");
	bool const copy = true;
#ifdef WSREP_DEBUG_PRINT
        if (wsrep_debug) {
	fprintf(stderr, "%s conn %ld, trx %llu, keylen %d, table %s\n SQL: %s ",
		wsrep_key_type_to_str(key_type),
		wsrep_thd_thread_id(thd), (long long)trx->id, key_len,
		table_share->table_name.str, wsrep_thd_query(thd));
            for (int i=0; i<key_len; i++) {
                    fprintf(stderr, "%hhX, ", key[i]);
            }
            fprintf(stderr, "\n");
        }
#endif
	wsrep_buf_t wkey_part[3];
	wsrep_key_t wkey = {wkey_part, 3};
	if (!wsrep_prepare_key_for_innodb(
			(const uchar*)table_share->table_cache_key.str,
			table_share->table_cache_key.length,
			(const uchar*)key, key_len,
			wkey_part,
			&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for: %s",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void");
		DBUG_RETURN(-1);
	}

	int rcode = wsrep->append_key(
				wsrep,
				wsrep_ws_handle(thd, trx),
				&wkey,
				1,
				key_type,
				copy);
	if (rcode) {
		DBUG_PRINT("wsrep", ("row key failed: %d", rcode));
		WSREP_WARN("Appending row key failed: %s, %d",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void", rcode);
		DBUG_RETURN(-1);
	}
	DBUG_RETURN(0);
}

extern void compute_md5_hash(char *digest, const char *buf, int len);
#define MD5_HASH compute_md5_hash

bool
wsrep_is_FK_index(dict_table_t* table,
                  dict_index_t* index)
{
	const dict_foreign_set*	fks = &table->referenced_set;

	/* Check for all FK references from other tables to the index. */
	for (dict_foreign_set::const_iterator it = fks->begin();
	     it != fks->end(); ++it) {

		dict_foreign_t*	foreign = *it;
		if (foreign->referenced_index == index) {
                	ut_ad(table == foreign->referenced_table);
			return true;
		}
        }
        return false;
}

int
ha_innobase::wsrep_append_keys(
/*===========================*/
	THD 		*thd,
	wsrep_key_type	key_type,	/*!< in: access type of this key
					(shared, exclusive, semi...) */
	const uchar*	record0,	/* in: row in MySQL format */
	const uchar*	record1)	/* in: row in MySQL format */
{
	int rcode;
	DBUG_ENTER("wsrep_append_keys");

	bool key_appended = false;
	trx_t *trx = thd_to_trx(thd);

        if (table_share && table_share->tmp_table  != NO_TMP_TABLE &&
               thd->lex->sql_command != SQLCOM_CREATE_TABLE) {
		WSREP_DEBUG("skipping tmp table DML: THD: %lu tmp: %d SQL: %s",
			    wsrep_thd_thread_id(thd),
			    table_share->tmp_table,
			    (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void");
		DBUG_RETURN(0);
	}

	if (wsrep_protocol_version == 0) {
		uint	len;
		char 	keyval[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
		char 	*key 		= &keyval[0];
		ibool    is_null;

		len = wsrep_store_key_val_for_row(
			thd, table, 0, key, WSREP_MAX_SUPPORTED_KEY_LENGTH, 
			record0, &is_null, prebuilt);

		if (!is_null) {
			rcode = wsrep_append_key(
				thd, trx, table_share, table, keyval,
				len, key_type);
			if (rcode) DBUG_RETURN(rcode);
		}
		else
		{
			WSREP_DEBUG("NULL key skipped (proto 0): %s",
				    wsrep_thd_query(thd));
		}
	} else {
		ut_a(table->s->keys <= 256);
		uint i;
		bool hasPK= false;

		for (i=0; i<table->s->keys; ++i) {
			KEY*  key_info	= table->key_info + i;
			if (key_info->flags & HA_NOSAME) {
				hasPK = true;
			}
		}

		for (i=0; i<table->s->keys; ++i) {
			uint  len0, len1;
			char  keyval0[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
			char  keyval1[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
			char* key0 		= &keyval0[1];
			char* key1 		= &keyval1[1];
			KEY*  key_info	= table->key_info + i;
			ibool is_null;

			dict_index_t* idx  = innobase_get_index(i);
			dict_table_t* tab  = (idx) ? idx->table : NULL;

			keyval0[0] = (char)i;
			keyval1[0] = (char)i;

			if (!tab) {
				WSREP_WARN("MySQL-InnoDB key mismatch %s %s",
					   table->s->table_name.str,
					   key_info->name);
			}

			/* !hasPK == table with no PK, 
                           must append all non-unique keys */
			if ((!hasPK && wsrep_certify_nonPK) ||
				key_info->flags & HA_NOSAME ||
			    ((tab && wsrep_is_FK_index(tab, idx)) ||
			     (!tab && referenced_by_foreign_key()))) {

				len0 = wsrep_store_key_val_for_row(
					thd, table, i, key0,
					WSREP_MAX_SUPPORTED_KEY_LENGTH,
					record0, &is_null, prebuilt);
				if (!is_null) {
					rcode = wsrep_append_key(
						thd, trx, table_share, table,
						keyval0, len0+1, key_type);
					if (rcode) DBUG_RETURN(rcode);

					if (key_info->flags & HA_NOSAME ||
					    key_type == WSREP_KEY_SHARED)
						key_appended = true;
				}
				else
				{
					WSREP_DEBUG("NULL key skipped: %s",
						    wsrep_thd_query(thd));
				}
				if (record1) {
					len1 = wsrep_store_key_val_for_row(
						thd, table, i, key1,
						WSREP_MAX_SUPPORTED_KEY_LENGTH,
						record1, &is_null, prebuilt);
					if (!is_null && (len0 != len1 || memcmp(key0, key1, len1))) {
						rcode = wsrep_append_key(
							thd, trx, table_share,
							table,
							keyval1, len1+1, key_type);
						if (rcode) DBUG_RETURN(rcode);
					}
				}
			}
		}
	}

	/* if no PK, calculate hash of full row, to be the key value */
	if (!key_appended && wsrep_certify_nonPK) {
		uchar digest[16];
		int rcode;

		wsrep_calc_row_hash(digest, record0, table, prebuilt, thd);
		if ((rcode = wsrep_append_key(thd, trx, table_share, table,
					      (const char*) digest, 16,
					      key_type))) {
			DBUG_RETURN(rcode);
		}

		if (record1) {
			wsrep_calc_row_hash(
				digest, record1, table, prebuilt, thd);
			if ((rcode = wsrep_append_key(thd, trx, table_share,
						      table,
						      (const char*) digest,
						      16, key_type))) {
				DBUG_RETURN(rcode);
			}
		}
		DBUG_RETURN(0);
	}

	/* If certification of table with non-PK is blocked by setting
	relevant configuration option wsrep_certify_nonPK = OFF/0 then
	ensure that thd->wsrep_ws_handle->trx_id = WSREP_UNDEFINED_TRX_ID.
	If not then make sure you set it to WSREP_UNDEFINED_TRX_ID.
	But what may cause trx_id to set if append-key is blocked ?
	CREATE TABLE ... SELECT statement will cause a fake_trx_id to set
	while processing SELECT statement. */
	if (!key_appended && !wsrep_certify_nonPK) {
		wsrep_ws_handle_for_trx(
			wsrep_thd_ws_handle(thd), WSREP_UNDEFINED_TRX_ID);
	}

	DBUG_RETURN(0);
}
#endif
/*********************************************************************//**
Stores a reference to the current row to 'ref' field of the handle. Note
that in the case where we have generated the clustered index for the
table, the function parameter is illogical: we MUST ASSUME that 'record'
is the current 'position' of the handle, because if row ref is actually
the row id internally generated in InnoDB, then 'record' does not contain
it. We just guess that the row id must be for the record where the handle
was positioned the last time. */
UNIV_INTERN
void
ha_innobase::position(
/*==================*/
	const uchar*	record)	/*!< in: row in MySQL format */
{
	uint		len;

	ut_a(prebuilt->trx == thd_to_trx(ha_thd()));

	if (prebuilt->clust_index_was_generated) {
		/* No primary key was defined for the table and we
		generated the clustered index from row id: the
		row reference will be the row id, not any key value
		that MySQL knows of */

		len = DATA_ROW_ID_LEN;

		memcpy(ref, prebuilt->row_id, len);
	} else {
		len = store_key_val_for_row(primary_key, (char*) ref,
							 ref_length, record);
	}

	/* We assume that the 'ref' value len is always fixed for the same
	table. */

	if (len != ref_length) {
		sql_print_error("Stored ref len is %lu, but table ref len is "
				"%lu", (ulong) len, (ulong) ref_length);
	}
}

/*****************************************************************//**
Check whether there exist a column named as "FTS_DOC_ID", which is
reserved for InnoDB FTS Doc ID
@return true if there exist a "FTS_DOC_ID" column */
static
bool
create_table_check_doc_id_col(
/*==========================*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	ulint*		doc_id_col)	/*!< out: Doc ID column number if
					there exist a FTS_DOC_ID column,
					ULINT_UNDEFINED if column is of the
					wrong type/name/size */
{
	for (ulint i = 0; i < form->s->fields; i++) {
		const Field*	field;
		ulint		col_type;
		ulint		col_len;
		ulint		unsigned_type;

		field = form->field[i];

		col_type = get_innobase_type_from_mysql_type(&unsigned_type,
							     field);

		col_len = field->pack_length();

		if (innobase_strcasecmp(field->field_name,
					FTS_DOC_ID_COL_NAME) == 0) {

			/* Note the name is case sensitive due to
			our internal query parser */
			if (col_type == DATA_INT
			    && !field->real_maybe_null()
			    && col_len == sizeof(doc_id_t)
			    && (strcmp(field->field_name,
				      FTS_DOC_ID_COL_NAME) == 0)) {
				*doc_id_col = i;
			} else {
				push_warning_printf(
					trx->mysql_thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: FTS_DOC_ID column must be "
					"of BIGINT NOT NULL type, and named "
					"in all capitalized characters");
				my_error(ER_WRONG_COLUMN_NAME, MYF(0),
					 field->field_name);
				*doc_id_col = ULINT_UNDEFINED;
			}

			return(true);
		}
	}

	return(false);
}

/*****************************************************************//**
Creates a table definition to an InnoDB database. */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
int
create_table_def(
/*=============*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	const char*	table_name,	/*!< in: table name */
	const char*	temp_path,	/*!< in: if this is a table explicitly
					created by the user with the
					TEMPORARY keyword, then this
					parameter is the dir path where the
					table should be placed if we create
					an .ibd file for it (no .ibd extension
					in the path, though). Otherwise this
					is a zero length-string */
	const char*	remote_path,	/*!< in: Remote path or zero length-string */
	ulint		flags,		/*!< in: table flags */
	ulint		flags2)		/*!< in: table flags2 */
{
	THD*		thd = trx->mysql_thd;
	dict_table_t*	table;
	ulint		n_cols;
	dberr_t		err;
	ulint		col_type;
	ulint		col_len;
	ulint		nulls_allowed;
	ulint		unsigned_type;
	ulint		binary_type;
	ulint		long_true_varchar;
	ulint		compressed;
	ulint		charset_no;
	ulint		i;
	ulint		doc_id_col = 0;
	ibool		has_doc_id_col = FALSE;
	mem_heap_t*	heap;

	DBUG_ENTER("create_table_def");
	DBUG_PRINT("enter", ("table_name: %s", table_name));

	DBUG_ASSERT(thd != NULL);

	/* MySQL does the name length check. But we do additional check
	on the name length here */
	const size_t	table_name_len = strlen(table_name);
	if (table_name_len > MAX_FULL_NAME_LEN) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table Name or Database Name is too long");

		DBUG_RETURN(ER_TABLE_NAME);
	}

	if (table_name[table_name_len - 1] == '/') {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table name is empty");

		DBUG_RETURN(ER_WRONG_TABLE_NAME);
	}

	n_cols = form->s->fields;

	/* Check whether there already exists a FTS_DOC_ID column */
	if (create_table_check_doc_id_col(trx, form, &doc_id_col)){

		/* Raise error if the Doc ID column is of wrong type or name */
		if (doc_id_col == ULINT_UNDEFINED) {
			trx_commit_for_mysql(trx);

			err = DB_ERROR;
			goto error_ret;
		} else {
			has_doc_id_col = TRUE;
		}
	}

	/* We pass 0 as the space id, and determine at a lower level the space
	id where to store the table */

	if (flags2 & DICT_TF2_FTS) {
		/* Adjust for the FTS hidden field */
		if (!has_doc_id_col) {
			table = dict_mem_table_create(table_name, 0, n_cols + 1,
						      flags, flags2);

			/* Set the hidden doc_id column. */
			table->fts->doc_col = n_cols;
		} else {
			table = dict_mem_table_create(table_name, 0, n_cols,
						      flags, flags2);
			table->fts->doc_col = doc_id_col;
		}
	} else {
		table = dict_mem_table_create(table_name, 0, n_cols,
					      flags, flags2);
	}

	if (flags2 & DICT_TF2_TEMPORARY) {
		ut_a(strlen(temp_path));
		table->dir_path_of_temp_table =
			mem_heap_strdup(table->heap, temp_path);
	}

	if (DICT_TF_HAS_DATA_DIR(flags)) {
		ut_a(strlen(remote_path));
		table->data_dir_path = mem_heap_strdup(table->heap, remote_path);
	} else {
		table->data_dir_path = NULL;
	}
	heap = mem_heap_create(1000);

	for (i = 0; i < n_cols; i++) {
		Field*	field = form->field[i];

		col_type = get_innobase_type_from_mysql_type(&unsigned_type,
							     field);

		if (!col_type) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_CANT_CREATE_TABLE,
				"Error creating table '%s' with "
				"column '%s'. Please check its "
				"column type and try to re-create "
				"the table with an appropriate "
				"column type.",
				table->name, field->field_name);
			goto err_col;
		}

		nulls_allowed = field->real_maybe_null() ? 0 : DATA_NOT_NULL;
		binary_type = field->binary() ? DATA_BINARY_TYPE : 0;

		charset_no = 0;

		if (dtype_is_string_type(col_type)) {

			charset_no = (ulint) field->charset()->number;

			if (UNIV_UNLIKELY(charset_no > MAX_CHAR_COLL_NUM)) {
				/* in data0type.h we assume that the
				number fits in one byte in prtype */
				push_warning_printf(
					thd, Sql_condition::WARN_LEVEL_WARN,
					ER_CANT_CREATE_TABLE,
					"In InnoDB, charset-collation codes"
					" must be below 256."
					" Unsupported code %lu.",
					(ulong) charset_no);
				mem_heap_free(heap);
				DBUG_RETURN(ER_CANT_CREATE_TABLE);
			}
		}

		/* we assume in dtype_form_prtype() that this fits in
		two bytes */
		ut_a(static_cast<uint>(field->type()) <= MAX_CHAR_COLL_NUM);
		col_len = field->pack_length();

		/* The MySQL pack length contains 1 or 2 bytes length field
		for a true VARCHAR. Let us subtract that, so that the InnoDB
		column length in the InnoDB data dictionary is the real
		maximum byte length of the actual data. */

		long_true_varchar = 0;

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			col_len -= ((Field_varstring*) field)->length_bytes;

			if (((Field_varstring*) field)->length_bytes == 2) {
				long_true_varchar = DATA_LONG_TRUE_VARCHAR;
			}
		}

		/* Check if the the field has COMPRESSED attribute */
		compressed = 0;
		if (field->column_format() ==
			COLUMN_FORMAT_TYPE_COMPRESSED) {
			compressed = DATA_COMPRESSED;
		}

		/* First check whether the column to be added has a
		system reserved name. */
		if (dict_col_name_is_reserved(field->field_name)){
			my_error(ER_WRONG_COLUMN_NAME, MYF(0),
				 field->field_name);
err_col:
			dict_mem_table_free(table);
			mem_heap_free(heap);
			trx_commit_for_mysql(trx);

			err = DB_ERROR;
			goto error_ret;
		}

		dict_mem_table_add_col(table, heap,
			field->field_name,
			col_type,
			dtype_form_prtype(
				(ulint) field->type()
				| nulls_allowed | unsigned_type
				| binary_type | long_true_varchar
				| compressed,
				charset_no),
			col_len);
	}

	/* Add the FTS doc_id hidden column. */
	if (flags2 & DICT_TF2_FTS && !has_doc_id_col) {
		fts_add_doc_id_column(table, heap);
	}

	err = row_create_table_for_mysql(table, trx, false);

	mem_heap_free(heap);

	DBUG_EXECUTE_IF("ib_create_err_tablespace_exist",
			err = DB_TABLESPACE_EXISTS;);

	if (err == DB_DUPLICATE_KEY || err == DB_TABLESPACE_EXISTS) {
		char display_name[FN_REFLEN];
		char* buf_end = innobase_convert_identifier(
			display_name, sizeof(display_name) - 1,
			table_name, strlen(table_name),
			thd, TRUE);

		*buf_end = '\0';

		my_error(err == DB_DUPLICATE_KEY
			 ? ER_TABLE_EXISTS_ERROR
			 : ER_TABLESPACE_EXISTS, MYF(0), display_name);
	}

	if (err == DB_SUCCESS && (flags2 & DICT_TF2_FTS)) {
		fts_optimize_add_table(table);
	}

error_ret:
	DBUG_RETURN(convert_error_code_to_mysql(err, flags, thd));
}

/*****************************************************************//**
Creates an index in an InnoDB database. */
static
int
create_index(
/*=========*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	ulint		flags,		/*!< in: InnoDB table flags */
	const char*	table_name,	/*!< in: table name */
	uint		key_num)	/*!< in: index number */
{
	dict_index_t*	index;
	int		error;
	const KEY*	key;
	ulint		ind_type;
	ulint*		field_lengths;

	DBUG_ENTER("create_index");

	key = form->key_info + key_num;

	/* Assert that "GEN_CLUST_INDEX" cannot be used as non-primary index */
	ut_a(innobase_strcasecmp(key->name, innobase_index_reserve_name) != 0);

	if (key->flags & HA_FULLTEXT) {
		index = dict_mem_index_create(table_name, key->name, 0,
					      DICT_FTS,
					      key->user_defined_key_parts);

		for (ulint i = 0; i < key->user_defined_key_parts; i++) {
			KEY_PART_INFO*	key_part = key->key_part + i;
			dict_mem_index_add_field(
				index, key_part->field->field_name, 0);
		}

		DBUG_RETURN(convert_error_code_to_mysql(
				    row_create_index_for_mysql(
					    index, trx, NULL),
				    flags, NULL));

	}

	ind_type = 0;

	if (key_num == form->s->primary_key) {
		ind_type |= DICT_CLUSTERED;
	}

	if (key->flags & HA_NOSAME) {
		ind_type |= DICT_UNIQUE;
	}

	field_lengths = (ulint*) my_malloc(
		key->user_defined_key_parts * sizeof *
				field_lengths, MYF(MY_FAE));

	/* We pass 0 as the space id, and determine at a lower level the space
	id where to store the table */

	index = dict_mem_index_create(table_name, key->name, 0,
				      ind_type, key->user_defined_key_parts);

	for (ulint i = 0; i < key->user_defined_key_parts; i++) {
		KEY_PART_INFO*	key_part = key->key_part + i;
		ulint		prefix_len;
		ulint		col_type;
		ulint		is_unsigned;


		/* (The flag HA_PART_KEY_SEG denotes in MySQL a
		column prefix field in an index: we only store a
		specified number of first bytes of the column to
		the index field.) The flag does not seem to be
		properly set by MySQL. Let us fall back on testing
		the length of the key part versus the column. */

		Field*	field = NULL;

		for (ulint j = 0; j < form->s->fields; j++) {

			field = form->field[j];

			if (0 == innobase_strcasecmp(
				    field->field_name,
				    key_part->field->field_name)) {
				/* Found the corresponding column */

				goto found;
			}
		}

		ut_error;
found:
		col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, key_part->field);

		if (DATA_BLOB == col_type
		    || (key_part->length < field->pack_length()
			&& field->type() != MYSQL_TYPE_VARCHAR)
		    || (field->type() == MYSQL_TYPE_VARCHAR
			&& key_part->length < field->pack_length()
			- ((Field_varstring*) field)->length_bytes)) {

			switch (col_type) {
			default:
				prefix_len = key_part->length;
				break;
			case DATA_INT:
			case DATA_FLOAT:
			case DATA_DOUBLE:
			case DATA_DECIMAL:
				sql_print_error(
					"MySQL is trying to create a column "
					"prefix index field, on an "
					"inappropriate data type. Table "
					"name %s, column name %s.",
					table_name,
					key_part->field->field_name);

				prefix_len = 0;
			}
		} else {
			prefix_len = 0;
		}

		field_lengths[i] = key_part->length;

		dict_mem_index_add_field(
			index, key_part->field->field_name, prefix_len);
	}

	ut_ad(key->flags & HA_FULLTEXT || !(index->type & DICT_FTS));

	/* Even though we've defined max_supported_key_part_length, we
	still do our own checking using field_lengths to be absolutely
	sure we don't create too long indexes. */

	error = convert_error_code_to_mysql(
		row_create_index_for_mysql(index, trx, field_lengths),
		flags, NULL);

	my_free(field_lengths);

	DBUG_RETURN(error);
}

/*****************************************************************//**
Creates an index to an InnoDB table when the user has defined no
primary index. */
static
int
create_clustered_index_when_no_primary(
/*===================================*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	ulint		flags,		/*!< in: InnoDB table flags */
	const char*	table_name)	/*!< in: table name */
{
	dict_index_t*	index;
	dberr_t		error;

	/* We pass 0 as the space id, and determine at a lower level the space
	id where to store the table */
	index = dict_mem_index_create(table_name,
				      innobase_index_reserve_name,
				      0, DICT_CLUSTERED, 0);

	error = row_create_index_for_mysql(index, trx, NULL);

	return(convert_error_code_to_mysql(error, flags, NULL));
}

/*****************************************************************//**
Return a display name for the row format
@return row format name */
UNIV_INTERN
const char*
get_row_format_name(
/*================*/
	enum row_type	row_format)		/*!< in: Row Format */
{
	switch (row_format) {
	case ROW_TYPE_COMPACT:
		return("COMPACT");
	case ROW_TYPE_COMPRESSED:
		return("COMPRESSED");
	case ROW_TYPE_DYNAMIC:
		return("DYNAMIC");
	case ROW_TYPE_REDUNDANT:
		return("REDUNDANT");
	case ROW_TYPE_DEFAULT:
		return("DEFAULT");
	case ROW_TYPE_FIXED:
		return("FIXED");
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
	default:
		break;
	}
	return("NOT USED");
}

/** If file-per-table is missing, issue warning and set ret false */
#define CHECK_ERROR_ROW_TYPE_NEEDS_FILE_PER_TABLE(use_tablespace)\
	if (!use_tablespace) {					\
		push_warning_printf(				\
			thd, Sql_condition::WARN_LEVEL_WARN,	\
			ER_ILLEGAL_HA_CREATE_OPTION,		\
			"InnoDB: ROW_FORMAT=%s requires"	\
			" innodb_file_per_table.",		\
			get_row_format_name(row_format));	\
		ret = "ROW_FORMAT";					\
	}

/** If file-format is Antelope, issue warning and set ret false */
#define CHECK_ERROR_ROW_TYPE_NEEDS_GT_ANTELOPE			\
	if (srv_file_format < UNIV_FORMAT_B) {		\
		push_warning_printf(				\
			thd, Sql_condition::WARN_LEVEL_WARN,	\
			ER_ILLEGAL_HA_CREATE_OPTION,		\
			"InnoDB: ROW_FORMAT=%s requires"	\
			" innodb_file_format > Antelope.",	\
			get_row_format_name(row_format));	\
		ret = "ROW_FORMAT";				\
	}


/*****************************************************************//**
Validates the create options. We may build on this function
in future. For now, it checks two specifiers:
KEY_BLOCK_SIZE and ROW_FORMAT
If innodb_strict_mode is not set then this function is a no-op
@return	NULL if valid, string if not. */
UNIV_INTERN
const char*
create_options_are_invalid(
/*=======================*/
	THD*		thd,		/*!< in: connection thread. */
	TABLE*		form,		/*!< in: information on table
					columns and indexes */
	HA_CREATE_INFO*	create_info,	/*!< in: create info. */
	bool		use_tablespace)	/*!< in: srv_file_per_table */
{
	ibool	kbs_specified	= FALSE;
	const char*	ret	= NULL;
	enum row_type	row_format	= form->s->row_type;

	ut_ad(thd != NULL);

	/* If innodb_strict_mode is not set don't do any validation. */
	if (!(THDVAR(thd, strict_mode))) {
		return(NULL);
	}

	ut_ad(form != NULL);
	ut_ad(create_info != NULL);

	/* First check if a non-zero KEY_BLOCK_SIZE was specified. */
	if (create_info->key_block_size) {
		kbs_specified = TRUE;
		switch (create_info->key_block_size) {
			ulint	kbs_max;
		case 1:
		case 2:
		case 4:
		case 8:
		case 16:
			/* Valid KEY_BLOCK_SIZE, check its dependencies. */
			if (!use_tablespace) {
				push_warning(
					thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE requires"
					" innodb_file_per_table.");
				ret = "KEY_BLOCK_SIZE";
			}
			if (srv_file_format < UNIV_FORMAT_B) {
				push_warning(
					thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE requires"
					" innodb_file_format > Antelope.");
				ret = "KEY_BLOCK_SIZE";
			}

			/* The maximum KEY_BLOCK_SIZE (KBS) is 16. But if
			UNIV_PAGE_SIZE is smaller than 16k, the maximum
			KBS is also smaller. */
			kbs_max = ut_min(
				1 << (UNIV_PAGE_SSIZE_MAX - 1),
				1 << (PAGE_ZIP_SSIZE_MAX - 1));
			if (create_info->key_block_size > kbs_max) {
				push_warning_printf(
					thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE=%ld"
					" cannot be larger than %ld.",
					create_info->key_block_size,
					kbs_max);
				ret = "KEY_BLOCK_SIZE";
			}
			break;
		default:
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: invalid KEY_BLOCK_SIZE = %lu."
				" Valid values are [1, 2, 4, 8, 16]",
				create_info->key_block_size);
			ret = "KEY_BLOCK_SIZE";
			break;
		}
	}

	/* Check for a valid Innodb ROW_FORMAT specifier and
	other incompatibilities. */
	switch (row_format) {
	case ROW_TYPE_COMPRESSED:
		CHECK_ERROR_ROW_TYPE_NEEDS_FILE_PER_TABLE(use_tablespace);
		CHECK_ERROR_ROW_TYPE_NEEDS_GT_ANTELOPE;
		break;
	case ROW_TYPE_DYNAMIC:
		CHECK_ERROR_ROW_TYPE_NEEDS_FILE_PER_TABLE(use_tablespace);
		CHECK_ERROR_ROW_TYPE_NEEDS_GT_ANTELOPE;
		// fallthrough
		// since dynamic also shuns KBS
	case ROW_TYPE_COMPACT:
	case ROW_TYPE_REDUNDANT:
		if (kbs_specified) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: cannot specify ROW_FORMAT = %s"
				" with KEY_BLOCK_SIZE.",
				get_row_format_name(row_format));
			ret = "KEY_BLOCK_SIZE";
		}
		break;
	case ROW_TYPE_DEFAULT:
		break;
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
	default:
		push_warning(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,		\
			"InnoDB: invalid ROW_FORMAT specifier.");
		ret = "ROW_TYPE";
		break;
	}

	/* Use DATA DIRECTORY only with file-per-table. */
	if (create_info->data_file_name && !use_tablespace) {
		push_warning(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY requires"
			" innodb_file_per_table.");
		ret = "DATA DIRECTORY";
	}

	/* Do not use DATA DIRECTORY with TEMPORARY TABLE. */
	if (create_info->data_file_name
	    && create_info->options & HA_LEX_CREATE_TMP_TABLE) {
		push_warning(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY cannot be used"
			" for TEMPORARY tables.");
		ret = "DATA DIRECTORY";
	}

	/* Do not allow INDEX_DIRECTORY */
	if (create_info->index_file_name) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: INDEX DIRECTORY is not supported");
		ret = "INDEX DIRECTORY";
	}

	return(ret);
}

/*****************************************************************//**
Update create_info.  Used in SHOW CREATE TABLE et al. */
UNIV_INTERN
void
ha_innobase::update_create_info(
/*============================*/
	HA_CREATE_INFO*	create_info)	/*!< in/out: create info */
{
	if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
		ha_innobase::info(HA_STATUS_AUTO);
		create_info->auto_increment_value = stats.auto_increment_value;
	}

	/* Update the DATA DIRECTORY name from SYS_DATAFILES. */
	dict_get_and_save_data_dir_path(prebuilt->table, false);

	if (prebuilt->table->data_dir_path) {
		create_info->data_file_name = prebuilt->table->data_dir_path;
	}
}

/*****************************************************************//**
Initialize the table FTS stopword list
@return TRUE if success */
UNIV_INTERN
ibool
innobase_fts_load_stopword(
/*=======================*/
	dict_table_t*	table,	/*!< in: Table has the FTS */
	trx_t*		trx,	/*!< in: transaction */
	THD*		thd)	/*!< in: current thread */
{
	return(fts_load_stopword(table, trx,
				 innobase_server_stopword_table,
				 THDVAR(thd, ft_user_stopword_table),
				 THDVAR(thd, ft_enable_stopword), FALSE));
}

/*****************************************************************//**
Parses the table name into normal name and either temp path or remote path
if needed.
@return	0 if successful, otherwise, error number */
UNIV_INTERN
int
ha_innobase::parse_table_name(
/*==========================*/
	const char*	name,		/*!< in/out: table name provided*/
	HA_CREATE_INFO*	create_info,	/*!< in: more information of the
					created table, contains also the
					create statement string */
	ulint		flags,		/*!< in: flags*/
	ulint		flags2,		/*!< in: flags2*/
	char*		norm_name,	/*!< out: normalized table name */
	char*		temp_path,	/*!< out: absolute path of table */
	char*		remote_path)	/*!< out: remote path of table */
{
	THD*		thd = ha_thd();
	bool		use_tablespace = flags2 & DICT_TF2_USE_TABLESPACE;
	DBUG_ENTER("ha_innobase::parse_table_name");

#ifdef __WIN__
	/* Names passed in from server are in two formats:
	1. <database_name>/<table_name>: for normal table creation
	2. full path: for temp table creation, or DATA DIRECTORY.

	When srv_file_per_table is on and mysqld_embedded is off,
	check for full path pattern, i.e.
	X:\dir\...,		X is a driver letter, or
	\\dir1\dir2\...,	UNC path
	returns error if it is in full path format, but not creating a temp.
	table. Currently InnoDB does not support symbolic link on Windows. */

	if (use_tablespace
	    && !mysqld_embedded
	    && !(create_info->options & HA_LEX_CREATE_TMP_TABLE)) {

		if ((name[1] == ':')
		    || (name[0] == '\\' && name[1] == '\\')) {
			sql_print_error("Cannot create table %s\n", name);
			DBUG_RETURN(HA_ERR_GENERIC);
		}
	}
#endif

	normalize_table_name(norm_name, name);
	temp_path[0] = '\0';
	remote_path[0] = '\0';

	/* A full path is used for TEMPORARY TABLE and DATA DIRECTORY.
	In the case of;
	  CREATE TEMPORARY TABLE ... DATA DIRECTORY={path} ... ;
	We ignore the DATA DIRECTORY. */
	if (create_info->options & HA_LEX_CREATE_TMP_TABLE) {
		strncpy(temp_path, name, FN_REFLEN - 1);
	}

	if (create_info->data_file_name) {
		bool ignore = false;

		/* Use DATA DIRECTORY only with file-per-table. */
		if (!use_tablespace) {
			push_warning(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: DATA DIRECTORY requires"
				" innodb_file_per_table.");
			ignore = true;
		}

		/* Do not use DATA DIRECTORY with TEMPORARY TABLE. */
		if (create_info->options & HA_LEX_CREATE_TMP_TABLE) {
			push_warning(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: DATA DIRECTORY cannot be"
				" used for TEMPORARY tables.");
			ignore = true;
		}

		if (ignore) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				WARN_OPTION_IGNORED,
				ER_DEFAULT(WARN_OPTION_IGNORED),
				"DATA DIRECTORY");
		} else {
			strncpy(remote_path, create_info->data_file_name,
				FN_REFLEN - 1);
		}
	}

	if (create_info->index_file_name) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			WARN_OPTION_IGNORED,
			ER_DEFAULT(WARN_OPTION_IGNORED),
			"INDEX DIRECTORY");
	}

	DBUG_RETURN(0);
}

/*****************************************************************//**
Determines InnoDB table flags.
@retval true if successful, false if error */
UNIV_INTERN
bool
innobase_table_flags(
/*=================*/
	const TABLE*		form,		/*!< in: table */
	const HA_CREATE_INFO*	create_info,	/*!< in: information
						on table columns and indexes */
	THD*			thd,		/*!< in: connection */
	bool			use_tablespace,	/*!< in: whether to create
						outside system tablespace */
	ulint*			flags,		/*!< out: DICT_TF flags */
	ulint*			flags2)		/*!< out: DICT_TF2 flags */
{
	DBUG_ENTER("innobase_table_flags");

	const char*	fts_doc_id_index_bad = NULL;
	bool		zip_allowed = true;
	ulint		zip_ssize = 0;
	enum row_type	row_format;
	rec_format_t	innodb_row_format = REC_FORMAT_COMPACT;
	bool		use_data_dir;

	/* Cache the value of innodb_file_format, in case it is
	modified by another thread while the table is being created. */
	const ulint	file_format_allowed = srv_file_format;

	*flags = 0;
	*flags2 = 0;

	/* Check if there are any FTS indexes defined on this table. */
	for (uint i = 0; i < form->s->keys; i++) {
		const KEY*	key = &form->key_info[i];

		if (key->flags & HA_FULLTEXT) {
			*flags2 |= DICT_TF2_FTS;

			/* We don't support FTS indexes in temporary
			tables. */
			if (create_info->options & HA_LEX_CREATE_TMP_TABLE) {

				my_error(ER_INNODB_NO_FT_TEMP_TABLE, MYF(0));
				DBUG_RETURN(false);
			}

			if (key->flags & HA_USES_PARSER) {
				my_error(ER_INNODB_NO_FT_USES_PARSER, MYF(0));
                                DBUG_RETURN(false);
			}

			if (fts_doc_id_index_bad) {
				goto index_bad;
			}
		}

		if (innobase_strcasecmp(key->name, FTS_DOC_ID_INDEX_NAME)) {
			continue;
		}

		/* Do a pre-check on FTS DOC ID index */
		if (!(key->flags & HA_NOSAME)
		    || strcmp(key->name, FTS_DOC_ID_INDEX_NAME)
		    || strcmp(key->key_part[0].field->field_name,
			      FTS_DOC_ID_COL_NAME)) {
			fts_doc_id_index_bad = key->name;
		}

		if (fts_doc_id_index_bad && (*flags2 & DICT_TF2_FTS)) {
index_bad:
			my_error(ER_INNODB_FT_WRONG_DOCID_INDEX, MYF(0),
				 fts_doc_id_index_bad);
			DBUG_RETURN(false);
		}
	}

	if (create_info->key_block_size) {
		/* The requested compressed page size (key_block_size)
		is given in kilobytes. If it is a valid number, store
		that value as the number of log2 shifts from 512 in
		zip_ssize. Zero means it is not compressed. */
		ulint zssize;		/* Zip Shift Size */
		ulint kbsize;		/* Key Block Size */
		for (zssize = kbsize = 1;
		     zssize <= ut_min(UNIV_PAGE_SSIZE_MAX,
				      PAGE_ZIP_SSIZE_MAX);
		     zssize++, kbsize <<= 1) {
			if (kbsize == create_info->key_block_size) {
				zip_ssize = zssize;
				break;
			}
		}

		/* Make sure compressed row format is allowed. */
		if (!use_tablespace) {
			push_warning(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE requires"
				" innodb_file_per_table.");
			zip_allowed = FALSE;
		}

		if (file_format_allowed < UNIV_FORMAT_B) {
			push_warning(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE requires"
				" innodb_file_format > Antelope.");
			zip_allowed = FALSE;
		}

		if (!zip_allowed
		    || zssize > ut_min(UNIV_PAGE_SSIZE_MAX,
				       PAGE_ZIP_SSIZE_MAX)) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%lu.",
				create_info->key_block_size);
		}
	}

	row_format = form->s->row_type;

	if (zip_ssize && zip_allowed) {
		/* if ROW_FORMAT is set to default,
		automatically change it to COMPRESSED.*/
		if (row_format == ROW_TYPE_DEFAULT) {
			row_format = ROW_TYPE_COMPRESSED;
		} else if (row_format != ROW_TYPE_COMPRESSED) {
			/* ROW_FORMAT other than COMPRESSED
			ignores KEY_BLOCK_SIZE.  It does not
			make sense to reject conflicting
			KEY_BLOCK_SIZE and ROW_FORMAT, because
			such combinations can be obtained
			with ALTER TABLE anyway. */
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%lu"
				" unless ROW_FORMAT=COMPRESSED.",
				create_info->key_block_size);
			zip_allowed = FALSE;
		}
	} else {
		/* zip_ssize == 0 means no KEY_BLOCK_SIZE.*/
		if (row_format == ROW_TYPE_COMPRESSED && zip_allowed) {
			/* ROW_FORMAT=COMPRESSED without KEY_BLOCK_SIZE
			implies half the maximum KEY_BLOCK_SIZE(*1k) or
			UNIV_PAGE_SIZE, whichever is less. */
			zip_ssize = ut_min(UNIV_PAGE_SSIZE_MAX,
					   PAGE_ZIP_SSIZE_MAX) - 1;
		}
	}

	/* Validate the row format.  Correct it if necessary */
	switch (row_format) {
	case ROW_TYPE_REDUNDANT:
		innodb_row_format = REC_FORMAT_REDUNDANT;
		break;

	case ROW_TYPE_COMPRESSED:
	case ROW_TYPE_DYNAMIC:
		if (!use_tablespace) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s requires"
				" innodb_file_per_table.",
				get_row_format_name(row_format));
		} else if (file_format_allowed == UNIV_FORMAT_A) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s requires"
				" innodb_file_format > Antelope.",
				get_row_format_name(row_format));
		} else {
			innodb_row_format = (row_format == ROW_TYPE_DYNAMIC
					     ? REC_FORMAT_DYNAMIC
					     : REC_FORMAT_COMPRESSED);
			break;
		}
		zip_allowed = FALSE;
		// fallthrough
		// to set row_format = COMPACT
	case ROW_TYPE_NOT_USED:
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
	default:
		push_warning(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: assuming ROW_FORMAT=COMPACT.");
		// fallthrough
	case ROW_TYPE_DEFAULT:
		/* If we fell through, set row format to Compact. */
		row_format = ROW_TYPE_COMPACT;
		// fallthrough
	case ROW_TYPE_COMPACT:
		break;
	}

	/* Set the table flags */
	if (!zip_allowed) {
		zip_ssize = 0;
	}

	use_data_dir = use_tablespace
		       && ((create_info->data_file_name != NULL)
		       && !(create_info->options & HA_LEX_CREATE_TMP_TABLE));

	dict_tf_set(flags, innodb_row_format, zip_ssize, use_data_dir);

	if (create_info->options & HA_LEX_CREATE_TMP_TABLE) {
		*flags2 |= DICT_TF2_TEMPORARY;
	}

	if (use_tablespace) {
		*flags2 |= DICT_TF2_USE_TABLESPACE;
	}

	/* Set the flags2 when create table or alter tables */
	*flags2 |= DICT_TF2_FTS_AUX_HEX_NAME;
	DBUG_EXECUTE_IF("innodb_test_wrong_fts_aux_table_name",
			*flags2 &= ~DICT_TF2_FTS_AUX_HEX_NAME;);

	DBUG_RETURN(true);
}

/*****************************************************************//**
Creates a new table to an InnoDB database.
@return	error number */
UNIV_INTERN
int
ha_innobase::create(
/*================*/
	const char*	name,		/*!< in: table name */
	TABLE*		form,		/*!< in: information on table
					columns and indexes */
	HA_CREATE_INFO*	create_info)	/*!< in: more information of the
					created table, contains also the
					create statement string */
{
	int		error;
	trx_t*		parent_trx;
	trx_t*		trx;
	int		primary_key_no;
	uint		i;
	char		norm_name[FN_REFLEN];	/* {database}/{tablename} */
	char		temp_path[FN_REFLEN];	/* absolute path of temp frm */
	char		remote_path[FN_REFLEN];	/* absolute path of table */
	THD*		thd = ha_thd();
	ib_int64_t	auto_inc_value;

	/* Cache the global variable "srv_file_per_table" to a local
	variable before using it. Note that "srv_file_per_table"
	is not under dict_sys mutex protection, and could be changed
	while creating the table. So we read the current value here
	and make all further decisions based on this. */
	bool		use_tablespace = srv_file_per_table;

	/* Zip Shift Size - log2 - 9 of compressed page size,
	zero for uncompressed */
	ulint		flags;
	ulint		flags2;
	dict_table_t*	innobase_table = NULL;

	const char*	stmt;
	size_t		stmt_len;

	mem_heap_t*	heap = 0;
	ulint*		zip_dict_ids = 0;
	const char*	err_zip_dict_name = 0;

	DBUG_ENTER("ha_innobase::create");

	DBUG_ASSERT(thd != NULL);
	DBUG_ASSERT(create_info != NULL);

	if (form->s->fields > REC_MAX_N_USER_FIELDS) {
		DBUG_RETURN(HA_ERR_TOO_MANY_FIELDS);
	} else if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_INNODB_READ_ONLY);
	}

	/* Create the table definition in InnoDB */

	/* Validate create options if innodb_strict_mode is set. */
	if (create_options_are_invalid(
			thd, form, create_info, use_tablespace)) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	if (!innobase_table_flags(form, create_info,
				  thd, use_tablespace,
				  &flags, &flags2)) {
		DBUG_RETURN(-1);
	}

	error = parse_table_name(name, create_info, flags, flags2,
				 norm_name, temp_path, remote_path);
	if (error) {
		DBUG_RETURN(error);
	}

	/* Look for a primary key */
	primary_key_no = (form->s->primary_key != MAX_KEY ?
			  (int) form->s->primary_key :
			  -1);

	/* Our function innobase_get_mysql_key_number_for_index assumes
	the primary key is always number 0, if it exists */
	ut_a(primary_key_no == -1 || primary_key_no == 0);

	/* Check for name conflicts (with reserved name) for
	any user indices to be created. */
	if (innobase_index_name_is_reserved(thd, form->key_info,
					    form->s->keys)) {
		DBUG_RETURN(-1);
	}

	if (row_is_magic_monitor_table(norm_name)) {
		push_warning_printf(thd,
				    Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_WRONG_COMMAND,
				    "Using the table name %s to enable "
				    "diagnostic output is deprecated "
				    "and may be removed in future releases. "
				    "Use INFORMATION_SCHEMA or "
				    "PERFORMANCE_SCHEMA tables or "
				    "SET GLOBAL innodb_status_output=ON.",
				    dict_remove_db_name(norm_name));

		/* Limit innodb monitor access to users with PROCESS privilege.
		See http://bugs.mysql.com/32710 why we chose PROCESS. */
		if (check_global_access(thd, PROCESS_ACL)) {
			DBUG_RETURN(HA_ERR_GENERIC);
		}
	}

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	parent_trx = check_trx_exists(thd);

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(parent_trx);

	trx = innobase_trx_allocate(thd);

	if (UNIV_UNLIKELY(trx->fake_changes)) {
		innobase_commit_low(trx);
		trx_free_for_mysql(trx);
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	/* Latch the InnoDB data dictionary exclusively so that no deadlocks
	or lock waits can happen in it during a table create operation.
	Drop table etc. do this latching in row0mysql.cc. */

	row_mysql_lock_data_dictionary(trx);

	heap = mem_heap_create(form->s->fields * sizeof(ulint));
	zip_dict_ids = static_cast<ulint*>(
		mem_heap_alloc(heap, form->s->fields * sizeof(ulint)));

	if (!innobase_check_zip_dicts(form, zip_dict_ids,
		trx, &err_zip_dict_name)) {
		error = -1;
		my_error(ER_COMPRESSION_DICTIONARY_DOES_NOT_EXIST,
			MYF(0), err_zip_dict_name);
		goto cleanup;
	}

	error = create_table_def(trx, form, norm_name, temp_path,
				 remote_path, flags, flags2);
	if (error) {
		goto cleanup;
	}

	/* Create the keys */

	if (form->s->keys == 0 || primary_key_no == -1) {
		/* Create an index which is used as the clustered index;
		order the rows by their row id which is internally generated
		by InnoDB */

		error = create_clustered_index_when_no_primary(
			trx, flags, norm_name);
		if (error) {
			goto cleanup;
		}
	}

	if (primary_key_no != -1) {
		/* In InnoDB the clustered index must always be created
		first */
		if ((error = create_index(trx, form, flags, norm_name,
					  (uint) primary_key_no))) {
			goto cleanup;
		}
	}

	/* Create the ancillary tables that are common to all FTS indexes on
	this table. */
	if (flags2 & DICT_TF2_FTS) {
		enum fts_doc_id_index_enum	ret;

		innobase_table = dict_table_open_on_name(
			norm_name, TRUE, FALSE, DICT_ERR_IGNORE_NONE);

		ut_a(innobase_table);

		/* Check whether there already exists FTS_DOC_ID_INDEX */
		ret = innobase_fts_check_doc_id_index_in_def(
			form->s->keys, form->key_info);

		switch (ret) {
		case FTS_INCORRECT_DOC_ID_INDEX:
			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    " InnoDB: Index name %s is reserved"
					    " for the unique index on"
					    " FTS_DOC_ID column for FTS"
					    " Document ID indexing"
					    " on table %s. Please check"
					    " the index definition to"
					    " make sure it is of correct"
					    " type\n",
					    FTS_DOC_ID_INDEX_NAME,
					    innobase_table->name);

			if (innobase_table->fts) {
				fts_free(innobase_table);
			}

			dict_table_close(innobase_table, TRUE, FALSE);
			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 FTS_DOC_ID_INDEX_NAME);
			error = -1;
			goto cleanup;
		case FTS_EXIST_DOC_ID_INDEX:
		case FTS_NOT_EXIST_DOC_ID_INDEX:
			break;
		}

		dberr_t	err = fts_create_common_tables(
			trx, innobase_table, norm_name,
			(ret == FTS_EXIST_DOC_ID_INDEX));

		error = convert_error_code_to_mysql(err, 0, NULL);

		dict_table_close(innobase_table, TRUE, FALSE);

		if (error) {
			goto cleanup;
		}
	}

	for (i = 0; i < form->s->keys; i++) {

		if (i != static_cast<uint>(primary_key_no)) {

			if ((error = create_index(trx, form, flags,
						  norm_name, i))) {
				goto cleanup;
			}
		}
	}

	/* Cache all the FTS indexes on this table in the FTS specific
	structure. They are used for FTS indexed column update handling. */
	if (flags2 & DICT_TF2_FTS) {
		fts_t*          fts = innobase_table->fts;

		ut_a(fts != NULL);

		dict_table_get_all_fts_indexes(innobase_table, fts->indexes);
	}

	/*
	Adding compression dictionary <-> compressed table column links
	to the SYS_ZIP_DICT_COLS table.
	*/
	ut_a(zip_dict_ids != 0);
	{
		dict_table_t*	local_table = dict_table_open_on_name(
			norm_name, TRUE, FALSE, DICT_ERR_IGNORE_NONE);

		ut_a(local_table);
		table_id_t table_id = local_table->id;
		dict_table_close(local_table, TRUE, FALSE);
		innobase_create_zip_dict_references(form,
			table_id, zip_dict_ids, trx);
	}

	stmt = innobase_get_stmt(thd, &stmt_len);

	if (stmt) {
		dberr_t	err = row_table_add_foreign_constraints(
			trx, stmt, stmt_len, norm_name,
			create_info->options & HA_LEX_CREATE_TMP_TABLE);

		switch (err) {

		case DB_PARENT_NO_INDEX:
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_CANNOT_ADD_FOREIGN,
				"Create table '%s' with foreign key constraint"
				" failed. There is no index in the referenced"
				" table where the referenced columns appear"
				" as the first columns.\n", norm_name);
			break;

		case DB_CHILD_NO_INDEX:
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_CANNOT_ADD_FOREIGN,
				"Create table '%s' with foreign key constraint"
				" failed. There is no index in the referencing"
				" table where referencing columns appear"
				" as the first columns.\n", norm_name);
			break;
		default:
			break;
		}

		error = convert_error_code_to_mysql(err, flags, NULL);

		if (error) {
			goto cleanup;
		}
	}

	innobase_commit_low(trx);

	row_mysql_unlock_data_dictionary(trx);

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	innobase_table = dict_table_open_on_name(
		norm_name, FALSE, FALSE, DICT_ERR_IGNORE_NONE);

	DBUG_ASSERT(innobase_table != 0);

	innobase_copy_frm_flags_from_create_info(innobase_table, create_info);

	dict_stats_update(innobase_table, DICT_STATS_EMPTY_TABLE);

	if (innobase_table) {
		/* We update the highest file format in the system table
		space, if this table has higher file format setting. */

		trx_sys_file_format_max_upgrade(
			(const char**) &innobase_file_format_max,
			dict_table_get_format(innobase_table));
	}

	/* Load server stopword into FTS cache */
	if (flags2 & DICT_TF2_FTS) {
		if (!innobase_fts_load_stopword(innobase_table, NULL, thd)) {
			dict_table_close(innobase_table, FALSE, FALSE);
			srv_active_wake_master_thread();
			trx_free_for_mysql(trx);
			DBUG_RETURN(-1);
		}
	}

	/* Note: We can't call update_thd() as prebuilt will not be
	setup at this stage and so we use thd. */

	/* We need to copy the AUTOINC value from the old table if
	this is an ALTER|OPTIMIZE TABLE or CREATE INDEX because CREATE INDEX
	does a table copy too. If query was one of :

		CREATE TABLE ...AUTO_INCREMENT = x; or
		ALTER TABLE...AUTO_INCREMENT = x;   or
		OPTIMIZE TABLE t; or
		CREATE INDEX x on t(...);

	Find out a table definition from the dictionary and get
	the current value of the auto increment field. Set a new
	value to the auto increment field if the value is greater
	than the maximum value in the column. */

	if (((create_info->used_fields & HA_CREATE_USED_AUTO)
	    || thd_sql_command(thd) == SQLCOM_ALTER_TABLE
	    || thd_sql_command(thd) == SQLCOM_OPTIMIZE
	    || thd_sql_command(thd) == SQLCOM_CREATE_INDEX)
	    && create_info->auto_increment_value > 0) {

		auto_inc_value = create_info->auto_increment_value;

		dict_table_autoinc_lock(innobase_table);
		dict_table_autoinc_initialize(innobase_table, auto_inc_value);
		dict_table_autoinc_unlock(innobase_table);
	}

	dict_table_close(innobase_table, FALSE, FALSE);

	/* Tell the InnoDB server that there might be work for
	utility threads: */

	srv_active_wake_master_thread();

	trx_free_for_mysql(trx);

	if (heap != 0)
		mem_heap_free(heap);

	DBUG_RETURN(0);

cleanup:
	trx_rollback_for_mysql(trx);

	row_mysql_unlock_data_dictionary(trx);

	trx_free_for_mysql(trx);

	if (heap != 0)
		mem_heap_free(heap);

	DBUG_RETURN(error);
}

/*****************************************************************//**
Discards or imports an InnoDB tablespace.
@return	0 == success, -1 == error */
UNIV_INTERN
int
ha_innobase::discard_or_import_tablespace(
/*======================================*/
	my_bool discard)	/*!< in: TRUE if discard, else import */
{
	dberr_t		err;
	dict_table_t*	dict_table;

	DBUG_ENTER("ha_innobase::discard_or_import_tablespace");

	ut_a(prebuilt->trx);
	ut_a(prebuilt->trx->magic_n == TRX_MAGIC_N);
	ut_a(prebuilt->trx == thd_to_trx(ha_thd()));

	if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	if (UNIV_UNLIKELY(prebuilt->trx->fake_changes)) {
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	dict_table = prebuilt->table;

	if (dict_table->space == TRX_SYS_SPACE) {

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_IN_SYSTEM_TABLESPACE,
			table->s->table_name.str);

		DBUG_RETURN(HA_ERR_TABLE_NEEDS_UPGRADE);
	}

	trx_start_if_not_started(prebuilt->trx);

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads. */
	trx_search_latch_release_if_reserved(prebuilt->trx);

	/* Obtain an exclusive lock on the table. */
	err = row_mysql_lock_table(
		prebuilt->trx, dict_table, LOCK_X,
		discard ? "setting table lock for DISCARD TABLESPACE"
			: "setting table lock for IMPORT TABLESPACE");

	if (err != DB_SUCCESS) {
		/* unable to lock the table: do nothing */
	} else if (discard) {

		/* Discarding an already discarded tablespace should be an
		idempotent operation. Also, if the .ibd file is missing the
		user may want to set the DISCARD flag in order to IMPORT
		a new tablespace. */

		if (dict_table->ibd_file_missing) {
			ib_senderrf(
				prebuilt->trx->mysql_thd,
				IB_LOG_LEVEL_WARN, ER_TABLESPACE_MISSING,
				table->s->table_name.str);
		}

		err = row_discard_tablespace_for_mysql(
			dict_table->name, prebuilt->trx);

	} else if (!dict_table->ibd_file_missing) {
		/* Commit the transaction in order to
		release the table lock. */
		trx_commit_for_mysql(prebuilt->trx);

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_EXISTS, table->s->table_name.str);

		DBUG_RETURN(HA_ERR_TABLE_EXIST);
	} else {
		err = row_import_for_mysql(dict_table, prebuilt);

		if (err == DB_SUCCESS) {

			if (table->found_next_number_field) {
				dict_table_autoinc_lock(dict_table);
				innobase_initialize_autoinc();
				dict_table_autoinc_unlock(dict_table);
			}

			info(HA_STATUS_TIME
			     | HA_STATUS_CONST
			     | HA_STATUS_VARIABLE
			     | HA_STATUS_AUTO);
		}
	}

	/* Commit the transaction in order to release the table lock. */
	trx_commit_for_mysql(prebuilt->trx);

	if (err == DB_SUCCESS && !discard
	    && dict_stats_is_persistent_enabled(dict_table)) {
		dberr_t		ret;

		/* Adjust the persistent statistics. */
		ret = dict_stats_update(dict_table,
					DICT_STATS_RECALC_PERSISTENT);

		if (ret != DB_SUCCESS) {
			push_warning_printf(
				ha_thd(),
				Sql_condition::WARN_LEVEL_WARN,
				ER_ALTER_INFO,
				"Error updating stats for table '%s'"
				" after table rebuild: %s",
				dict_table->name, ut_strerr(ret));
		}
	}

	DBUG_RETURN(convert_error_code_to_mysql(err, dict_table->flags, NULL));
}

/*****************************************************************//**
Deletes all rows of an InnoDB table.
@return	error number */
UNIV_INTERN
int
ha_innobase::truncate()
/*===================*/
{
	dberr_t		err;
	int		error;

	DBUG_ENTER("ha_innobase::truncate");

	if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	/* Get the transaction associated with the current thd, or create one
	if not yet created, and update prebuilt->trx */

	update_thd(ha_thd());

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (UNIV_UNLIKELY(prebuilt->trx->fake_changes)) {
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	if (!trx_is_started(prebuilt->trx)) {
		++prebuilt->trx->will_lock;
	}
	/* Truncate the table in InnoDB */

	err = row_truncate_table_for_mysql(prebuilt->table, prebuilt->trx);

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	switch (err) {

	case DB_TABLESPACE_DELETED:
	case DB_TABLESPACE_NOT_FOUND:
		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			(err == DB_TABLESPACE_DELETED ?
			ER_TABLESPACE_DISCARDED : ER_TABLESPACE_MISSING),
			table->s->table_name.str);
		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_NO_SUCH_TABLE;
		break;

	default:
		error = convert_error_code_to_mysql(
			err, prebuilt->table->flags,
			prebuilt->trx->mysql_thd);
		table->status = STATUS_NOT_FOUND;
		break;
	}
	DBUG_RETURN(error);
}

/*****************************************************************//**
Drops a table from an InnoDB database. Before calling this function,
MySQL calls innobase_commit to commit the transaction of the current user.
Then the current user cannot have locks set on the table. Drop table
operation inside InnoDB will remove all locks any user has on the table
inside InnoDB.
@return	error number */
UNIV_INTERN
int
ha_innobase::delete_table(
/*======================*/
	const char*	name)	/*!< in: table name */
{
	ulint	name_len;
	dberr_t	err;
	trx_t*	parent_trx;
	trx_t*	trx;
	THD*	thd = ha_thd();
	char	norm_name[FN_REFLEN];

	DBUG_ENTER("ha_innobase::delete_table");

	DBUG_EXECUTE_IF(
		"test_normalize_table_name_low",
		test_normalize_table_name_low();
	);
	DBUG_EXECUTE_IF(
		"test_ut_format_name",
		test_ut_format_name();
	);

	/* Strangely, MySQL passes the table name without the '.frm'
	extension, in contrast to ::create */
	normalize_table_name(norm_name, name);

	if (srv_read_only_mode
	    || srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (row_is_magic_monitor_table(norm_name)
		   && check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(HA_ERR_GENERIC);
	}

	parent_trx = check_trx_exists(thd);

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(parent_trx);

	trx = innobase_trx_allocate(thd);

	if (UNIV_UNLIKELY(trx->fake_changes)) {
		innobase_commit_low(trx);
		trx_free_for_mysql(trx);
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	name_len = strlen(name);

	ut_a(name_len < 1000);

	/* Either the transaction is already flagged as a locking transaction
	or it hasn't been started yet. */

	ut_a(!trx_is_started(trx) || trx->will_lock > 0);

	/* We are doing a DDL operation. */
	++trx->will_lock;
	trx->ddl = true;

	/* Drop the table in InnoDB */
	err = row_drop_table_for_mysql(
		norm_name, trx, thd_sql_command(thd) == SQLCOM_DROP_DB);


	if (err == DB_TABLE_NOT_FOUND
	    && innobase_get_lower_case_table_names() == 1) {
		char*	is_part = NULL;
#ifdef __WIN__
		is_part = strstr(norm_name, "#p#");
#else
		is_part = strstr(norm_name, "#P#");
#endif /* __WIN__ */

		if (is_part) {
			char	par_case_name[FN_REFLEN];

#ifndef __WIN__
			/* Check for the table using lower
			case name, including the partition
			separator "P" */
			strcpy(par_case_name, norm_name);
			innobase_casedn_str(par_case_name);
#else
			/* On Windows platfrom, check
			whether there exists table name in
			system table whose name is
			not being normalized to lower case */
			normalize_table_name_low(
				par_case_name, name, FALSE);
#endif
			err = row_drop_table_for_mysql(
				par_case_name, trx,
				thd_sql_command(thd) == SQLCOM_DROP_DB);
		}
	}

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	innobase_commit_low(trx);

	trx_free_for_mysql(trx);

	DBUG_RETURN(convert_error_code_to_mysql(err, 0, NULL));
}

/*****************************************************************//**
Removes all tables in the named database inside InnoDB. */
static
void
innobase_drop_database(
/*===================*/
	handlerton*	hton,	/*!< in: handlerton of Innodb */
	char*		path)	/*!< in: database path; inside InnoDB the name
				of the last directory in the path is used as
				the database name: for example, in
				'mysql/data/test' the database name is 'test' */
{
	ulint	len		= 0;
	trx_t*	trx;
	char*	ptr;
	char*	namebuf;
	THD*	thd		= current_thd;

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (srv_read_only_mode) {
		return;
	}

	/* In the Windows plugin, thd = current_thd is always NULL */
	if (thd) {
		trx_t*	parent_trx = check_trx_exists(thd);

		/* In case MySQL calls this in the middle of a SELECT
		query, release possible adaptive hash latch to avoid
		deadlocks of threads */

		trx_search_latch_release_if_reserved(parent_trx);
	}

	ptr = strend(path) - 2;

	while (ptr >= path && *ptr != '\\' && *ptr != '/') {
		ptr--;
		len++;
	}

	ptr++;
	namebuf = (char*) my_malloc((uint) len + 2, MYF(0));

	memcpy(namebuf, ptr, len);
	namebuf[len] = '/';
	namebuf[len + 1] = '\0';
#ifdef	__WIN__
	innobase_casedn_str(namebuf);
#endif
	trx = innobase_trx_allocate(thd);

	if (UNIV_UNLIKELY(trx->fake_changes)) {
		my_free(namebuf);
		innobase_commit_low(trx);
		trx_free_for_mysql(trx);
		return; /* ignore */
	}

	/* Either the transaction is already flagged as a locking transaction
	or it hasn't been started yet. */

	ut_a(!trx_is_started(trx) || trx->will_lock > 0);

	/* We are doing a DDL operation. */
	++trx->will_lock;

	row_drop_database_for_mysql(namebuf, trx);

	my_free(namebuf);

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	innobase_commit_low(trx);
	trx_free_for_mysql(trx);
}

/*********************************************************************//**
Renames an InnoDB table.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
innobase_rename_table(
/*==================*/
	THD*            thd,    /*!< Connection thread handle */
	trx_t*		trx,	/*!< in: transaction */
	const char*	from,	/*!< in: old name of the table */
	const char*	to)	/*!< in: new name of the table */
{
	dberr_t	error;
	char	norm_to[FN_REFLEN];
	char	norm_from[FN_REFLEN];

	DBUG_ENTER("innobase_rename_table");
	DBUG_ASSERT(trx_get_dict_operation(trx) == TRX_DICT_OP_INDEX);

	ut_ad(!srv_read_only_mode);

	normalize_table_name(norm_to, to);
	normalize_table_name(norm_from, from);

	DEBUG_SYNC_C("innodb_rename_table_ready");

	trx_start_if_not_started(trx);

	/* Serialize data dictionary operations with dictionary mutex:
	no deadlocks can occur then in these operations. */

	row_mysql_lock_data_dictionary(trx);

	dict_table_t*   table                   = NULL;
        table = dict_table_open_on_name(norm_from, TRUE, FALSE,
                                        DICT_ERR_IGNORE_NONE);

        /* Since DICT_BG_YIELD has sleep for 250 milliseconds,
	Convert lock_wait_timeout unit from second to 250 milliseconds */
        long int lock_wait_timeout = thd_lock_wait_timeout(thd) * 4;
        if (table != NULL) {
                for (dict_index_t* index = dict_table_get_first_index(table);
                     index != NULL;
                     index = dict_table_get_next_index(index)) {

                        if (index->type & DICT_FTS) {
                                /* Found */
                                while (index->index_fts_syncing
                                        && !trx_is_interrupted(trx)
                                        && (lock_wait_timeout--) > 0) {
                                        DICT_BG_YIELD(trx);
                                }
                        }
                }
                dict_table_close(table, TRUE, FALSE);
        }

        /* FTS sync is in progress. We shall timeout this operation */
        if (lock_wait_timeout < 0) {
                error = DB_LOCK_WAIT_TIMEOUT;
                row_mysql_unlock_data_dictionary(trx);
                DBUG_RETURN(error);
        }

	/* Transaction must be flagged as a locking transaction or it hasn't
	been started yet. */

	ut_a(trx->will_lock > 0);

	error = row_rename_table_for_mysql(
		norm_from, norm_to, trx, TRUE);

	if (error != DB_SUCCESS) {
		if (error == DB_TABLE_NOT_FOUND
		    && innobase_get_lower_case_table_names() == 1) {
			char*	is_part = NULL;
#ifdef __WIN__
			is_part = strstr(norm_from, "#p#");
#else
			is_part = strstr(norm_from, "#P#");
#endif /* __WIN__ */

			if (is_part) {
				char	par_case_name[FN_REFLEN];
#ifndef __WIN__
				/* Check for the table using lower
				case name, including the partition
				separator "P" */
				strcpy(par_case_name, norm_from);
				innobase_casedn_str(par_case_name);
#else
				/* On Windows platfrom, check
				whether there exists table name in
				system table whose name is
				not being normalized to lower case */
				normalize_table_name_low(
					par_case_name, from, FALSE);
#endif
				trx_start_if_not_started(trx);
				error = row_rename_table_for_mysql(
					par_case_name, norm_to, trx, TRUE);
			}
		}

		if (error == DB_SUCCESS) {
#ifndef __WIN__
			sql_print_warning("Rename partition table %s "
					  "succeeds after converting to lower "
					  "case. The table may have "
					  "been moved from a case "
					  "in-sensitive file system.\n",
					  norm_from);
#else
			sql_print_warning("Rename partition table %s "
					  "succeeds after skipping the step to "
					  "lower case the table name. "
					  "The table may have been "
					  "moved from a case sensitive "
					  "file system.\n",
					  norm_from);
#endif /* __WIN__ */
		}
	}

	row_mysql_unlock_data_dictionary(trx);

	/* Flush the log to reduce probability that the .frm
	files and the InnoDB data dictionary get out-of-sync
	if the user runs with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	DBUG_RETURN(error);
}

/*********************************************************************//**
Renames an InnoDB table.
@return	0 or error code */
UNIV_INTERN
int
ha_innobase::rename_table(
/*======================*/
	const char*	from,	/*!< in: old name of the table */
	const char*	to)	/*!< in: new name of the table */
{
	trx_t*	trx;
	dberr_t	error;
	trx_t*	parent_trx;
	THD*	thd		= ha_thd();

	DBUG_ENTER("ha_innobase::rename_table");

	if (high_level_read_only) {
		ib_senderrf(thd, IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	parent_trx = check_trx_exists(thd);

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(parent_trx);

	trx = innobase_trx_allocate(thd);
	if (UNIV_UNLIKELY(trx->fake_changes)) {
		innobase_commit_low(trx);
		trx_free_for_mysql(trx);
		DBUG_RETURN(HA_ERR_WRONG_COMMAND);
	}

	/* We are doing a DDL operation. */
	++trx->will_lock;
	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);

	error = innobase_rename_table(thd, trx, from, to);

	DEBUG_SYNC(thd, "after_innobase_rename_table");

	innobase_commit_low(trx);
	trx_free_for_mysql(trx);

	if (error == DB_SUCCESS) {
		char	norm_from[MAX_FULL_NAME_LEN];
		char	norm_to[MAX_FULL_NAME_LEN];
		char	errstr[512];
		dberr_t	ret;

		normalize_table_name(norm_from, from);
		normalize_table_name(norm_to, to);

		ret = dict_stats_rename_table(norm_from, norm_to,
					      errstr, sizeof(errstr));

		if (ret != DB_SUCCESS) {
			ut_print_timestamp(stderr);
			fprintf(stderr, " InnoDB: %s\n", errstr);

			push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
				     ER_LOCK_WAIT_TIMEOUT, errstr);
		}
	}

	/* Add a special case to handle the Duplicated Key error
	and return DB_ERROR instead.
	This is to avoid a possible SIGSEGV error from mysql error
	handling code. Currently, mysql handles the Duplicated Key
	error by re-entering the storage layer and getting dup key
	info by calling get_dup_key(). This operation requires a valid
	table handle ('row_prebuilt_t' structure) which could no
	longer be available in the error handling stage. The suggested
	solution is to report a 'table exists' error message (since
	the dup key error here is due to an existing table whose name
	is the one we are trying to rename to) and return the generic
	error code. */
	if (error == DB_DUPLICATE_KEY) {
		my_error(ER_TABLE_EXISTS_ERROR, MYF(0), to);

		error = DB_ERROR;
	}

	else if (error == DB_LOCK_WAIT_TIMEOUT) {
                my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0), to);

                error = DB_LOCK_WAIT;
        }

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*********************************************************************//**
Estimates the number of index records in a range.
@return	estimated number of rows */
UNIV_INTERN
ha_rows
ha_innobase::records_in_range(
/*==========================*/
	uint			keynr,		/*!< in: index number */
	key_range		*min_key,	/*!< in: start key value of the
						range, may also be 0 */
	key_range		*max_key)	/*!< in: range end key val, may
						also be 0 */
{
	KEY*		key;
	dict_index_t*	index;
	dtuple_t*	range_start;
	dtuple_t*	range_end;
	ib_int64_t	n_rows;
	ulint		mode1;
	ulint		mode2;
	mem_heap_t*	heap;

	DBUG_ENTER("records_in_range");

	ut_a(prebuilt->trx == thd_to_trx(ha_thd()));

	prebuilt->trx->op_info = (char*)"estimating records in index range";

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(prebuilt->trx);

	active_index = keynr;

	key = table->key_info + active_index;

	index = innobase_get_index(keynr);

	/* There exists possibility of not being able to find requested
	index due to inconsistency between MySQL and InoDB dictionary info.
	Necessary message should have been printed in innobase_get_index() */
	if (dict_table_is_discarded(prebuilt->table)) {
		n_rows = HA_POS_ERROR;
		goto func_exit;
	}
	if (UNIV_UNLIKELY(!index)) {
		n_rows = HA_POS_ERROR;
		goto func_exit;
	}
	if (dict_index_is_corrupted(index)) {
		n_rows = HA_ERR_INDEX_CORRUPT;
		goto func_exit;
	}
	if (UNIV_UNLIKELY(!row_merge_is_index_usable(prebuilt->trx, index))) {
		n_rows = HA_ERR_TABLE_DEF_CHANGED;
		goto func_exit;
	}

	heap = mem_heap_create(2 * (key->actual_key_parts * sizeof(dfield_t)
				    + sizeof(dtuple_t)));

	range_start = dtuple_create(heap, key->actual_key_parts);
	dict_index_copy_types(range_start, index, key->actual_key_parts);

	range_end = dtuple_create(heap, key->actual_key_parts);
	dict_index_copy_types(range_end, index, key->actual_key_parts);

	row_sel_convert_mysql_key_to_innobase(
				range_start,
				prebuilt->srch_key_val1,
				prebuilt->srch_key_val_len,
				index,
				(byte*) (min_key ? min_key->key :
					 (const uchar*) 0),
				(ulint) (min_key ? min_key->length : 0),
				prebuilt->trx);
	DBUG_ASSERT(min_key
		    ? range_start->n_fields > 0
		    : range_start->n_fields == 0);

	row_sel_convert_mysql_key_to_innobase(
				range_end,
				prebuilt->srch_key_val2,
				prebuilt->srch_key_val_len,
				index,
				(byte*) (max_key ? max_key->key :
					 (const uchar*) 0),
				(ulint) (max_key ? max_key->length : 0),
				prebuilt->trx);
	DBUG_ASSERT(max_key
		    ? range_end->n_fields > 0
		    : range_end->n_fields == 0);

	mode1 = convert_search_mode_to_innobase(min_key ? min_key->flag :
						HA_READ_KEY_EXACT);
	mode2 = convert_search_mode_to_innobase(max_key ? max_key->flag :
						HA_READ_KEY_EXACT);

	if (mode1 != PAGE_CUR_UNSUPP && mode2 != PAGE_CUR_UNSUPP) {

		n_rows = btr_estimate_n_rows_in_range(index, range_start,
						      mode1, range_end,
						      mode2);
	} else {

		n_rows = HA_POS_ERROR;
	}

	mem_heap_free(heap);

func_exit:

	prebuilt->trx->op_info = (char*)"";

	/* The MySQL optimizer seems to believe an estimate of 0 rows is
	always accurate and may return the result 'Empty set' based on that.
	The accuracy is not guaranteed, and even if it were, for a locking
	read we should anyway perform the search to set the next-key lock.
	Add 1 to the value to make sure MySQL does not make the assumption! */

	if (n_rows == 0) {
		n_rows = 1;
	}

	DBUG_RETURN((ha_rows) n_rows);
}

/*********************************************************************//**
Gives an UPPER BOUND to the number of rows in a table. This is used in
filesort.cc.
@return	upper bound of rows */
UNIV_INTERN
ha_rows
ha_innobase::estimate_rows_upper_bound()
/*====================================*/
{
	const dict_index_t*	index;
	ulonglong		estimate;
	ulonglong		local_data_file_length;
	ulint			stat_n_leaf_pages;

	DBUG_ENTER("estimate_rows_upper_bound");

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	prebuilt->trx->op_info = "calculating upper bound for table rows";

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(prebuilt->trx);

	index = dict_table_get_first_index(prebuilt->table);

	stat_n_leaf_pages = index->stat_n_leaf_pages;

	ut_a(stat_n_leaf_pages > 0);

	local_data_file_length =
		((ulonglong) stat_n_leaf_pages) * UNIV_PAGE_SIZE;

	/* Calculate a minimum length for a clustered index record and from
	that an upper bound for the number of rows. Since we only calculate
	new statistics in row0mysql.cc when a table has grown by a threshold
	factor, we must add a safety factor 2 in front of the formula below. */

	estimate = 2 * local_data_file_length
		/ dict_index_calc_min_rec_len(index);

	prebuilt->trx->op_info = "";

        /* Set num_rows less than MERGEBUFF to simulate the case where we do
        not have enough space to merge the externally sorted file blocks. */
        DBUG_EXECUTE_IF("set_num_rows_lt_MERGEBUFF",
                        estimate = 2;
                        DBUG_SET("-d,set_num_rows_lt_MERGEBUFF");
                       );

	DBUG_RETURN((ha_rows) estimate);
}

/*********************************************************************//**
How many seeks it will take to read through the table. This is to be
comparable to the number returned by records_in_range so that we can
decide if we should scan the table or use keys.
@return	estimated time measured in disk seeks */
UNIV_INTERN
double
ha_innobase::scan_time()
/*====================*/
{
	/* Since MySQL seems to favor table scans too much over index
	searches, we pretend that a sequential read takes the same time
	as a random disk read, that is, we do not divide the following
	by 10, which would be physically realistic. */

	/* The locking below is disabled for performance reasons. Without
	it we could end up returning uninitialized value to the caller,
	which in the worst case could make some query plan go bogus or
	issue a Valgrind warning. */
#if 0
	/* avoid potential lock order violation with dict_table_stats_lock()
	below */
	update_thd(ha_thd());
	trx_search_latch_release_if_reserved(prebuilt->trx);
#endif

	ulint	stat_clustered_index_size;

#if 0
	dict_table_stats_lock(prebuilt->table, RW_S_LATCH);
#endif

	ut_a(prebuilt->table->stat_initialized);

	stat_clustered_index_size = prebuilt->table->stat_clustered_index_size;

#if 0
	dict_table_stats_unlock(prebuilt->table, RW_S_LATCH);
#endif

	return((double) stat_clustered_index_size);
}

/******************************************************************//**
Calculate the time it takes to read a set of ranges through an index
This enables us to optimise reads for clustered indexes.
@return	estimated time measured in disk seeks */
UNIV_INTERN
double
ha_innobase::read_time(
/*===================*/
	uint	index,	/*!< in: key number */
	uint	ranges,	/*!< in: how many ranges */
	ha_rows rows)	/*!< in: estimated number of rows in the ranges */
{
	ha_rows total_rows;
	double	time_for_scan;

	if (index != table->s->primary_key) {
		/* Not clustered */
		return(handler::read_time(index, ranges, rows));
	}

	if (rows <= 2) {

		return((double) rows);
	}

	/* Assume that the read time is proportional to the scan time for all
	rows + at most one seek per range. */

	time_for_scan = scan_time();

	if ((total_rows = estimate_rows_upper_bound()) < rows) {

		return(time_for_scan);
	}

	return(ranges + (double) rows / (double) total_rows * time_for_scan);
}

/******************************************************************//**
Return the size of the InnoDB memory buffer. */
UNIV_INTERN
longlong
ha_innobase::get_memory_buffer_size() const
/*=======================================*/
{
	return(innobase_buffer_pool_size);
}

/*********************************************************************//**
Calculates the key number used inside MySQL for an Innobase index. We will
first check the "index translation table" for a match of the index to get
the index number. If there does not exist an "index translation table",
or not able to find the index in the translation table, then we will fall back
to the traditional way of looping through dict_index_t list to find a
match. In this case, we have to take into account if we generated a
default clustered index for the table
@return the key number used inside MySQL */
static
int
innobase_get_mysql_key_number_for_index(
/*====================================*/
	INNOBASE_SHARE*		share,	/*!< in: share structure for index
					translation table. */
	const TABLE*		table,	/*!< in: table in MySQL data
					dictionary */
	dict_table_t*		ib_table,/*!< in: table in Innodb data
					dictionary */
	const dict_index_t*	index)	/*!< in: index */
{
	const dict_index_t*	ind;
	unsigned int		i;

 	ut_a(index);

	/* If index does not belong to the table object of share structure
	(ib_table comes from the share structure) search the index->table
	object instead */
	if (index->table != ib_table) {
		i = 0;
		ind = dict_table_get_first_index(index->table);

		while (index != ind) {
			ind = dict_table_get_next_index(ind);
			i++;
		}

		if (row_table_got_default_clust_index(index->table)) {
			ut_a(i > 0);
			i--;
		}

		return(i);
	}

	/* If index translation table exists, we will first check
	the index through index translation table for a match. */
	if (share->idx_trans_tbl.index_mapping) {
		for (i = 0; i < share->idx_trans_tbl.index_count; i++) {
			if (share->idx_trans_tbl.index_mapping[i] == index) {
				return(i);
			}
		}

		/* Print an error message if we cannot find the index
		in the "index translation table". */
		if (*index->name != TEMP_INDEX_PREFIX) {
			sql_print_error("Cannot find index %s in InnoDB index "
					"translation table.", index->name);
		}
	}

	/* If we do not have an "index translation table", or not able
	to find the index in the translation table, we'll directly find
	matching index with information from mysql TABLE structure and
	InnoDB dict_index_t list */
	for (i = 0; i < table->s->keys; i++) {
		ind = dict_table_get_index_on_name(
			ib_table, table->key_info[i].name);

		if (index == ind) {
			return(i);
		}
	}

	/* Loop through each index of the table and lock them */
	for (ind = dict_table_get_first_index(ib_table);
	     ind != NULL;
	     ind = dict_table_get_next_index(ind)) {
		if (index == ind) {
			/* Temp index is internal to InnoDB, that is
			not present in the MySQL index list, so no
			need to print such mismatch warning. */
			if (*(index->name) != TEMP_INDEX_PREFIX) {
				sql_print_warning(
					"Find index %s in InnoDB index list "
					"but not its MySQL index number "
					"It could be an InnoDB internal index.",
					index->name);
			}
			return(-1);
		}
	}

	ut_error;

	return(-1);
}

/*********************************************************************//**
Calculate Record Per Key value. Need to exclude the NULL value if
innodb_stats_method is set to "nulls_ignored"
@return estimated record per key value */
static
ha_rows
innodb_rec_per_key(
/*===============*/
	dict_index_t*	index,		/*!< in: dict_index_t structure */
	ulint		i,		/*!< in: the column we are
					calculating rec per key */
	ha_rows		records)	/*!< in: estimated total records */
{
	ha_rows		rec_per_key;
	ib_uint64_t	n_diff;

	ut_a(index->table->stat_initialized);

	ut_ad(i < dict_index_get_n_unique(index));

	n_diff = index->stat_n_diff_key_vals[i];

	if (n_diff == 0) {

		rec_per_key = records;
	} else if (srv_innodb_stats_method == SRV_STATS_NULLS_IGNORED) {
		ib_uint64_t	n_null;
		ib_uint64_t	n_non_null;

		n_non_null = index->stat_n_non_null_key_vals[i];

		/* In theory, index->stat_n_non_null_key_vals[i]
		should always be less than the number of records.
		Since this is statistics value, the value could
		have slight discrepancy. But we will make sure
		the number of null values is not a negative number. */
		if (records < n_non_null) {
			n_null = 0;
		} else {
			n_null = records - n_non_null;
		}

		/* If the number of NULL values is the same as or
		large than that of the distinct values, we could
		consider that the table consists mostly of NULL value.
		Set rec_per_key to 1. */
		if (n_diff <= n_null) {
			rec_per_key = 1;
		} else {
			/* Need to exclude rows with NULL values from
			rec_per_key calculation */
			rec_per_key = (ha_rows)
				((records - n_null) / (n_diff - n_null));
		}
	} else {
		DEBUG_SYNC_C("after_checking_for_0");
		rec_per_key = (ha_rows) (records / n_diff);
	}

	return(rec_per_key);
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */
UNIV_INTERN
int
ha_innobase::info_low(
/*==================*/
	uint	flag,	/*!< in: what information is requested */
	bool	is_analyze)
{
	dict_table_t*	ib_table;
	ha_rows		rec_per_key;
	ib_uint64_t	n_rows;
	os_file_stat_t	stat_info;

	DBUG_ENTER("info");

	/* If we are forcing recovery at a high level, we will suppress
	statistics calculation on tables, because that may crash the
	server if an index is badly corrupted. */

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	prebuilt->trx->op_info = (char*)"returning various info to MySQL";

	trx_search_latch_release_if_reserved(prebuilt->trx);

	ib_table = prebuilt->table;
	DBUG_ASSERT(ib_table->n_ref_count > 0);

	if (flag & HA_STATUS_TIME) {
		if (is_analyze || innobase_stats_on_metadata) {

			dict_stats_upd_option_t	opt;
			dberr_t			ret;

			prebuilt->trx->op_info = "updating table statistics";

			if (dict_stats_is_persistent_enabled(ib_table)) {

				if (is_analyze) {

					/* If this table is already queued for
					background analyze, remove it from the
					queue as we are about to do the same */
					if (!srv_read_only_mode) {

						dict_mutex_enter_for_mysql();
						dict_stats_recalc_pool_del(
							ib_table);
						dict_mutex_exit_for_mysql();
					}

					opt = DICT_STATS_RECALC_PERSISTENT;
				} else {
					/* This is e.g. 'SHOW INDEXES', fetch
					the persistent stats from disk. */
					opt = DICT_STATS_FETCH_ONLY_IF_NOT_IN_MEMORY;
				}
			} else {
				opt = DICT_STATS_RECALC_TRANSIENT;
			}

			ut_ad(!mutex_own(&dict_sys->mutex));
			ret = dict_stats_update(ib_table, opt);

			if (ret != DB_SUCCESS) {
				prebuilt->trx->op_info = "";
				DBUG_RETURN(HA_ERR_GENERIC);
			}

			prebuilt->trx->op_info =
				"returning various info to MySQL";
		}

	}

	if (flag & HA_STATUS_VARIABLE) {

		ulint	page_size;
		ulint	stat_clustered_index_size;
		ulint	stat_sum_of_other_index_sizes;

		if (!(flag & HA_STATUS_NO_LOCK)) {
			dict_table_stats_lock(ib_table, RW_S_LATCH);
		}

		ut_a(ib_table->stat_initialized);

		n_rows = ib_table->stat_n_rows;

		stat_clustered_index_size
			= ib_table->stat_clustered_index_size;

		stat_sum_of_other_index_sizes
			= ib_table->stat_sum_of_other_index_sizes;

		if (!(flag & HA_STATUS_NO_LOCK)) {
			dict_table_stats_unlock(ib_table, RW_S_LATCH);
		}

		/*
		The MySQL optimizer seems to assume in a left join that n_rows
		is an accurate estimate if it is zero. Of course, it is not,
		since we do not have any locks on the rows yet at this phase.
		Since SHOW TABLE STATUS seems to call this function with the
		HA_STATUS_TIME flag set, while the left join optimizer does not
		set that flag, we add one to a zero value if the flag is not
		set. That way SHOW TABLE STATUS will show the best estimate,
		while the optimizer never sees the table empty. */

		if (n_rows == 0 && !(flag & HA_STATUS_TIME)) {
			n_rows++;
		}

		/* Fix bug#40386: Not flushing query cache after truncate.
		n_rows can not be 0 unless the table is empty, set to 1
		instead. The original problem of bug#29507 is actually
		fixed in the server code. */
		if (thd_sql_command(user_thd) == SQLCOM_TRUNCATE) {

			n_rows = 1;

			/* We need to reset the prebuilt value too, otherwise
			checks for values greater than the last value written
			to the table will fail and the autoinc counter will
			not be updated. This will force write_row() into
			attempting an update of the table's AUTOINC counter. */

			prebuilt->autoinc_last_value = 0;
		}

		page_size = dict_table_zip_size(ib_table);
		if (page_size == 0) {
			page_size = UNIV_PAGE_SIZE;
		}

		stats.records = (ha_rows) n_rows;
		stats.deleted = 0;
		stats.data_file_length
			= ((ulonglong) stat_clustered_index_size)
			* page_size;
		stats.index_file_length
			= ((ulonglong) stat_sum_of_other_index_sizes)
			* page_size;

		/* Since fsp_get_available_space_in_free_extents() is
		acquiring latches inside InnoDB, we do not call it if we
		are asked by MySQL to avoid locking. Another reason to
		avoid the call is that it uses quite a lot of CPU.
		See Bug#38185. */
		if (flag & HA_STATUS_NO_LOCK
		    || !(flag & HA_STATUS_VARIABLE_EXTRA)) {
			/* We do not update delete_length if no
			locking is requested so the "old" value can
			remain. delete_length is initialized to 0 in
			the ha_statistics' constructor. Also we only
			need delete_length to be set when
			HA_STATUS_VARIABLE_EXTRA is set */
		} else if (UNIV_UNLIKELY
			   (srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE)) {
			/* Avoid accessing the tablespace if
			innodb_crash_recovery is set to a high value. */
			stats.delete_length = 0;
		} else {
			ullint	avail_space;

			avail_space = fsp_get_available_space_in_free_extents(
				ib_table->space);

			if (avail_space == ULLINT_UNDEFINED) {
				THD*	thd;
				char	errbuf[MYSYS_STRERROR_SIZE];

				thd = ha_thd();

				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_CANT_GET_STAT,
					"InnoDB: Trying to get the free "
					"space for table %s but its "
					"tablespace has been discarded or "
					"the .ibd file is missing. Setting "
					"the free space to zero. "
					"(errno: %d - %s)",
					ib_table->name, errno,
					my_strerror(errbuf, sizeof(errbuf),
						    errno));

				stats.delete_length = 0;
			} else {
				stats.delete_length = avail_space * 1024;
			}
		}

		stats.check_time = 0;
		stats.mrr_length_per_rec = ref_length + sizeof(void*);

		if (stats.records == 0) {
			stats.mean_rec_length = 0;
		} else {
			stats.mean_rec_length = (ulong)
				(stats.data_file_length / stats.records);
		}
	}

	if (flag & HA_STATUS_CONST) {
		ulong	i;
		char	path[FN_REFLEN];
		/* Verify the number of index in InnoDB and MySQL
		matches up. If prebuilt->clust_index_was_generated
		holds, InnoDB defines GEN_CLUST_INDEX internally */
		ulint	num_innodb_index = UT_LIST_GET_LEN(ib_table->indexes)
			- prebuilt->clust_index_was_generated;
		if (table->s->keys < num_innodb_index) {
			/* If there are too many indexes defined
			inside InnoDB, ignore those that are being
			created, because MySQL will only consider
			the fully built indexes here. */

			for (const dict_index_t* index
				     = UT_LIST_GET_FIRST(ib_table->indexes);
			     index != NULL;
			     index = UT_LIST_GET_NEXT(indexes, index)) {

				/* First, online index creation is
				completed inside InnoDB, and then
				MySQL attempts to upgrade the
				meta-data lock so that it can rebuild
				the .frm file. If we get here in that
				time frame, dict_index_is_online_ddl()
				would not hold and the index would
				still not be included in TABLE_SHARE. */
				if (*index->name == TEMP_INDEX_PREFIX) {
					num_innodb_index--;
				}
			}

			if (table->s->keys < num_innodb_index
			    && innobase_fts_check_doc_id_index(
				    ib_table, NULL, NULL)
			    == FTS_EXIST_DOC_ID_INDEX) {
				num_innodb_index--;
			}
		}

		if (table->s->keys != num_innodb_index) {
			sql_print_error("InnoDB: Table %s contains %lu "
					"indexes inside InnoDB, which "
					"is different from the number of "
					"indexes %u defined in the MySQL ",
					ib_table->name, num_innodb_index,
					table->s->keys);
		}

		if (!(flag & HA_STATUS_NO_LOCK)) {
			dict_table_stats_lock(ib_table, RW_S_LATCH);
		}

		ut_a(ib_table->stat_initialized);

		for (i = 0; i < table->s->keys; i++) {
			ulong	j;
			/* We could get index quickly through internal
			index mapping with the index translation table.
			The identity of index (match up index name with
			that of table->key_info[i]) is already verified in
			innobase_get_index().  */
			dict_index_t* index = innobase_get_index(i);

			if (index == NULL) {
				sql_print_error("Table %s contains fewer "
						"indexes inside InnoDB than "
						"are defined in the MySQL "
						".frm file. Have you mixed up "
						".frm files from different "
						"installations? See "
						REFMAN
						"innodb-troubleshooting.html\n",
						ib_table->name);
				break;
			}

			for (j = 0; j < table->key_info[i].actual_key_parts; j++) {

				if (table->key_info[i].flags & HA_FULLTEXT) {
					/* The whole concept has no validity
					for FTS indexes. */
					table->key_info[i].rec_per_key[j] = 1;
					continue;
				}

				if (j + 1 > index->n_uniq) {
					sql_print_error(
						"Index %s of %s has %lu columns"
					        " unique inside InnoDB, but "
						"MySQL is asking statistics for"
					        " %lu columns. Have you mixed "
						"up .frm files from different "
					       	"installations? "
						"See " REFMAN
						"innodb-troubleshooting.html\n",
						index->name,
						ib_table->name,
						(unsigned long)
						index->n_uniq, j + 1);
					break;
				}

				rec_per_key = innodb_rec_per_key(
					index, j, stats.records);

				/* Since MySQL seems to favor table scans
				too much over index searches, we pretend
				index selectivity is 2 times better than
				our estimate: */

				rec_per_key = rec_per_key / 2;

				if (rec_per_key == 0) {
					rec_per_key = 1;
				}

				table->key_info[i].rec_per_key[j] =
				  rec_per_key >= ~(ulong) 0 ? ~(ulong) 0 :
				  (ulong) rec_per_key;
			}
		}

		if (!(flag & HA_STATUS_NO_LOCK)) {
			dict_table_stats_unlock(ib_table, RW_S_LATCH);
		}

		my_snprintf(path, sizeof(path), "%s/%s%s",
			    mysql_data_home,
			    table->s->normalized_path.str,
			    reg_ext);

		unpack_filename(path,path);

		/* Note that we do not know the access time of the table,
		nor the CHECK TABLE time, nor the UPDATE or INSERT time. */

		if (os_file_get_status(path, &stat_info, false) == DB_SUCCESS) {
			stats.create_time = (ulong) stat_info.ctime;
		}
	}

	if (srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE) {

		goto func_exit;
	}

	if (flag & HA_STATUS_ERRKEY) {
		const dict_index_t*	err_index;

		ut_a(prebuilt->trx);
		ut_a(prebuilt->trx->magic_n == TRX_MAGIC_N);

		err_index = trx_get_error_info(prebuilt->trx);

		if (err_index) {
			errkey = innobase_get_mysql_key_number_for_index(
					share, table, ib_table, err_index);
		} else {
			errkey = (unsigned int) (
				(prebuilt->trx->error_key_num
				 == ULINT_UNDEFINED)
					? ~0
					: prebuilt->trx->error_key_num);
		}
	}

	if ((flag & HA_STATUS_AUTO) && table->found_next_number_field) {
		stats.auto_increment_value = innobase_peek_autoinc();
	}

func_exit:
	prebuilt->trx->op_info = (char*)"";

	DBUG_RETURN(0);
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */
UNIV_INTERN
int
ha_innobase::info(
/*==============*/
	uint	flag)	/*!< in: what information is requested */
{
	return(this->info_low(flag, false /* not ANALYZE */));
}

/**********************************************************************//**
Updates index cardinalities of the table, based on random dives into
each index tree. This does NOT calculate exact statistics on the table.
@return	HA_ADMIN_* error code or HA_ADMIN_OK */
UNIV_INTERN
int
ha_innobase::analyze(
/*=================*/
	THD*		thd,		/*!< in: connection thread handle */
	HA_CHECK_OPT*	check_opt)	/*!< in: currently ignored */
{
	int	ret;

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		return(HA_ADMIN_CORRUPT);
	}

	/* Simply call this->info_low() with all the flags
	and request recalculation of the statistics */
	ret = this->info_low(
		HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE,
		true /* this is ANALYZE */);

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		return(HA_ADMIN_CORRUPT);
	}

	if (ret != 0) {
		return(HA_ADMIN_FAILED);
	}

	return(HA_ADMIN_OK);
}

/**********************************************************************//**
This is mapped to "ALTER TABLE tablename ENGINE=InnoDB", which rebuilds
the table in MySQL. */
UNIV_INTERN
int
ha_innobase::optimize(
/*==================*/
	THD*		thd,		/*!< in: connection thread handle */
	HA_CHECK_OPT*	check_opt)	/*!< in: currently ignored */
{
	/*FTS-FIXME: Since MySQL doesn't support engine-specific commands,
	we have to hijack some existing command in order to be able to test
	the new admin commands added in InnoDB's FTS support. For now, we
	use MySQL's OPTIMIZE command, normally mapped to ALTER TABLE in
	InnoDB (so it recreates the table anew), and map it to OPTIMIZE.

	This works OK otherwise, but MySQL locks the entire table during
	calls to OPTIMIZE, which is undesirable. */

	if (innodb_optimize_fulltext_only) {
		if (prebuilt->table->fts && prebuilt->table->fts->cache
		    && !dict_table_is_discarded(prebuilt->table)) {
			fts_sync_table(prebuilt->table, false, true, false);
			fts_optimize_table(prebuilt->table);
		}
		return(HA_ADMIN_OK);
	} else {

		return(HA_ADMIN_TRY_ALTER);
	}
}

/*******************************************************************//**
Tries to check that an InnoDB table is not corrupted. If corruption is
noticed, prints to stderr information about it. In case of corruption
may also assert a failure and crash the server.
@return	HA_ADMIN_CORRUPT or HA_ADMIN_OK */
UNIV_INTERN
int
ha_innobase::check(
/*===============*/
	THD*		thd,		/*!< in: user thread handle */
	HA_CHECK_OPT*	check_opt)	/*!< in: check options */
{
	dict_index_t*	index;
	ulint		n_rows;
	ulint		n_rows_in_table	= ULINT_UNDEFINED;
	bool		is_ok		= true;
	ulint		old_isolation_level;
	ibool		table_corrupted;

	DBUG_ENTER("ha_innobase::check");
	DBUG_ASSERT(thd == ha_thd());
	ut_a(prebuilt->trx);
	ut_a(prebuilt->trx->magic_n == TRX_MAGIC_N);
	ut_a(prebuilt->trx == thd_to_trx(thd));

	if (prebuilt->mysql_template == NULL) {
		/* Build the template; we will use a dummy template
		in index scans done in checking */

		build_template(true);
	}

	if (dict_table_is_discarded(prebuilt->table)) {

		ib_senderrf(
			thd,
			IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);

	} else if (prebuilt->table->ibd_file_missing) {

		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);
	}

	prebuilt->trx->op_info = "checking table";

	old_isolation_level = prebuilt->trx->isolation_level;

	/* We must run the index record counts at an isolation level
	>= READ COMMITTED, because a dirty read can see a wrong number
	of records in some index; to play safe, we use always
	REPEATABLE READ here */

	prebuilt->trx->isolation_level = TRX_ISO_REPEATABLE_READ;

	/* Check whether the table is already marked as corrupted
	before running the check table */
	table_corrupted = prebuilt->table->corrupted;

	/* Reset table->corrupted bit so that check table can proceed to
	do additional check */
	prebuilt->table->corrupted = FALSE;

	for (index = dict_table_get_first_index(prebuilt->table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {
		char	index_name[MAX_FULL_NAME_LEN + 1];

		/* If this is an index being created or dropped, skip */
		if (*index->name == TEMP_INDEX_PREFIX) {
			continue;
		}

		if (!(check_opt->flags & T_QUICK)) {
			/* Enlarge the fatal lock wait timeout during
			CHECK TABLE. */
			os_increment_counter_by_amount(
				server_mutex,
				srv_fatal_semaphore_wait_threshold,
				SRV_SEMAPHORE_WAIT_EXTENSION);
			bool valid = btr_validate_index(index, prebuilt->trx);

			/* Restore the fatal lock wait timeout after
			CHECK TABLE. */
			os_decrement_counter_by_amount(
				server_mutex,
				srv_fatal_semaphore_wait_threshold,
				SRV_SEMAPHORE_WAIT_EXTENSION);

			if (!valid) {
				is_ok = false;

				innobase_format_name(
					index_name, sizeof index_name,
					index->name, TRUE);
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_NOT_KEYFILE,
					"InnoDB: The B-tree of"
					" index %s is corrupted.",
					index_name);
				continue;
			}
		}

		/* Instead of invoking change_active_index(), set up
		a dummy template for non-locking reads, disabling
		access to the clustered index. */
		prebuilt->index = index;

		prebuilt->index_usable = row_merge_is_index_usable(
			prebuilt->trx, prebuilt->index);

		if (UNIV_UNLIKELY(!prebuilt->index_usable)) {
			innobase_format_name(
				index_name, sizeof index_name,
				prebuilt->index->name, TRUE);

			if (dict_index_is_corrupted(prebuilt->index)) {
				push_warning_printf(
					user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_INDEX_CORRUPT,
					"InnoDB: Index %s is marked as"
					" corrupted",
					index_name);
				is_ok = false;
			} else {
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_TABLE_DEF_CHANGED,
					"InnoDB: Insufficient history for"
					" index %s",
					index_name);
			}
			continue;
		}

		prebuilt->sql_stat_start = TRUE;
		prebuilt->template_type = ROW_MYSQL_DUMMY_TEMPLATE;
		prebuilt->n_template = 0;
		prebuilt->need_to_access_clustered = FALSE;

		dtuple_set_n_fields(prebuilt->search_tuple, 0);

		prebuilt->select_lock_type = LOCK_NONE;

		bool check_result
			= row_check_index_for_mysql(prebuilt, index, &n_rows);
		DBUG_EXECUTE_IF(
				"dict_set_index_corrupted",
				if (!(index->type & DICT_CLUSTERED)) {
					check_result = false;
				});
		if (!check_result) {
			innobase_format_name(
				index_name, sizeof index_name,
				index->name, TRUE);

			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: The B-tree of"
				" index %s is corrupted.",
				index_name);
			is_ok = false;
			dict_set_corrupted(
				index, prebuilt->trx, "CHECK TABLE-check index");
		}

		if (thd_killed(user_thd)) {
			break;
		}

#if 0
		fprintf(stderr, "%lu entries in index %s\n", n_rows,
			index->name);
#endif

		if (index == dict_table_get_first_index(prebuilt->table)) {
			n_rows_in_table = n_rows;
		} else if (!(index->type & DICT_FTS)
			   && (n_rows != n_rows_in_table)) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: Index '%-.200s' contains %lu"
				" entries, should be %lu.",
				index->name,
				(ulong) n_rows,
				(ulong) n_rows_in_table);
			is_ok = false;
			dict_set_corrupted(
				index, prebuilt->trx,
				"CHECK TABLE; Wrong count");
		}
	}

	if (table_corrupted) {
		/* If some previous operation has marked the table as
		corrupted in memory, and has not propagated such to
		clustered index, we will do so here */
		index = dict_table_get_first_index(prebuilt->table);

		if (!dict_index_is_corrupted(index)) {
			dict_set_corrupted(
				index, prebuilt->trx, "CHECK TABLE");
		}
		prebuilt->table->corrupted = TRUE;
	}

	/* Restore the original isolation level */
	prebuilt->trx->isolation_level = old_isolation_level;

	/* We validate the whole adaptive hash index for all tables
	at every CHECK TABLE only when QUICK flag is not present. */

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	if (!(check_opt->flags & T_QUICK) && !btr_search_validate()) {
		push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_NOT_KEYFILE,
			     "InnoDB: The adaptive hash index is corrupted.");
		is_ok = false;
	}
#endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */

	prebuilt->trx->op_info = "";
	if (thd_killed(user_thd)) {
		thd_set_kill_status(user_thd);
	}

	if (UNIV_UNLIKELY(prebuilt->table && prebuilt->table->corrupted)) {
		DBUG_RETURN(HA_ADMIN_CORRUPT);
	}

	DBUG_RETURN(is_ok ? HA_ADMIN_OK : HA_ADMIN_CORRUPT);
}

/*************************************************************//**
Adds information about free space in the InnoDB tablespace to a table comment
which is printed out when a user calls SHOW TABLE STATUS. Adds also info on
foreign keys.
@return	table comment + InnoDB free space + info on foreign keys */
UNIV_INTERN
char*
ha_innobase::update_table_comment(
/*==============================*/
	const char*	comment)/*!< in: table comment defined by user */
{
	uint	length = (uint) strlen(comment);
	char*	str;
	long	flen;

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	if (length > 64000 - 3) {
		return((char*) comment); /* string too long */
	}

	update_thd(ha_thd());

	prebuilt->trx->op_info = (char*)"returning table comment";

	/* In case MySQL calls this in the middle of a SELECT query, release
	possible adaptive hash latch to avoid deadlocks of threads */

	trx_search_latch_release_if_reserved(prebuilt->trx);
	str = NULL;

	/* output the data to a temporary file */

	if (!srv_read_only_mode) {

		mutex_enter(&srv_dict_tmpfile_mutex);

		rewind(srv_dict_tmpfile);

		fprintf(srv_dict_tmpfile, "InnoDB free: %llu kB",
			fsp_get_available_space_in_free_extents(
				prebuilt->table->space));

		dict_print_info_on_foreign_keys(
			FALSE, srv_dict_tmpfile, prebuilt->trx,
			prebuilt->table);

		flen = ftell(srv_dict_tmpfile);

		if (flen < 0) {
			flen = 0;
		} else if (length + flen + 3 > 64000) {
			flen = 64000 - 3 - length;
		}

		/* allocate buffer for the full string, and
		read the contents of the temporary file */

		str = (char*) my_malloc(length + flen + 3, MYF(0));

		if (str) {
			char* pos	= str + length;
			if (length) {
				memcpy(str, comment, length);
				*pos++ = ';';
				*pos++ = ' ';
			}
			rewind(srv_dict_tmpfile);
			flen = (uint) fread(pos, 1, flen, srv_dict_tmpfile);
			pos[flen] = 0;
		}

		mutex_exit(&srv_dict_tmpfile_mutex);
	}

	prebuilt->trx->op_info = (char*)"";

	return(str ? str : (char*) comment);
}

/*******************************************************************//**
Gets the foreign key create info for a table stored in InnoDB.
@return own: character string in the form which can be inserted to the
CREATE TABLE statement, MUST be freed with
ha_innobase::free_foreign_key_create_info */
UNIV_INTERN
char*
ha_innobase::get_foreign_key_create_info(void)
/*==========================================*/
{
	long	flen;
	char*	str	= 0;

	ut_a(prebuilt != NULL);

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	prebuilt->trx->op_info = (char*)"getting info on foreign keys";

	/* In case MySQL calls this in the middle of a SELECT query,
	release possible adaptive hash latch to avoid
	deadlocks of threads */

	trx_search_latch_release_if_reserved(prebuilt->trx);

	if (!srv_read_only_mode) {
		mutex_enter(&srv_dict_tmpfile_mutex);
		rewind(srv_dict_tmpfile);

		/* Output the data to a temporary file */
		dict_print_info_on_foreign_keys(
			TRUE, srv_dict_tmpfile, prebuilt->trx,
			prebuilt->table);

		prebuilt->trx->op_info = (char*)"";

		flen = ftell(srv_dict_tmpfile);

		if (flen < 0) {
			flen = 0;
		}

		/* Allocate buffer for the string, and
		read the contents of the temporary file */

		str = (char*) my_malloc(flen + 1, MYF(0));

		if (str) {
			rewind(srv_dict_tmpfile);
			flen = (uint) fread(str, 1, flen, srv_dict_tmpfile);
			str[flen] = 0;
		}

		mutex_exit(&srv_dict_tmpfile_mutex);
	}

	return(str);
}


/***********************************************************************//**
Maps a InnoDB foreign key constraint to a equivalent MySQL foreign key info.
@return pointer to foreign key info */
static
FOREIGN_KEY_INFO*
get_foreign_key_info(
/*=================*/
	THD*			thd,		/*!< in: user thread handle */
	dict_foreign_t*		foreign)	/*!< in: foreign key constraint */
{
	FOREIGN_KEY_INFO	f_key_info;
	FOREIGN_KEY_INFO*	pf_key_info;
	uint			i = 0;
	ulint			len;
	char			tmp_buff[NAME_LEN+1];
	char			name_buff[NAME_LEN+1];
	const char*		ptr;
	LEX_STRING*		referenced_key_name;
	LEX_STRING*		name = NULL;

	ptr = dict_remove_db_name(foreign->id);
	f_key_info.foreign_id = thd_make_lex_string(thd, 0, ptr,
						    (uint) strlen(ptr), 1);

	/* Name format: database name, '/', table name, '\0' */

	/* Referenced (parent) database name */
	len = dict_get_db_name_len(foreign->referenced_table_name);
	ut_a(len < sizeof(tmp_buff));
	ut_memcpy(tmp_buff, foreign->referenced_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.referenced_db = thd_make_lex_string(
		thd, 0, name_buff, static_cast<unsigned int>(len), 1);

	/* Referenced (parent) table name */
	ptr = dict_remove_db_name(foreign->referenced_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff));
	f_key_info.referenced_table = thd_make_lex_string(
		thd, 0, name_buff, static_cast<unsigned int>(len), 1);

	/* Dependent (child) database name */
	len = dict_get_db_name_len(foreign->foreign_table_name);
	ut_a(len < sizeof(tmp_buff));
	ut_memcpy(tmp_buff, foreign->foreign_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.foreign_db = thd_make_lex_string(
		thd, 0, name_buff, static_cast<unsigned int>(len), 1);

	/* Dependent (child) table name */
	ptr = dict_remove_db_name(foreign->foreign_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff));
	f_key_info.foreign_table = thd_make_lex_string(
		thd, 0, name_buff, static_cast<unsigned int>(len), 1);

	do {
		ptr = foreign->foreign_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   (uint) strlen(ptr), 1);
		f_key_info.foreign_fields.push_back(name);
		ptr = foreign->referenced_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   (uint) strlen(ptr), 1);
		f_key_info.referenced_fields.push_back(name);
	} while (++i < foreign->n_fields);

	if (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE) {
		len = 7;
		ptr = "CASCADE";
	} else if (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL) {
		len = 8;
		ptr = "SET NULL";
	} else if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
		len = 9;
		ptr = "NO ACTION";
	} else {
		len = 8;
		ptr = "RESTRICT";
	}

	f_key_info.delete_method = thd_make_lex_string(
		thd, f_key_info.delete_method, ptr,
		static_cast<unsigned int>(len), 1);

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
		len = 7;
		ptr = "CASCADE";
	} else if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
		len = 8;
		ptr = "SET NULL";
	} else if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
		len = 9;
		ptr = "NO ACTION";
	} else {
		len = 8;
		ptr = "RESTRICT";
	}

	f_key_info.update_method = thd_make_lex_string(
		thd, f_key_info.update_method, ptr,
		static_cast<unsigned int>(len), 1);

	if (foreign->referenced_index && foreign->referenced_index->name) {
		referenced_key_name = thd_make_lex_string(thd,
					f_key_info.referenced_key_name,
					foreign->referenced_index->name,
					 (uint) strlen(foreign->referenced_index->name),
					1);
	} else {
		referenced_key_name = NULL;
	}

	f_key_info.referenced_key_name = referenced_key_name;

	pf_key_info = (FOREIGN_KEY_INFO*) thd_memdup(thd, &f_key_info,
						      sizeof(FOREIGN_KEY_INFO));

	return(pf_key_info);
}

/** Get the list of foreign keys referencing a specified table
table.
@param thd		The thread handle
@param path		Path to the table
@param f_key_list[out]	The list of foreign keys */
static
void
fill_foreign_key_list(THD* thd,
		      const dict_table_t* table,
		      List<FOREIGN_KEY_INFO>* f_key_list)
{
	ut_ad(mutex_own(&dict_sys->mutex));

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end(); ++it) {

		dict_foreign_t* foreign = *it;

		FOREIGN_KEY_INFO* pf_key_info
			= get_foreign_key_info(thd, foreign);
		if (pf_key_info) {
			f_key_list->push_back(pf_key_info);
		}
	}
}

/** Get the list of foreign keys referencing a specified table
table.
@param thd		The thread handle
@param path		Path to the table
@param f_key_list[out]	The list of foreign keys

@return error code or zero for success */
static
int
innobase_get_parent_fk_list(
	THD*			thd,
	const char*		path,
	List<FOREIGN_KEY_INFO>*	f_key_list)
{
	ut_a(strlen(path) <= FN_REFLEN);
	char	norm_name[FN_REFLEN + 1];
	normalize_table_name(norm_name, path);

	trx_t*	parent_trx = check_trx_exists(thd);
	parent_trx->op_info = "getting list of referencing foreign keys";
	trx_search_latch_release_if_reserved(parent_trx);

	mutex_enter(&dict_sys->mutex);

	dict_table_t*	table
		= dict_table_open_on_name(norm_name, TRUE, FALSE,
					  static_cast<dict_err_ignore_t>(
						  DICT_ERR_IGNORE_INDEX_ROOT
						  | DICT_ERR_IGNORE_CORRUPT));
	if (!table) {
		mutex_exit(&dict_sys->mutex);
		return(HA_ERR_NO_SUCH_TABLE);
	}

	fill_foreign_key_list(thd, table, f_key_list);

	dict_table_close(table, TRUE, FALSE);

	mutex_exit(&dict_sys->mutex);
	parent_trx->op_info = "";
	return(0);
}

/*******************************************************************//**
Gets the list of foreign keys in this table.
@return always 0, that is, always succeeds */
UNIV_INTERN
int
ha_innobase::get_foreign_key_list(
/*==============================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	FOREIGN_KEY_INFO*	pf_key_info;
	dict_foreign_t*		foreign;

	ut_a(prebuilt != NULL);
	update_thd(ha_thd());

	prebuilt->trx->op_info = "getting list of foreign keys";

	trx_search_latch_release_if_reserved(prebuilt->trx);

	mutex_enter(&(dict_sys->mutex));

	for (dict_foreign_set::iterator it
		= prebuilt->table->foreign_set.begin();
	     it != prebuilt->table->foreign_set.end();
	     ++it) {

		foreign = *it;

		pf_key_info = get_foreign_key_info(thd, foreign);
		if (pf_key_info) {
			f_key_list->push_back(pf_key_info);
		}
	}

	mutex_exit(&(dict_sys->mutex));

	prebuilt->trx->op_info = "";

	return(0);
}

/*******************************************************************//**
Gets the set of foreign keys where this table is the referenced table.
@return always 0, that is, always succeeds */
UNIV_INTERN
int
ha_innobase::get_parent_foreign_key_list(
/*=====================================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	ut_a(prebuilt != NULL);
	update_thd(ha_thd());

	prebuilt->trx->op_info = "getting list of referencing foreign keys";

	trx_search_latch_release_if_reserved(prebuilt->trx);

	mutex_enter(&(dict_sys->mutex));
	fill_foreign_key_list(thd, prebuilt->table, f_key_list);
	mutex_exit(&(dict_sys->mutex));

	prebuilt->trx->op_info = "";

	return(0);
}

/*****************************************************************//**
Checks if ALTER TABLE may change the storage engine of the table.
Changing storage engines is not allowed for tables for which there
are foreign key constraints (parent or child tables).
@return	TRUE if can switch engines */
UNIV_INTERN
bool
ha_innobase::can_switch_engines(void)
/*=================================*/
{
	bool	can_switch;

	DBUG_ENTER("ha_innobase::can_switch_engines");
	update_thd();

	prebuilt->trx->op_info =
			"determining if there are foreign key constraints";
	row_mysql_freeze_data_dictionary(prebuilt->trx);

	can_switch = prebuilt->table->referenced_set.empty()
		&& prebuilt->table->foreign_set.empty();

	row_mysql_unfreeze_data_dictionary(prebuilt->trx);
	prebuilt->trx->op_info = "";

	DBUG_RETURN(can_switch);
}

/*******************************************************************//**
Checks if a table is referenced by a foreign key. The MySQL manual states that
a REPLACE is either equivalent to an INSERT, or DELETE(s) + INSERT. Only a
delete is then allowed internally to resolve a duplicate key conflict in
REPLACE, not an update.
@return	> 0 if referenced by a FOREIGN KEY */
UNIV_INTERN
uint
ha_innobase::referenced_by_foreign_key(void)
/*========================================*/
{
	if (dict_table_is_referenced_by_foreign_key(prebuilt->table)) {

		return(1);
	}

	return(0);
}

/*******************************************************************//**
Frees the foreign key create info for a table stored in InnoDB, if it is
non-NULL. */
UNIV_INTERN
void
ha_innobase::free_foreign_key_create_info(
/*======================================*/
	char*	str)	/*!< in, own: create info string to free */
{
	if (str) {
		my_free(str);
	}
}

/*******************************************************************//**
Tells something additional to the handler about how to do things.
@return	0 or error number */
UNIV_INTERN
int
ha_innobase::extra(
/*===============*/
	enum ha_extra_function operation)
			   /*!< in: HA_EXTRA_FLUSH or some other flag */
{
	check_trx_exists(ha_thd());

	/* Warning: since it is not sure that MySQL calls external_lock
	before calling this function, the trx field in prebuilt can be
	obsolete! */

	switch (operation) {
	case HA_EXTRA_FLUSH:
		if (prebuilt->blob_heap) {
			row_mysql_prebuilt_free_blob_heap(prebuilt);
		}

		if (prebuilt->compress_heap) {
			row_mysql_prebuilt_free_compress_heap(prebuilt);
		}

		break;
	case HA_EXTRA_RESET_STATE:
		reset_template();
		thd_to_trx(ha_thd())->duplicates = 0;
		break;
	case HA_EXTRA_NO_KEYREAD:
		prebuilt->read_just_key = 0;
		break;
	case HA_EXTRA_KEYREAD:
		prebuilt->read_just_key = 1;
		break;
	case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
		prebuilt->keep_other_fields_on_keyread = 1;
		break;

		/* IMPORTANT: prebuilt->trx can be obsolete in
		this method, because it is not sure that MySQL
		calls external_lock before this method with the
		parameters below.  We must not invoke update_thd()
		either, because the calling threads may change.
		CAREFUL HERE, OR MEMORY CORRUPTION MAY OCCUR! */
	case HA_EXTRA_INSERT_WITH_UPDATE:
		thd_to_trx(ha_thd())->duplicates |= TRX_DUP_IGNORE;
		break;
	case HA_EXTRA_NO_IGNORE_DUP_KEY:
		thd_to_trx(ha_thd())->duplicates &= ~TRX_DUP_IGNORE;
		break;
	case HA_EXTRA_WRITE_CAN_REPLACE:
		thd_to_trx(ha_thd())->duplicates |= TRX_DUP_REPLACE;
		break;
	case HA_EXTRA_WRITE_CANNOT_REPLACE:
		thd_to_trx(ha_thd())->duplicates &= ~TRX_DUP_REPLACE;
		break;
	default:/* Do nothing */
		;
	}

	return(0);
}

/******************************************************************//**
*/
UNIV_INTERN
int
ha_innobase::reset()
/*================*/
{
	if (prebuilt->blob_heap) {
		row_mysql_prebuilt_free_blob_heap(prebuilt);
	}

	if (prebuilt->compress_heap) {
		row_mysql_prebuilt_free_compress_heap(prebuilt);
	}

	reset_template();
	ds_mrr.reset();

	/* TODO: This should really be reset in reset_template() but for now
	it's safer to do it explicitly here. */

	/* This is a statement level counter. */
	prebuilt->autoinc_last_value = 0;

	return(0);
}

/******************************************************************//**
MySQL calls this function at the start of each SQL statement inside LOCK
TABLES. Inside LOCK TABLES the ::external_lock method does not work to
mark SQL statement borders. Note also a special case: if a temporary table
is created inside LOCK TABLES, MySQL has not called external_lock() at all
on that table.
MySQL-5.0 also calls this before each statement in an execution of a stored
procedure. To make the execution more deterministic for binlogging, MySQL-5.0
locks all tables involved in a stored procedure with full explicit table
locks (thd_in_lock_tables(thd) holds in store_lock()) before executing the
procedure.
@return	0 or error code */
UNIV_INTERN
int
ha_innobase::start_stmt(
/*====================*/
	THD*		thd,	/*!< in: handle to the user thread */
	thr_lock_type	lock_type)
{
	trx_t*		trx;
	DBUG_ENTER("ha_innobase::start_stmt");

	update_thd(thd);

	trx = prebuilt->trx;

	/* Here we release the search latch and the InnoDB thread FIFO ticket
	if they were reserved. They should have been released already at the
	end of the previous statement, but because inside LOCK TABLES the
	lock count method does not work to mark the end of a SELECT statement,
	that may not be the case. We MUST release the search latch before an
	INSERT, for example. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* Reset the AUTOINC statement level counter for multi-row INSERTs. */
	trx->n_autoinc_rows = 0;

	prebuilt->sql_stat_start = TRUE;
	prebuilt->hint_need_to_fetch_extra_cols = 0;
	reset_template();

	if (dict_table_is_temporary(prebuilt->table)
	    && prebuilt->mysql_has_locked
	    && prebuilt->select_lock_type == LOCK_NONE) {
		dberr_t error;

		switch (thd_sql_command(thd)) {
		case SQLCOM_INSERT:
		case SQLCOM_UPDATE:
		case SQLCOM_DELETE:
		case SQLCOM_REPLACE:
			init_table_handle_for_HANDLER();
			prebuilt->select_lock_type = LOCK_X;
			prebuilt->stored_select_lock_type = LOCK_X;
			error = row_lock_table_for_mysql(prebuilt, NULL, 1);

			if (error != DB_SUCCESS) {
				int st = convert_error_code_to_mysql(
					error, 0, thd);
				DBUG_RETURN(st);
			}
			break;
		}
	}

	if (!prebuilt->mysql_has_locked) {
		/* This handle is for a temporary table created inside
		this same LOCK TABLES; since MySQL does NOT call external_lock
		in this case, we must use x-row locks inside InnoDB to be
		prepared for an update of a row */

		prebuilt->select_lock_type = LOCK_X;

	} else if (trx->isolation_level != TRX_ISO_SERIALIZABLE
		   && thd_sql_command(thd) == SQLCOM_SELECT
		   && lock_type == TL_READ) {

		/* For other than temporary tables, we obtain
		no lock for consistent read (plain SELECT). */

		prebuilt->select_lock_type = LOCK_NONE;
	} else {
		/* Not a consistent read: restore the
		select_lock_type value. The value of
		stored_select_lock_type was decided in:
		1) ::store_lock(),
		2) ::external_lock(),
		3) ::init_table_handle_for_HANDLER(), and
		4) ::transactional_table_lock(). */

		ut_a(prebuilt->stored_select_lock_type != LOCK_NONE_UNSET);
		prebuilt->select_lock_type = prebuilt->stored_select_lock_type;
	}

	*trx->detailed_error = 0;

	innobase_register_trx(ht, thd, trx);

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	DBUG_RETURN(0);
}

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return	InnoDB isolation level */
static inline
ulint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso)	/*!< in: MySQL isolation level code */
{
	switch (iso) {
	case ISO_REPEATABLE_READ:	return(TRX_ISO_REPEATABLE_READ);
	case ISO_READ_COMMITTED:	return(TRX_ISO_READ_COMMITTED);
	case ISO_SERIALIZABLE:		return(TRX_ISO_SERIALIZABLE);
	case ISO_READ_UNCOMMITTED:	return(TRX_ISO_READ_UNCOMMITTED);
	}

	ut_error;

	return(0);
}

/******************************************************************//**
As MySQL will execute an external lock for every new table it uses when it
starts to process an SQL statement (an exception is when MySQL calls
start_stmt for the handle) we can use this function to store the pointer to
the THD in the handle. We will also use this function to communicate
to InnoDB that a new SQL statement has started and that we must store a
savepoint to our transaction handle, so that we are able to roll back
the SQL statement in case of an error.
@return	0 */
UNIV_INTERN
int
ha_innobase::external_lock(
/*=======================*/
	THD*	thd,		/*!< in: handle to the user thread */
	int	lock_type)	/*!< in: lock type */
{
	trx_t*		trx;

	DBUG_ENTER("ha_innobase::external_lock");
	DBUG_PRINT("enter",("lock_type: %d", lock_type));

	update_thd(thd);

	/* Statement based binlogging does not work in isolation level
	READ UNCOMMITTED and READ COMMITTED since the necessary
	locks cannot be taken. In this case, we print an
	informative error message and return with an error.
	Note: decide_logging_format would give the same error message,
	except it cannot give the extra details. */

	if (lock_type == F_WRLCK
	    && !(table_flags() & HA_BINLOG_STMT_CAPABLE)
	    && thd_binlog_format(thd) == BINLOG_FORMAT_STMT
	    && thd_binlog_filter_ok(thd)
	    && thd_sqlcom_can_generate_row_events(thd)) {
		bool skip = 0;
		/* used by test case */
		DBUG_EXECUTE_IF("no_innodb_binlog_errors", skip = true;);
		if (!skip) {
#ifdef WITH_WSREP
		  if (!wsrep_on(thd) || wsrep_thd_exec_mode(thd) == LOCAL_STATE)
			{
#endif /* WITH_WSREP */
			my_error(ER_BINLOG_STMT_MODE_AND_ROW_ENGINE, MYF(0),
			         " InnoDB is limited to row-logging when "
			         "transaction isolation level is "
			         "READ COMMITTED or READ UNCOMMITTED.");
			DBUG_RETURN(HA_ERR_LOGGING_IMPOSSIBLE);
#ifdef WITH_WSREP
		}
#endif /* WITH_WSREP */
		}
	}

	/* Check for UPDATEs in read-only mode. */
	if (srv_read_only_mode
	    && (thd_sql_command(thd) == SQLCOM_UPDATE
		|| thd_sql_command(thd) == SQLCOM_INSERT
		|| thd_sql_command(thd) == SQLCOM_REPLACE
		|| thd_sql_command(thd) == SQLCOM_DROP_TABLE
		|| thd_sql_command(thd) == SQLCOM_ALTER_TABLE
		|| thd_sql_command(thd) == SQLCOM_OPTIMIZE
		|| (thd_sql_command(thd) == SQLCOM_CREATE_TABLE
		    && lock_type == F_WRLCK)
		|| thd_sql_command(thd) == SQLCOM_CREATE_INDEX
		|| thd_sql_command(thd) == SQLCOM_DROP_INDEX
		|| thd_sql_command(thd) == SQLCOM_DELETE)) {

		if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE)
		{
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_INNODB_READ_ONLY);
			DBUG_RETURN(HA_ERR_INNODB_READ_ONLY);
		} else {
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_READ_ONLY_MODE);
			DBUG_RETURN(HA_ERR_TABLE_READONLY);
		}

	}

	/* Check for UPDATEs in read-only mode. */
	if (srv_read_only_mode
	    && (thd_sql_command(thd) == SQLCOM_UPDATE
		|| thd_sql_command(thd) == SQLCOM_INSERT
		|| thd_sql_command(thd) == SQLCOM_REPLACE
		|| thd_sql_command(thd) == SQLCOM_DROP_TABLE
		|| thd_sql_command(thd) == SQLCOM_ALTER_TABLE
		|| thd_sql_command(thd) == SQLCOM_OPTIMIZE
		|| (thd_sql_command(thd) == SQLCOM_CREATE_TABLE
		    && lock_type == F_WRLCK)
		|| thd_sql_command(thd) == SQLCOM_CREATE_INDEX
		|| thd_sql_command(thd) == SQLCOM_DROP_INDEX
		|| thd_sql_command(thd) == SQLCOM_DELETE
		|| thd_sql_command(thd) ==
			SQLCOM_CREATE_COMPRESSION_DICTIONARY
		|| thd_sql_command(thd) ==
			SQLCOM_DROP_COMPRESSION_DICTIONARY)) {

		if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE)
		{
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_INNODB_READ_ONLY);
			DBUG_RETURN(HA_ERR_INNODB_READ_ONLY);
		} else {
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_READ_ONLY_MODE);
			DBUG_RETURN(HA_ERR_TABLE_READONLY);
		}

	}

	trx = prebuilt->trx;

	prebuilt->sql_stat_start = TRUE;
	prebuilt->hint_need_to_fetch_extra_cols = 0;

	reset_template();

	switch (prebuilt->table->quiesce) {
	case QUIESCE_START:
		/* Check for FLUSH TABLE t WITH READ LOCK; */
		if (!srv_read_only_mode
		    && thd_sql_command(thd) == SQLCOM_FLUSH
		    && lock_type == F_RDLCK) {

			row_quiesce_table_start(prebuilt->table, trx);

			/* Use the transaction instance to track UNLOCK
			TABLES. It can be done via START TRANSACTION; too
			implicitly. */

			++trx->flush_tables;
		}
		break;

	case QUIESCE_COMPLETE:
		/* Check for UNLOCK TABLES; implicit or explicit
		or trx interruption. */
		if (trx->flush_tables > 0
		    && (lock_type == F_UNLCK || trx_is_interrupted(trx))) {

			row_quiesce_table_complete(prebuilt->table, trx);

			ut_a(trx->flush_tables > 0);
			--trx->flush_tables;
		}

		break;

	case QUIESCE_NONE:
		break;
	}

	if (lock_type == F_WRLCK) {

		/* If this is a SELECT, then it is in UPDATE TABLE ...
		or SELECT ... FOR UPDATE */
		prebuilt->select_lock_type = LOCK_X;
		prebuilt->stored_select_lock_type = LOCK_X;
	}

	if (lock_type != F_UNLCK) {
		/* MySQL is setting a new table lock */

		*trx->detailed_error = 0;

		innobase_register_trx(ht, thd, trx);

		if (trx->isolation_level == TRX_ISO_SERIALIZABLE
		    && prebuilt->select_lock_type == LOCK_NONE
		    && thd_test_options(
			    thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

			/* To get serializable execution, we let InnoDB
			conceptually add 'LOCK IN SHARE MODE' to all SELECTs
			which otherwise would have been consistent reads. An
			exception is consistent reads in the AUTOCOMMIT=1 mode:
			we know that they are read-only transactions, and they
			can be serialized also if performed as consistent
			reads. */

			prebuilt->select_lock_type = LOCK_S;
			prebuilt->stored_select_lock_type = LOCK_S;
		}

		/* Starting from 4.1.9, no InnoDB table lock is taken in LOCK
		TABLES if AUTOCOMMIT=1. It does not make much sense to acquire
		an InnoDB table lock if it is released immediately at the end
		of LOCK TABLES, and InnoDB's table locks in that case cause
		VERY easily deadlocks.

		We do not set InnoDB table locks if user has not explicitly
		requested a table lock. Note that thd_in_lock_tables(thd)
		can hold in some cases, e.g., at the start of a stored
		procedure call (SQLCOM_CALL). */

		if (prebuilt->select_lock_type != LOCK_NONE) {

			if (thd_sql_command(thd) == SQLCOM_LOCK_TABLES
			    && THDVAR(thd, table_locks)
			    && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT)
			    && thd_in_lock_tables(thd)) {

				dberr_t	error = row_lock_table_for_mysql(
					prebuilt, NULL, 0);

				if (error != DB_SUCCESS) {
					DBUG_RETURN(
						convert_error_code_to_mysql(
							error, 0, thd));
				}
			}

			trx->mysql_n_tables_locked++;
		}

		trx->n_mysql_tables_in_use++;
		prebuilt->mysql_has_locked = TRUE;

		if (!trx_is_started(trx)
		    && (prebuilt->select_lock_type != LOCK_NONE
			|| prebuilt->stored_select_lock_type != LOCK_NONE)) {

			++trx->will_lock;
		}

		DBUG_RETURN(0);
	}

	/* MySQL is releasing a table lock */

	trx->n_mysql_tables_in_use--;
	prebuilt->mysql_has_locked = FALSE;

	/* Release a possible FIFO ticket and search latch. Since we
	may reserve the trx_sys->mutex, we have to release the search
	system latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* If the MySQL lock count drops to zero we know that the current SQL
	statement has ended */

	if (trx->n_mysql_tables_in_use == 0) {
#ifdef EXTENDED_SLOWLOG
		if (UNIV_UNLIKELY(trx->take_stats)) {
			increment_thd_innodb_stats(thd,
						   (unsigned long long) trx->id,
						   trx->io_reads,
						   trx->io_read,
						   trx->io_reads_wait_timer,
						   trx->lock_que_wait_timer,
						   trx->innodb_que_wait_timer,
						   trx->distinct_page_access);

			trx->io_reads = 0;
			trx->io_read = 0;
			trx->io_reads_wait_timer = 0;
			trx->lock_que_wait_timer = 0;
			trx->innodb_que_wait_timer = 0;
			trx->distinct_page_access = 0;
			if (trx->distinct_page_access_hash)
				memset(trx->distinct_page_access_hash, 0,
				       DPAH_SIZE);
		}
#endif

		trx->mysql_n_tables_locked = 0;
		prebuilt->used_in_HANDLER = FALSE;

		if (!thd_test_options(
				thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

			if (trx_is_started(trx)) {
				innobase_commit(ht, thd, TRUE);
			}

		} else if (trx->isolation_level <= TRX_ISO_READ_COMMITTED
			   && trx->global_read_view) {

			/* At low transaction isolation levels we let
			each consistent read set its own snapshot */

			read_view_close_for_mysql(trx);
		}
	}

	if (!trx_is_started(trx)
	    && (prebuilt->select_lock_type != LOCK_NONE
		|| prebuilt->stored_select_lock_type != LOCK_NONE)) {

		++trx->will_lock;
	}

	DBUG_RETURN(0);
}

/******************************************************************//**
With this function MySQL request a transactional lock to a table when
user issued query LOCK TABLES..WHERE ENGINE = InnoDB.
@return	error code */
UNIV_INTERN
int
ha_innobase::transactional_table_lock(
/*==================================*/
	THD*	thd,		/*!< in: handle to the user thread */
	int	lock_type)	/*!< in: lock type */
{
	trx_t*		trx;

	DBUG_ENTER("ha_innobase::transactional_table_lock");
	DBUG_PRINT("enter",("lock_type: %d", lock_type));

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(thd);

	if (UNIV_UNLIKELY(share->ib_table->is_corrupt)) {
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (!thd_tablespace_op(thd)) {

		if (dict_table_is_discarded(prebuilt->table)) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_DISCARDED,
				table->s->table_name.str);

		} else if (prebuilt->table->ibd_file_missing) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_MISSING,
				table->s->table_name.str);
		}

		DBUG_RETURN(HA_ERR_CRASHED);
	}

	trx = prebuilt->trx;

	prebuilt->sql_stat_start = TRUE;
	prebuilt->hint_need_to_fetch_extra_cols = 0;

	reset_template();

	if (lock_type == F_WRLCK) {
		prebuilt->select_lock_type = LOCK_X;
		prebuilt->stored_select_lock_type = LOCK_X;
	} else if (lock_type == F_RDLCK) {
		prebuilt->select_lock_type = LOCK_S;
		prebuilt->stored_select_lock_type = LOCK_S;
	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"MySQL is trying to set transactional table lock "
			"with corrupted lock type to table %s, lock type "
			"%d does not exist.",
			table->s->table_name.str, lock_type);

		DBUG_RETURN(HA_ERR_CRASHED);
	}

	/* MySQL is setting a new transactional table lock */

	innobase_register_trx(ht, thd, trx);

	if (THDVAR(thd, table_locks) && thd_in_lock_tables(thd)) {
		dberr_t	error;

		error = row_lock_table_for_mysql(prebuilt, NULL, 0);

		if (error != DB_SUCCESS) {
			DBUG_RETURN(
				convert_error_code_to_mysql(
					error, prebuilt->table->flags, thd));
		}

		if (thd_test_options(
			thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

			/* Store the current undo_no of the transaction
			so that we know where to roll back if we have
			to roll back the next SQL statement */

			trx_mark_sql_stat_end(trx);
		}
	}

	DBUG_RETURN(0);
}

/************************************************************************//**
Here we export InnoDB status variables to MySQL. */
static
void
innodb_export_status()
/*==================*/
{
	if (innodb_inited) {
		srv_export_innodb_status();
	}
}

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,	/*!< in: the innodb handlerton */
	THD*		thd,	/*!< in: the MySQL query thread of the caller */
	stat_print_fn*	stat_print)
{
	trx_t*			trx;
	static const char	truncated_msg[] = "... truncated...\n";
	const long		MAX_STATUS_SIZE = 1048576;
	ulint			trx_list_start = ULINT_UNDEFINED;
	ulint			trx_list_end = ULINT_UNDEFINED;
	bool			ret_val;

	DBUG_ENTER("innodb_show_status");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* We don't create the temp files or associated
	mutexes in read-only-mode */

	if (srv_read_only_mode) {
		DBUG_RETURN(0);
	}

	trx = check_trx_exists(thd);

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	/* We let the InnoDB Monitor to output at most MAX_STATUS_SIZE
	bytes of text. */

	char*	str;
	ssize_t	flen, usable_len;

	mutex_enter(&srv_monitor_file_mutex);
	rewind(srv_monitor_file);

	srv_printf_innodb_monitor(srv_monitor_file, FALSE,
				  &trx_list_start, &trx_list_end);

	os_file_set_eof(srv_monitor_file);

	if ((flen = ftell(srv_monitor_file)) < 0) {
		flen = 0;
	}

	if (flen > MAX_STATUS_SIZE) {
		usable_len = MAX_STATUS_SIZE;
		srv_truncated_status_writes++;
	} else {
		usable_len = flen;
	}

	/* allocate buffer for the string, and
	read the contents of the temporary file */

	if (!(str = (char*) my_malloc(usable_len + 1, MYF(0)))) {
		mutex_exit(&srv_monitor_file_mutex);
		DBUG_RETURN(1);
	}

	rewind(srv_monitor_file);

	if (flen < MAX_STATUS_SIZE) {
		/* Display the entire output. */
		flen = fread(str, 1, flen, srv_monitor_file);
	} else if (trx_list_end < (ulint) flen
		   && trx_list_start < trx_list_end
		   && trx_list_start + (flen - trx_list_end)
		   < MAX_STATUS_SIZE - sizeof truncated_msg - 1) {

		/* Omit the beginning of the list of active transactions. */
		ssize_t	len = fread(str, 1, trx_list_start, srv_monitor_file);

		memcpy(str + len, truncated_msg, sizeof truncated_msg - 1);
		len += sizeof truncated_msg - 1;
		usable_len = (MAX_STATUS_SIZE - 1) - len;
		fseek(srv_monitor_file,
		      static_cast<long>(flen - usable_len), SEEK_SET);
		len += fread(str + len, 1, usable_len, srv_monitor_file);
		flen = len;
	} else {
		/* Omit the end of the output. */
		flen = fread(str, 1, MAX_STATUS_SIZE - 1, srv_monitor_file);
	}

	mutex_exit(&srv_monitor_file_mutex);

	ret_val= stat_print(
		thd, innobase_hton_name,
		static_cast<uint>(strlen(innobase_hton_name)),
		STRING_WITH_LEN(""), str, static_cast<uint>(flen));

	my_free(str);

	DBUG_RETURN(ret_val);
}

/************************************************************************//**
Implements the SHOW MUTEX STATUS command.
@return 0 on success. */
static
int
innodb_mutex_show_status(
/*=====================*/
	handlerton*	hton,		/*!< in: the innodb handlerton */
	THD*		thd,		/*!< in: the MySQL query thread of the
					caller */
	stat_print_fn*	stat_print)	/*!< in: function for printing
					statistics */
{
	char		buf1[IO_SIZE];
	char		buf2[IO_SIZE];
	ib_mutex_t*	mutex;
	rw_lock_t*	lock;
	ulint		block_mutex_oswait_count = 0;
	ulint		block_lock_oswait_count = 0;
	ib_mutex_t*	block_mutex = NULL;
	rw_lock_t*	block_lock = NULL;
#ifdef UNIV_DEBUG
	ulint		rw_lock_count= 0;
	ulint		rw_lock_count_spin_loop= 0;
	ulint		rw_lock_count_spin_rounds= 0;
	ulint		rw_lock_count_os_wait= 0;
	ulint		rw_lock_count_os_yield= 0;
	ulonglong	rw_lock_wait_time= 0;
#endif /* UNIV_DEBUG */
	uint		buf1len;
	uint		buf2len;
	uint		hton_name_len;

	hton_name_len = (uint) strlen(innobase_hton_name);

	DBUG_ENTER("innodb_mutex_show_status");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	mutex_enter(&mutex_list_mutex);

	for (mutex = UT_LIST_GET_FIRST(mutex_list); mutex != NULL;
	     mutex = UT_LIST_GET_NEXT(list, mutex)) {
		if (mutex->count_os_wait == 0) {
			continue;
		}

		if (buf_pool_is_block_mutex(mutex)) {
			block_mutex = mutex;
			block_mutex_oswait_count += mutex->count_os_wait;
			continue;
		}

		buf1len= (uint) my_snprintf(buf1, sizeof(buf1), "%s",
				     mutex->cmutex_name);
		buf2len= (uint) my_snprintf(buf2, sizeof(buf2), "os_waits=%lu",
				     (ulong) mutex->count_os_wait);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len, buf1, buf1len,
			       buf2, buf2len)) {
			mutex_exit(&mutex_list_mutex);
			DBUG_RETURN(1);
		}
	}

	if (block_mutex) {
		buf1len = (uint) my_snprintf(buf1, sizeof buf1,
					     "combined %s",
					     block_mutex->cmutex_name);
		buf2len = (uint) my_snprintf(buf2, sizeof buf2,
					     "os_waits=%lu",
					     (ulong) block_mutex_oswait_count);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len, buf1, buf1len,
			       buf2, buf2len)) {
			mutex_exit(&mutex_list_mutex);
			DBUG_RETURN(1);
		}
	}

	mutex_exit(&mutex_list_mutex);

	mutex_enter(&rw_lock_list_mutex);

	for (lock = UT_LIST_GET_FIRST(rw_lock_list); lock != NULL;
	     lock = UT_LIST_GET_NEXT(list, lock)) {
		if (lock->count_os_wait == 0) {
			continue;
		}

		if (buf_pool_is_block_lock(lock)) {
			block_lock = lock;
			block_lock_oswait_count += lock->count_os_wait;
			continue;
		}

		buf1len = (uint) my_snprintf(
			buf1, sizeof buf1, "%s",
			lock->lock_name);
		buf2len = (uint) my_snprintf(
			buf2, sizeof buf2, "os_waits=%lu",
			static_cast<ulong>(lock->count_os_wait));

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len, buf1, buf1len,
			       buf2, buf2len)) {
			mutex_exit(&rw_lock_list_mutex);
			DBUG_RETURN(1);
		}
	}

	if (block_lock) {
		buf1len = (uint) my_snprintf(buf1, sizeof buf1,
					     "combined %s",
					     block_lock->lock_name);
		buf2len = (uint) my_snprintf(buf2, sizeof buf2,
					     "os_waits=%lu",
					     (ulong) block_lock_oswait_count);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len, buf1, buf1len,
			       buf2, buf2len)) {
			mutex_exit(&rw_lock_list_mutex);
			DBUG_RETURN(1);
		}
	}

	mutex_exit(&rw_lock_list_mutex);

#ifdef UNIV_DEBUG
	buf2len = static_cast<uint>(my_snprintf(buf2, sizeof buf2,
			     "count=%lu, spin_waits=%lu, spin_rounds=%lu, "
			     "os_waits=%lu, os_yields=%lu, os_wait_times=%lu",
			      (ulong) rw_lock_count,
			      (ulong) rw_lock_count_spin_loop,
			      (ulong) rw_lock_count_spin_rounds,
			      (ulong) rw_lock_count_os_wait,
			      (ulong) rw_lock_count_os_yield,
			      (ulong) (rw_lock_wait_time / 1000)));

	if (stat_print(thd, innobase_hton_name, hton_name_len,
			STRING_WITH_LEN("rw_lock_mutexes"), buf2, buf2len)) {
		DBUG_RETURN(1);
	}
#endif /* UNIV_DEBUG */

	/* Success */
	DBUG_RETURN(0);
}

/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread
					of the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	switch (stat_type) {
	case HA_ENGINE_STATUS:
		/* Non-zero return value means there was an error. */
		return(innodb_show_status(hton, thd, stat_print) != 0);

	case HA_ENGINE_MUTEX:
		/* Non-zero return value means there was an error. */
		return(innodb_mutex_show_status(hton, thd, stat_print) != 0);

	case HA_ENGINE_LOGS:
		/* Not handled */
		break;
	}

	/* Success */
	return(false);
}

/************************************************************************//**
Handling the shared INNOBASE_SHARE structure that is needed to provide table
locking. Register the table name if it doesn't exist in the hash table. */
static
INNOBASE_SHARE*
get_share(
/*======*/
	const char*	table_name)
{
	INNOBASE_SHARE*	share;

	mysql_mutex_lock(&innobase_share_mutex);

	ulint	fold = ut_fold_string(table_name);

	HASH_SEARCH(table_name_hash, innobase_open_tables, fold,
		    INNOBASE_SHARE*, share,
		    ut_ad(share->use_count > 0),
		    !strcmp(share->table_name, table_name));

	if (!share) {

		uint length = (uint) strlen(table_name);

		/* TODO: invoke HASH_MIGRATE if innobase_open_tables
		grows too big */

		share = (INNOBASE_SHARE*) my_malloc(sizeof(*share)+length+1,
			MYF(MY_FAE | MY_ZEROFILL));

		share->table_name = (char*) memcpy(share + 1,
						   table_name, length + 1);

		HASH_INSERT(INNOBASE_SHARE, table_name_hash,
			    innobase_open_tables, fold, share);

		thr_lock_init(&share->lock);

		/* Index translation table initialization */
		share->idx_trans_tbl.index_mapping = NULL;
		share->idx_trans_tbl.index_count = 0;
		share->idx_trans_tbl.array_size = 0;
	}

	share->use_count++;
	mysql_mutex_unlock(&innobase_share_mutex);

	return(share);
}

/************************************************************************//**
Free the shared object that was registered with get_share(). */
static
void
free_share(
/*=======*/
	INNOBASE_SHARE*	share)	/*!< in/own: table share to free */
{
	mysql_mutex_lock(&innobase_share_mutex);

#ifdef UNIV_DEBUG
	INNOBASE_SHARE* share2;
	ulint	fold = ut_fold_string(share->table_name);

	HASH_SEARCH(table_name_hash, innobase_open_tables, fold,
		    INNOBASE_SHARE*, share2,
		    ut_ad(share->use_count > 0),
		    !strcmp(share->table_name, share2->table_name));

	ut_a(share2 == share);
#endif /* UNIV_DEBUG */

	if (!--share->use_count) {
		ulint	fold = ut_fold_string(share->table_name);

		HASH_DELETE(INNOBASE_SHARE, table_name_hash,
			    innobase_open_tables, fold, share);
		thr_lock_delete(&share->lock);

		/* Free any memory from index translation table */
		my_free(share->idx_trans_tbl.index_mapping);

		my_free(share);

		/* TODO: invoke HASH_MIGRATE if innobase_open_tables
		shrinks too much */
	}

	mysql_mutex_unlock(&innobase_share_mutex);
}

/*****************************************************************//**
Converts a MySQL table lock stored in the 'lock' field of the handle to
a proper type before storing pointer to the lock into an array of pointers.
MySQL also calls this if it wants to reset some table locks to a not-locked
state during the processing of an SQL query. An example is that during a
SELECT the read lock is released early on the 'const' tables where we only
fetch one row. MySQL does not call this when it releases all locks at the
end of an SQL statement.
@return	pointer to the next element in the 'to' array */
UNIV_INTERN
THR_LOCK_DATA**
ha_innobase::store_lock(
/*====================*/
	THD*			thd,		/*!< in: user thread handle */
	THR_LOCK_DATA**		to,		/*!< in: pointer to an array
						of pointers to lock structs;
						pointer to the 'lock' field
						of current handle is stored
						next to this array */
	enum thr_lock_type	lock_type)	/*!< in: lock type to store in
						'lock'; this may also be
						TL_IGNORE */
{
	trx_t*		trx;

	/* Note that trx in this function is NOT necessarily prebuilt->trx
	because we call update_thd() later, in ::external_lock()! Failure to
	understand this caused a serious memory corruption bug in 5.1.11. */

	trx = check_trx_exists(thd);

	/* NOTE: MySQL can call this function with lock 'type' TL_IGNORE!
	Be careful to ignore TL_IGNORE if we are going to do something with
	only 'real' locks! */

	/* If no MySQL table is in use, we need to set the isolation level
	of the transaction. */

	if (lock_type != TL_IGNORE
	    && trx->n_mysql_tables_in_use == 0) {
		trx->isolation_level = innobase_map_isolation_level(
			(enum_tx_isolation) thd_tx_isolation(thd));

		if (trx->isolation_level <= TRX_ISO_READ_COMMITTED
		    && trx->global_read_view) {

			/* At low transaction isolation levels we let
			each consistent read set its own snapshot */

			read_view_close_for_mysql(trx);
		}
	}

	DBUG_ASSERT(EQ_CURRENT_THD(thd));
	const bool in_lock_tables = thd_in_lock_tables(thd);
	const uint sql_command = thd_sql_command(thd);

	if (srv_read_only_mode
	    && (sql_command == SQLCOM_UPDATE
		|| sql_command == SQLCOM_INSERT
		|| sql_command == SQLCOM_REPLACE
		|| sql_command == SQLCOM_DROP_TABLE
		|| sql_command == SQLCOM_ALTER_TABLE
		|| sql_command == SQLCOM_OPTIMIZE
		|| (sql_command == SQLCOM_CREATE_TABLE
		    && (lock_type >= TL_WRITE_CONCURRENT_INSERT
			 && lock_type <= TL_WRITE))
		|| sql_command == SQLCOM_CREATE_INDEX
		|| sql_command == SQLCOM_DROP_INDEX
		|| sql_command == SQLCOM_DELETE
		|| sql_command == SQLCOM_CREATE_COMPRESSION_DICTIONARY
		|| sql_command == SQLCOM_DROP_COMPRESSION_DICTIONARY)) {

		ib_senderrf(trx->mysql_thd,
			    IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);

	} else if (sql_command == SQLCOM_FLUSH
		   && lock_type == TL_READ_NO_INSERT) {

		/* Check for FLUSH TABLES ... WITH READ LOCK */

		/* Note: This call can fail, but there is no way to return
		the error to the caller. We simply ignore it for now here
		and push the error code to the caller where the error is
		detected in the function. */

		dberr_t	err = row_quiesce_set_state(
			prebuilt->table, QUIESCE_START, trx);

		ut_a(err == DB_SUCCESS || err == DB_UNSUPPORTED);

		if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
			prebuilt->select_lock_type = LOCK_S;
			prebuilt->stored_select_lock_type = LOCK_S;
		} else {
			prebuilt->select_lock_type = LOCK_NONE;
			prebuilt->stored_select_lock_type = LOCK_NONE;
		}

	/* Check for DROP TABLE */
	} else if (sql_command == SQLCOM_DROP_TABLE) {

		/* MySQL calls this function in DROP TABLE though this table
		handle may belong to another thd that is running a query. Let
		us in that case skip any changes to the prebuilt struct. */

	/* Check for LOCK TABLE t1,...,tn WITH SHARED LOCKS */
	} else if ((lock_type == TL_READ && in_lock_tables)
		   || (lock_type == TL_READ_HIGH_PRIORITY && in_lock_tables)
		   || lock_type == TL_READ_WITH_SHARED_LOCKS
		   || lock_type == TL_READ_NO_INSERT
		   || (lock_type != TL_IGNORE
		       && sql_command != SQLCOM_SELECT)) {

		/* The OR cases above are in this order:
		1) MySQL is doing LOCK TABLES ... READ LOCAL, or we
		are processing a stored procedure or function, or
		2) (we do not know when TL_READ_HIGH_PRIORITY is used), or
		3) this is a SELECT ... IN SHARE MODE, or
		4) we are doing a complex SQL statement like
		INSERT INTO ... SELECT ... and the logical logging (MySQL
		binlog) requires the use of a locking read, or
		MySQL is doing LOCK TABLES ... READ.
		5) we let InnoDB do locking reads for all SQL statements that
		are not simple SELECTs; note that select_lock_type in this
		case may get strengthened in ::external_lock() to LOCK_X.
		Note that we MUST use a locking read in all data modifying
		SQL statements, because otherwise the execution would not be
		serializable, and also the results from the update could be
		unexpected if an obsolete consistent read view would be
		used. */

		/* Use consistent read for checksum table */

		if (sql_command == SQLCOM_CHECKSUM
		    || ((srv_locks_unsafe_for_binlog
			|| trx->isolation_level <= TRX_ISO_READ_COMMITTED)
			&& trx->isolation_level != TRX_ISO_SERIALIZABLE
			&& (lock_type == TL_READ
			    || lock_type == TL_READ_NO_INSERT)
			&& (sql_command == SQLCOM_INSERT_SELECT
			    || sql_command == SQLCOM_REPLACE_SELECT
			    || sql_command == SQLCOM_UPDATE
			    || sql_command == SQLCOM_CREATE_TABLE))) {

			/* If we either have innobase_locks_unsafe_for_binlog
			option set or this session is using READ COMMITTED
			isolation level and isolation level of the transaction
			is not set to serializable and MySQL is doing
			INSERT INTO...SELECT or REPLACE INTO...SELECT
			or UPDATE ... = (SELECT ...) or CREATE  ...
			SELECT... without FOR UPDATE or IN SHARE
			MODE in select, then we use consistent read
			for select. */

			prebuilt->select_lock_type = LOCK_NONE;
			prebuilt->stored_select_lock_type = LOCK_NONE;
		} else {
			prebuilt->select_lock_type = LOCK_S;
			prebuilt->stored_select_lock_type = LOCK_S;
		}

	} else if (lock_type != TL_IGNORE) {

		/* We set possible LOCK_X value in external_lock, not yet
		here even if this would be SELECT ... FOR UPDATE */

		prebuilt->select_lock_type = LOCK_NONE;
		prebuilt->stored_select_lock_type = LOCK_NONE;
	}

	if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) {

		/* Starting from 5.0.7, we weaken also the table locks
		set at the start of a MySQL stored procedure call, just like
		we weaken the locks set at the start of an SQL statement.
		MySQL does set in_lock_tables TRUE there, but in reality
		we do not need table locks to make the execution of a
		single transaction stored procedure call deterministic
		(if it does not use a consistent read). */

		if (lock_type == TL_READ
		    && sql_command == SQLCOM_LOCK_TABLES) {
			/* We come here if MySQL is processing LOCK TABLES
			... READ LOCAL. MyISAM under that table lock type
			reads the table as it was at the time the lock was
			granted (new inserts are allowed, but not seen by the
			reader). To get a similar effect on an InnoDB table,
			we must use LOCK TABLES ... READ. We convert the lock
			type here, so that for InnoDB, READ LOCAL is
			equivalent to READ. This will change the InnoDB
			behavior in mysqldump, so that dumps of InnoDB tables
			are consistent with dumps of MyISAM tables. */

			lock_type = TL_READ_NO_INSERT;
		}

		/* If we are not doing a LOCK TABLE, DISCARD/IMPORT
		TABLESPACE or TRUNCATE TABLE then allow multiple
		writers. Note that ALTER TABLE uses a TL_WRITE_ALLOW_READ
		< TL_WRITE_CONCURRENT_INSERT.

		We especially allow multiple writers if MySQL is at the
		start of a stored procedure call (SQLCOM_CALL) or a
		stored function call (MySQL does have in_lock_tables
		TRUE there). */

		if ((lock_type >= TL_WRITE_CONCURRENT_INSERT
		     && lock_type <= TL_WRITE)
		    && !(in_lock_tables
			 && sql_command == SQLCOM_LOCK_TABLES)
		    && !thd_tablespace_op(thd)
		    && sql_command != SQLCOM_TRUNCATE
		    && sql_command != SQLCOM_OPTIMIZE
		    && sql_command != SQLCOM_CREATE_TABLE) {

			lock_type = TL_WRITE_ALLOW_WRITE;
		}

		/* In queries of type INSERT INTO t1 SELECT ... FROM t2 ...
		MySQL would use the lock TL_READ_NO_INSERT on t2, and that
		would conflict with TL_WRITE_ALLOW_WRITE, blocking all inserts
		to t2. Convert the lock to a normal read lock to allow
		concurrent inserts to t2.

		We especially allow concurrent inserts if MySQL is at the
		start of a stored procedure call (SQLCOM_CALL)
		(MySQL does have thd_in_lock_tables() TRUE there). */

		if (lock_type == TL_READ_NO_INSERT
		    && sql_command != SQLCOM_LOCK_TABLES) {

			lock_type = TL_READ;
		}

		lock.type = lock_type;
	}

	*to++= &lock;

	if (!trx_is_started(trx)
	    && (prebuilt->select_lock_type != LOCK_NONE
	        || prebuilt->stored_select_lock_type != LOCK_NONE)) {

		++trx->will_lock;
	}

	return(to);
}

/*********************************************************************//**
Read the next autoinc value. Acquire the relevant locks before reading
the AUTOINC value. If SUCCESS then the table AUTOINC mutex will be locked
on return and all relevant locks acquired.
@return	DB_SUCCESS or error code */
UNIV_INTERN
dberr_t
ha_innobase::innobase_get_autoinc(
/*==============================*/
	ulonglong*	value)		/*!< out: autoinc value */
{
	*value = 0;

	prebuilt->autoinc_error = innobase_lock_autoinc();

	if (prebuilt->autoinc_error == DB_SUCCESS) {

		/* Determine the first value of the interval */
		*value = dict_table_autoinc_read(prebuilt->table);

		/* It should have been initialized during open. */
		if (*value == 0) {
			prebuilt->autoinc_error = DB_UNSUPPORTED;
			dict_table_autoinc_unlock(prebuilt->table);
		}
	}

	return(prebuilt->autoinc_error);
}

/*******************************************************************//**
This function reads the global auto-inc counter. It doesn't use the
AUTOINC lock even if the lock mode is set to TRADITIONAL.
@return	the autoinc value */
UNIV_INTERN
ulonglong
ha_innobase::innobase_peek_autoinc(void)
/*====================================*/
{
	ulonglong	auto_inc;
	dict_table_t*	innodb_table;

	ut_a(prebuilt != NULL);
	ut_a(prebuilt->table != NULL);

	innodb_table = prebuilt->table;

	dict_table_autoinc_lock(innodb_table);

	auto_inc = dict_table_autoinc_read(innodb_table);

	if (auto_inc == 0) {
		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: AUTOINC next value generation "
			"is disabled for '%s'\n", innodb_table->name);
	}

	dict_table_autoinc_unlock(innodb_table);

	return(auto_inc);
}

/*********************************************************************//**
Returns the value of the auto-inc counter in *first_value and ~0 on failure. */
UNIV_INTERN
void
ha_innobase::get_auto_increment(
/*============================*/
	ulonglong	offset,			/*!< in: table autoinc offset */
	ulonglong	increment,		/*!< in: table autoinc
						increment */
	ulonglong	nb_desired_values,	/*!< in: number of values
						reqd */
	ulonglong*	first_value,		/*!< out: the autoinc value */
	ulonglong*	nb_reserved_values)	/*!< out: count of reserved
						values */
{
	trx_t*		trx;
	dberr_t		error;
	ulonglong	autoinc = 0;

	/* Prepare prebuilt->trx in the table handle */
	update_thd(ha_thd());

	error = innobase_get_autoinc(&autoinc);

	if (error != DB_SUCCESS) {
		*first_value = (~(ulonglong) 0);
		return;
	}

	/* This is a hack, since nb_desired_values seems to be accurate only
	for the first call to get_auto_increment() for multi-row INSERT and
	meaningless for other statements e.g, LOAD etc. Subsequent calls to
	this method for the same statement results in different values which
	don't make sense. Therefore we store the value the first time we are
	called and count down from that as rows are written (see write_row()).
	*/

	trx = prebuilt->trx;

	/* Note: We can't rely on *first_value since some MySQL engines,
	in particular the partition engine, don't initialize it to 0 when
	invoking this method. So we are not sure if it's guaranteed to
	be 0 or not. */

	/* We need the upper limit of the col type to check for
	whether we update the table autoinc counter or not. */
	ulonglong	col_max_value = innobase_get_int_col_max_value(
		table->next_number_field);

	/** The following logic is needed to avoid duplicate key error
	for autoincrement column.

	(1) InnoDB gives the current autoincrement value with respect
	to increment and offset value.

	(2) Basically it does compute_next_insert_id() logic inside InnoDB
	to avoid the current auto increment value changed by handler layer.

	(3) It is restricted only for insert operations. */

	if (increment > 1 && thd_sql_command(user_thd) != SQLCOM_ALTER_TABLE
	    && autoinc < col_max_value) {

		ulonglong	prev_auto_inc = autoinc;

		autoinc = ((autoinc - 1) + increment - offset)/ increment;

		autoinc = autoinc * increment + offset;

		/* If autoinc exceeds the col_max_value then reset
		to old autoinc value. Because in case of non-strict
		sql mode, boundary value is not considered as error. */

		if (autoinc >= col_max_value) {
			autoinc = prev_auto_inc;
		}

		ut_ad(autoinc > 0);
	}

	/* Called for the first time ? */
	if (trx->n_autoinc_rows == 0) {

		trx->n_autoinc_rows = (ulint) nb_desired_values;

		/* It's possible for nb_desired_values to be 0:
		e.g., INSERT INTO T1(C) SELECT C FROM T2; */
		if (nb_desired_values == 0) {

			trx->n_autoinc_rows = 1;
		}

		set_if_bigger(*first_value, autoinc);
	/* Not in the middle of a mult-row INSERT. */
	} else if (prebuilt->autoinc_last_value == 0) {
		set_if_bigger(*first_value, autoinc);
	/* Check for -ve values. */
	} else if (*first_value > col_max_value && trx->n_autoinc_rows > 0) {
		/* Set to next logical value. */
		ut_a(autoinc > trx->n_autoinc_rows);
		*first_value = (autoinc - trx->n_autoinc_rows) - 1;
	}

	*nb_reserved_values = trx->n_autoinc_rows;

	/* With old style AUTOINC locking we only update the table's
	AUTOINC counter after attempting to insert the row. */
	if (innobase_autoinc_lock_mode != AUTOINC_OLD_STYLE_LOCKING) {
		ulonglong	current;
		ulonglong	next_value;

		current = *first_value > col_max_value ? autoinc : *first_value;

		/* If the increment step of the auto increment column
		decreases then it is not affecting the immediate
		next value in the series. */
		if (prebuilt->autoinc_increment > increment) {

#ifdef WITH_WSREP
			WSREP_DEBUG("Refresh change in auto-inc configuration"
				    " from (off: %llu -> %llu)"
				    " and (inc: %llu -> %llu)."
				    " Re-align auto increment"
				    " value for table (%s)"
				    " THD: %lu, current: %llu, autoinc: %llu",
				    prebuilt->autoinc_offset, offset,
				    prebuilt->autoinc_increment, increment,
				    prebuilt->table->name,
				    wsrep_thd_thread_id(ha_thd()),
				    current, autoinc);
			if (!wsrep_on(ha_thd()))
			{
#endif /* WITH_WSREP */

			/* MySQL flow will construct last_inserted_id but PXC
			can't do so because any values in that range are
			potentially unsafe as they were reserved for other node
			and if other node has used them it will result in
			conflict. So PXC skip this readjustment logic
			and re-calibration too as there is no change in
			current autoinc value. */
			current = autoinc - prebuilt->autoinc_increment;

			current = innobase_next_autoinc(
				current, 1, increment, 1, col_max_value);
#ifdef WITH_WSREP
			}
#endif /* WITH_WSREP */

			dict_table_autoinc_initialize(prebuilt->table, current);

			*first_value = current;
		}

		/* Compute the last value in the interval */
		next_value = innobase_next_autoinc(
			current, *nb_reserved_values, increment, offset,
			col_max_value);

		prebuilt->autoinc_last_value = next_value;

		if (prebuilt->autoinc_last_value < *first_value) {
			*first_value = (~(ulonglong) 0);
		} else {
			/* Update the table autoinc variable */
			dict_table_autoinc_update_if_greater(
				prebuilt->table, prebuilt->autoinc_last_value);
		}
	} else {
		/* This will force write_row() into attempting an update
		of the table's AUTOINC counter. */
		prebuilt->autoinc_last_value = 0;
	}

	/* The increment to be used to increase the AUTOINC value, we use
	this in write_row() and update_row() to increase the autoinc counter
	for columns that are filled by the user. We need the offset and
	the increment. */
	prebuilt->autoinc_offset = offset;
	prebuilt->autoinc_increment = increment;

	dict_table_autoinc_unlock(prebuilt->table);
}

/*******************************************************************//**
Reset the auto-increment counter to the given value, i.e. the next row
inserted will get the given value. This is called e.g. after TRUNCATE
is emulated by doing a 'DELETE FROM t'. HA_ERR_WRONG_COMMAND is
returned by storage engines that don't support this operation.
@return	0 or error code */
UNIV_INTERN
int
ha_innobase::reset_auto_increment(
/*==============================*/
	ulonglong	value)		/*!< in: new value for table autoinc */
{
	DBUG_ENTER("ha_innobase::reset_auto_increment");

	dberr_t	error;

	update_thd(ha_thd());

	error = row_lock_table_autoinc_for_mysql(prebuilt);

	if (error != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
				    error, prebuilt->table->flags, user_thd));
	}

	/* The next value can never be 0. */
	if (value == 0) {
		value = 1;
	}

	innobase_reset_autoinc(value);

	DBUG_RETURN(0);
}

/*******************************************************************//**
See comment in handler.cc */
UNIV_INTERN
bool
ha_innobase::get_error_message(
/*===========================*/
	int	error,
	String*	buf)
{
	trx_t*	trx = check_trx_exists(ha_thd());

	buf->copy(trx->detailed_error, (uint) strlen(trx->detailed_error),
		system_charset_info);

	return(FALSE);
}

/*******************************************************************//**
  Retrieves the names of the table and the key for which there was a
  duplicate entry in the case of HA_ERR_FOREIGN_DUPLICATE_KEY.

  If any of the names is not available, then this method will return
  false and will not change any of child_table_name or child_key_name.

  @param child_table_name[out]    Table name
  @param child_table_name_len[in] Table name buffer size
  @param child_key_name[out]      Key name
  @param child_key_name_len[in]   Key name buffer size

  @retval  true                  table and key names were available
                                 and were written into the corresponding
                                 out parameters.
  @retval  false                 table and key names were not available,
                                 the out parameters were not touched.
*/
bool
ha_innobase::get_foreign_dup_key(
/*=============================*/
	char*	child_table_name,
	uint	child_table_name_len,
	char*	child_key_name,
	uint	child_key_name_len)
{
	const dict_index_t*	err_index;

	ut_a(prebuilt->trx != NULL);
	ut_a(prebuilt->trx->magic_n == TRX_MAGIC_N);

	err_index = trx_get_error_info(prebuilt->trx);

	if (err_index == NULL) {
		return(false);
	}
	/* else */

	/* copy table name (and convert from filename-safe encoding to
	system_charset_info) */
	char*	p;
	p = strchr(err_index->table->name, '/');
	/* strip ".../" prefix if any */
	if (p != NULL) {
		p++;
	} else {
		p = err_index->table->name;
	}
	uint	len;
	len = filename_to_tablename(p, child_table_name, child_table_name_len);
	child_table_name[len] = '\0';

	/* copy index name */
	ut_snprintf(child_key_name, child_key_name_len, "%s", err_index->name);

	return(true);
}

/*******************************************************************//**
Compares two 'refs'. A 'ref' is the (internal) primary key value of the row.
If there is no explicitly declared non-null unique key or a primary key, then
InnoDB internally uses the row id as the primary key.
@return	< 0 if ref1 < ref2, 0 if equal, else > 0 */
UNIV_INTERN
int
ha_innobase::cmp_ref(
/*=================*/
	const uchar*	ref1,	/*!< in: an (internal) primary key value in the
				MySQL key value format */
	const uchar*	ref2)	/*!< in: an (internal) primary key value in the
				MySQL key value format */
{
	enum_field_types mysql_type;
	Field*		field;
	KEY_PART_INFO*	key_part;
	KEY_PART_INFO*	key_part_end;
	uint		len1;
	uint		len2;
	int		result;

	if (prebuilt->clust_index_was_generated) {
		/* The 'ref' is an InnoDB row id */

		return(memcmp(ref1, ref2, DATA_ROW_ID_LEN));
	}

	/* Do a type-aware comparison of primary key fields. PK fields
	are always NOT NULL, so no checks for NULL are performed. */

	key_part = table->key_info[table->s->primary_key].key_part;

	key_part_end = key_part
			+ table->key_info[table->s->primary_key].user_defined_key_parts;

	for (; key_part != key_part_end; ++key_part) {
		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB) {

			/* In the MySQL key value format, a column prefix of
			a BLOB is preceded by a 2-byte length field */

			len1 = innobase_read_from_2_little_endian(ref1);
			len2 = innobase_read_from_2_little_endian(ref2);

			result = ((Field_blob*) field)->cmp(
				ref1 + 2, len1, ref2 + 2, len2);
		} else {
			result = field->key_cmp(ref1, ref2);
		}

		if (result) {

			return(result);
		}

		ref1 += key_part->store_length;
		ref2 += key_part->store_length;
	}

	return(0);
}

/*******************************************************************//**
Ask InnoDB if a query to a table can be cached.
@return	TRUE if query caching of the table is permitted */
UNIV_INTERN
my_bool
ha_innobase::register_query_cache_table(
/*====================================*/
	THD*		thd,		/*!< in: user thread handle */
	char*		table_key,	/*!< in: normalized path to the
					table */
	uint		key_length,	/*!< in: length of the normalized
					path to the table */
	qc_engine_callback*
			call_back,	/*!< out: pointer to function for
					checking if query caching
					is permitted */
	ulonglong	*engine_data)	/*!< in/out: data to call_back */
{
	*call_back = innobase_query_caching_of_table_permitted;
	*engine_data = 0;
	return(innobase_query_caching_of_table_permitted(thd, table_key,
							 key_length,
							 engine_data));
}

/*******************************************************************//**
Get the bin log name. */
UNIV_INTERN
const char*
ha_innobase::get_mysql_bin_log_name()
/*=================================*/
{
	return(trx_sys_mysql_bin_log_name);
}

/*******************************************************************//**
Get the bin log offset (or file position). */
UNIV_INTERN
ulonglong
ha_innobase::get_mysql_bin_log_pos()
/*================================*/
{
	/* trx... is ib_int64_t, which is a typedef for a 64-bit integer
	(__int64 or longlong) so it's ok to cast it to ulonglong. */

	return(trx_sys_mysql_bin_log_pos);
}

/******************************************************************//**
This function is used to find the storage length in bytes of the first n
characters for prefix indexes using a multibyte character set. The function
finds charset information and returns length of prefix_len characters in the
index field in bytes.
@return	number of bytes occupied by the first n characters */
UNIV_INTERN
ulint
innobase_get_at_most_n_mbchars(
/*===========================*/
	ulint charset_id,	/*!< in: character set id */
	ulint prefix_len,	/*!< in: prefix length in bytes of the index
				(this has to be divided by mbmaxlen to get the
				number of CHARACTERS n in the prefix) */
	ulint data_len,		/*!< in: length of the string in bytes */
	const char* str)	/*!< in: character string */
{
	ulint char_length;	/*!< character length in bytes */
	ulint n_chars;		/*!< number of characters in prefix */
	CHARSET_INFO* charset;	/*!< charset used in the field */

	charset = get_charset((uint) charset_id, MYF(MY_WME));

	ut_ad(charset);
	ut_ad(charset->mbmaxlen);

	/* Calculate how many characters at most the prefix index contains */

	n_chars = prefix_len / charset->mbmaxlen;

	/* If the charset is multi-byte, then we must find the length of the
	first at most n chars in the string. If the string contains less
	characters than n, then we return the length to the end of the last
	character. */

	if (charset->mbmaxlen > 1) {
		/* my_charpos() returns the byte length of the first n_chars
		characters, or a value bigger than the length of str, if
		there were not enough full characters in str.

		Why does the code below work:
		Suppose that we are looking for n UTF-8 characters.

		1) If the string is long enough, then the prefix contains at
		least n complete UTF-8 characters + maybe some extra
		characters + an incomplete UTF-8 character. No problem in
		this case. The function returns the pointer to the
		end of the nth character.

		2) If the string is not long enough, then the string contains
		the complete value of a column, that is, only complete UTF-8
		characters, and we can store in the column prefix index the
		whole string. */

		char_length = my_charpos(charset, str,
						str + data_len, (int) n_chars);
		if (char_length > data_len) {
			char_length = data_len;
		}
	} else {
		if (data_len < prefix_len) {
			char_length = data_len;
		} else {
			char_length = prefix_len;
		}
	}

	return(char_length);
}

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return	0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		prepare_trx)	/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
{
	int		error = 0;
	trx_t*		trx = check_trx_exists(thd);

	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* we use support_xa value as it was seen at transaction start
	time, not the current session variable value. Any possible changes
	to the session variable take effect only in the next transaction */
	if (!trx->support_xa) {

#ifdef WITH_WSREP
                thd_get_xid(thd, (MYSQL_XID*) &trx->xid);
#endif // WITH_WSREP
		return(0);
	}

	if (UNIV_UNLIKELY(trx->fake_changes)) {

		if (prepare_trx
		    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT
					  | OPTION_BEGIN))) {

			thd->get_stmt_da()->reset_diagnostics_area();
			return(HA_ERR_WRONG_COMMAND);
		}
		return(0);
	}

	thd_get_xid(thd, (MYSQL_XID*) &trx->xid);

	/* Release a possible FIFO ticket and search latch. Since we will
	reserve the trx_sys->mutex, we have to release the search system
	latch first to obey the latching order. */

	trx_search_latch_release_if_reserved(trx);

	innobase_srv_conc_force_exit_innodb(trx);

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {

		sql_print_error("Transaction not registered for MySQL 2PC, "
				"but transaction is active");
	}

	if (prepare_trx
	    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		/* We were instructed to prepare the whole transaction, or
		this is an SQL statement end and autocommit is on */

		ut_ad(trx_is_registered_for_2pc(trx));

		trx_prepare_for_mysql(trx);

		DBUG_EXECUTE_IF("crash_innodb_after_prepare",
				DBUG_SUICIDE(););

		error = 0;
	} else {
		/* We just mark the SQL statement ended and do not do a
		transaction prepare */

		/* If we had reserved the auto-inc lock for some
		table in this SQL statement we release it now */

		lock_unlock_table_autoinc(trx);

		/* Store the current undo_no of the transaction so that we
		know where to roll back if we have to roll back the next
		SQL statement */

		trx_mark_sql_stat_end(trx);
	}

	if (thd_sql_command(thd) != SQLCOM_XA_PREPARE
	    && (prepare_trx
		|| !thd_test_options(
			thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		/* For mysqlbackup to work the order of transactions in binlog
		and InnoDB must be the same. Consider the situation

		  thread1> prepare; write to binlog; ...
			  <context switch>
		  thread2> prepare; write to binlog; commit
		  thread1>			     ... commit

                The server guarantees that writes to the binary log
                and commits are in the same order, so we do not have
                to handle this case. */
	}

	return(error);
}

/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return	number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	XID*		xid_list,/*!< in/out: prepared transactions */
	uint		len)	/*!< in: number of slots in xid_list */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (len == 0 || xid_list == NULL) {

		return(0);
	}

	return(trx_recover_for_mysql(xid_list, len));
}

/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return	0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	handlerton*	hton,
	XID*		xid)	/*!< in: X/Open XA transaction identification */
{
	trx_t*	trx;

	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = trx_get_trx_by_xid(xid);
	trx->wsrep_recover_xid = xid;

	if (trx) {
		innobase_commit_low(trx);
		trx_free_for_background(trx);
		return(XA_OK);
	} else {
		return(XAER_NOTA);
	}
}

/*******************************************************************//**
This function is used to rollback one X/Open XA distributed transaction
which is in the prepared state
@return	0 or error number */
static
int
innobase_rollback_by_xid(
/*=====================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	XID*		xid)	/*!< in: X/Open XA transaction
				identification */
{
	trx_t*	trx;

	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = trx_get_trx_by_xid(xid);

	if (trx) {
		int	ret = innobase_rollback_trx(trx);
		trx_free_for_background(trx);
		return(ret);
	} else {
		return(XAER_NOTA);
	}
}

/*******************************************************************//**
Create a consistent view for a cursor based on current transaction
which is created if the corresponding MySQL thread still lacks one.
This consistent view is then used inside of MySQL when accessing records
using a cursor.
@return	pointer to cursor view or NULL */
static
void*
innobase_create_cursor_view(
/*========================*/
	handlerton*	hton,	/*!< in: innobase hton */
	THD*		thd)	/*!< in: user thread handle */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	return(read_cursor_view_create_for_mysql(check_trx_exists(thd)));
}

/*******************************************************************//**
Close the given consistent cursor view of a transaction and restore
global read view to a transaction read view. Transaction is created if the
corresponding MySQL thread still lacks one. */
static
void
innobase_close_cursor_view(
/*=======================*/
	handlerton*	hton,	/*!< in: innobase hton */
	THD*		thd,	/*!< in: user thread handle */
	void*		curview)/*!< in: Consistent read view to be closed */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	read_cursor_view_close_for_mysql(check_trx_exists(thd),
					 (cursor_view_t*) curview);
}

/*******************************************************************//**
Set the given consistent cursor view to a transaction which is created
if the corresponding MySQL thread still lacks one. If the given
consistent cursor view is NULL global read view of a transaction is
restored to a transaction read view. */
static
void
innobase_set_cursor_view(
/*=====================*/
	handlerton*	hton,	/*!< in: innobase hton */
	THD*		thd,	/*!< in: user thread handle */
	void*		curview)/*!< in: Consistent cursor view to be set */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	read_cursor_set_for_mysql(check_trx_exists(thd),
				  (cursor_view_t*) curview);
}

/*******************************************************************//**
*/
UNIV_INTERN
bool
ha_innobase::check_if_incompatible_data(
/*====================================*/
	HA_CREATE_INFO*	info,
	uint		table_changes)
{
	innobase_copy_frm_flags_from_create_info(prebuilt->table, info);

	if (table_changes != IS_EQUAL_YES) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that auto_increment value was not changed */
	if ((info->used_fields & HA_CREATE_USED_AUTO) &&
		info->auto_increment_value != 0) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that row format didn't change */
	if ((info->used_fields & HA_CREATE_USED_ROW_FORMAT)
	    && info->row_type != get_row_type()) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Specifying KEY_BLOCK_SIZE requests a rebuild of the table. */
	if (info->used_fields & HA_CREATE_USED_KEY_BLOCK_SIZE) {
		return(COMPATIBLE_DATA_NO);
	}

	return(COMPATIBLE_DATA_YES);
}

/** This function reads zip dict-related info from SYS_ZIP_DICT
and SYS_ZIP_DICT_COLS for all columns marked with
COLUMN_FORMAT_TYPE_COMPRESSED flag and updates
zip_dict_name / zip_dict_data for those which have associated
compression dictionaries.

@param part_name Full table name (including partition part).
		 Must be non-NULL only if called from ha_partition.
*/
UNIV_INTERN
void
ha_innobase::update_field_defs_with_zip_dict_info(const char *part_name)
{
	DBUG_ENTER("update_field_defs_with_zip_dict_info");
	ut_ad(!mutex_own(&dict_sys->mutex));

	char norm_name[FN_REFLEN];
	normalize_table_name(norm_name,
		part_name != 0 ? part_name :
			table_share->normalized_path.str);

	dict_table_t* ib_table = dict_table_open_on_name(
		norm_name, FALSE, FALSE, DICT_ERR_IGNORE_NONE);

	/* if dict_table_open_on_name() returns NULL, then it means that
	TABLE_SHARE is populated for a table being created and we can
	skip filling zip dict info here */
	if (ib_table == 0)
		DBUG_VOID_RETURN;

	table_id_t ib_table_id = ib_table->id;
	dict_table_close(ib_table, FALSE, FALSE);
	Field* field;
	for (uint i = 0; i < table_share->fields; ++i) {
		field = table_share->field[i];
		if (field->column_format() ==
		    COLUMN_FORMAT_TYPE_COMPRESSED) {
			bool reference_found = false;
			ulint dict_id = 0;
			switch (dict_get_dictionary_id_by_key(ib_table_id, i,
				&dict_id)) {
				case DB_SUCCESS:
					reference_found = true;
					break;
				case DB_RECORD_NOT_FOUND:
					reference_found = false;
					break;
				default:
					ut_error;
			}
			if (reference_found) {
				char* local_name = 0;
				ulint local_name_len = 0;
				char* local_data = 0;
				ulint local_data_len = 0;
				if (dict_get_dictionary_info_by_id(dict_id,
					&local_name, &local_name_len,
					&local_data, &local_data_len) !=
					DB_SUCCESS) {
					ut_error;
				}
				else {
					field->zip_dict_name.str =
						local_name;
					field->zip_dict_name.length =
						local_name_len;
					field->zip_dict_data.str =
						local_data;
					field->zip_dict_data.length =
						local_data_len;
				}
			}
			else {
				field->zip_dict_name = null_lex_cstr;
				field->zip_dict_data = null_lex_cstr;
			}
		}
	}
	DBUG_VOID_RETURN;
}

/****************************************************************//**
Update the system variable innodb_io_capacity_max using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_max_update(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);
	if (in_val < srv_io_capacity) {
		in_val = srv_io_capacity;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_io_capacity_max cannot be"
				    " set lower than innodb_io_capacity.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity_max to %lu",
				    srv_io_capacity);
	}

	srv_max_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_io_capacity using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_update(
/*======================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);
	if (in_val > srv_max_io_capacity) {
		in_val = srv_max_io_capacity;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_io_capacity cannot be set"
				    " higher than innodb_io_capacity_max.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity to %lu",
				    srv_max_io_capacity);
	}

	srv_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_log_arch_expire_sec using
the "saved" value. This function is registered as a callback with MySQL. */
static
void
innodb_log_archive_expire_update(
/*==============================*/
	THD*				thd,		/*!< in: thread handle */
	struct st_mysql_sys_var*	var,		/*!< in: pointer to
							system variable */
	void*				var_ptr,	/*!< out: unused */
	const void*			save)		/*!< in: immediate result
							from check function */
{
	srv_log_arch_expire_sec = *(ulint*) save;
}

static
void
innodb_log_archive_update(
/*======================*/
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save)
{
	if (srv_read_only_mode)
		return;

	my_bool	in_val = *static_cast<const my_bool*>(save);

	if (in_val) {
		/* turn archiving on */
		srv_log_archive_on = innobase_log_archive = 1;
		log_archive_archivelog();
	} else {
		/* turn archivng off */
		srv_log_archive_on = innobase_log_archive = 0;
		log_archive_noarchivelog();
	}
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_update(
/*==============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);
	if (in_val < srv_max_dirty_pages_pct_lwm) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct cannot be"
				    " set lower than"
				    " innodb_max_dirty_pages_pct_lwm.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Lowering"
				    " innodb_max_dirty_page_pct_lwm to %lu",
				    in_val);

		srv_max_dirty_pages_pct_lwm = in_val;
	}

	srv_max_buf_pool_modified_pct = in_val;
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct_lwm using the
"saved" value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_lwm_update(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);
	if (in_val > srv_max_buf_pool_modified_pct) {
		in_val = srv_max_buf_pool_modified_pct;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct_lwm"
				    " cannot be set higher than"
				    " innodb_max_dirty_pages_pct.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_max_dirty_page_pct_lwm"
				    " to %lu",
				    in_val);
	}

	srv_max_dirty_pages_pct_lwm = in_val;
}

/************************************************************//**
Validate the file format name and return its corresponding id.
@return	valid file format id */
static
uint
innobase_file_format_name_lookup(
/*=============================*/
	const char*	format_name)	/*!< in: pointer to file format name */
{
	char*	endp;
	uint	format_id;

	ut_a(format_name != NULL);

	/* The format name can contain the format id itself instead of
	the name and we check for that. */
	format_id = (uint) strtoul(format_name, &endp, 10);

	/* Check for valid parse. */
	if (*endp == '\0' && *format_name != '\0') {

		if (format_id <= UNIV_FORMAT_MAX) {

			return(format_id);
		}
	} else {

		for (format_id = 0; format_id <= UNIV_FORMAT_MAX;
		     format_id++) {
			const char*	name;

			name = trx_sys_file_format_id_to_name(format_id);

			if (!innobase_strcasecmp(format_name, name)) {

				return(format_id);
			}
		}
	}

	return(UNIV_FORMAT_MAX + 1);
}

/************************************************************//**
Validate the file format check config parameters, as a side effect it
sets the srv_max_file_format_at_startup variable.
@return the format_id if valid config value, otherwise, return -1 */
static
int
innobase_file_format_validate_and_set(
/*==================================*/
	const char*	format_max)	/*!< in: parameter value */
{
	uint		format_id;

	format_id = innobase_file_format_name_lookup(format_max);

	if (format_id < UNIV_FORMAT_MAX + 1) {
		srv_max_file_format_at_startup = format_id;

		return((int) format_id);
	} else {
		return(-1);
	}
}

/*************************************************************//**
Check if it is a valid file format. This function is registered as
a callback with MySQL.
@return	0 for valid file format */
static
int
innodb_file_format_name_validate(
/*=============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	file_format_input;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);

	ut_a(save != NULL);
	ut_a(value != NULL);

	file_format_input = value->val_str(value, buff, &len);

	if (file_format_input != NULL) {
		uint	format_id;

		format_id = innobase_file_format_name_lookup(
			file_format_input);

		if (format_id <= UNIV_FORMAT_MAX) {

			/* Save a pointer to the name in the
			'file_format_name_map' constant array. */
			*static_cast<const char**>(save) =
			    trx_sys_file_format_id_to_name(format_id);

			return(0);
		}
	}

	*static_cast<const char**>(save) = NULL;
	return(1);
}

/****************************************************************//**
Update the system variable innodb_file_format using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_file_format_name_update(
/*===========================*/
	THD*				thd,		/*!< in: thread handle */
	struct st_mysql_sys_var*	var,		/*!< in: pointer to
							system variable */
	void*				var_ptr,	/*!< out: where the
							formal string goes */
	const void*			save)		/*!< in: immediate result
							from check function */
{
	const char* format_name;

	ut_a(var_ptr != NULL);
	ut_a(save != NULL);

	format_name = *static_cast<const char*const*>(save);

	if (format_name) {
		uint	format_id;

		format_id = innobase_file_format_name_lookup(format_name);

		if (format_id <= UNIV_FORMAT_MAX) {
			srv_file_format = format_id;
		}
	}

	*static_cast<const char**>(var_ptr)
		= trx_sys_file_format_id_to_name(srv_file_format);
}

/*************************************************************//**
Check if valid argument to innodb_file_format_max. This function
is registered as a callback with MySQL.
@return	0 for valid file format */
static
int
innodb_file_format_max_validate(
/*============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	file_format_input;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	int		format_id;

	ut_a(save != NULL);
	ut_a(value != NULL);

	file_format_input = value->val_str(value, buff, &len);

	if (file_format_input != NULL) {

		format_id = innobase_file_format_validate_and_set(
			file_format_input);

		if (format_id >= 0) {
			/* Save a pointer to the name in the
			'file_format_name_map' constant array. */
			*static_cast<const char**>(save) =
			    trx_sys_file_format_id_to_name(
						(uint) format_id);

			return(0);

		} else {
			push_warning_printf(thd,
			  Sql_condition::WARN_LEVEL_WARN,
			  ER_WRONG_ARGUMENTS,
			  "InnoDB: invalid innodb_file_format_max "
			  "value; can be any format up to %s "
			  "or equivalent id of %d",
			  trx_sys_file_format_id_to_name(UNIV_FORMAT_MAX),
			  UNIV_FORMAT_MAX);
		}
	}

	*static_cast<const char**>(save) = NULL;
	return(1);
}

/****************************************************************//**
Update the system variable innodb_file_format_max using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_file_format_max_update(
/*==========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	const char*	format_name_in;
	const char**	format_name_out;
	uint		format_id;

	ut_a(save != NULL);
	ut_a(var_ptr != NULL);

	format_name_in = *static_cast<const char*const*>(save);

	if (!format_name_in) {

		return;
	}

	format_id = innobase_file_format_name_lookup(format_name_in);

	if (format_id > UNIV_FORMAT_MAX) {
		/* DEFAULT is "on", which is invalid at runtime. */
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Ignoring SET innodb_file_format=%s",
				    format_name_in);
		return;
	}

	format_name_out = static_cast<const char**>(var_ptr);

	/* Update the max format id in the system tablespace. */
	if (trx_sys_file_format_max_set(format_id, format_name_out)) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" [Info] InnoDB: the file format in the system "
			"tablespace is now set to %s.\n", *format_name_out);
	}
}

/*************************************************************//**
Check whether valid argument given to innobase_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	stopword_table_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	trx_t*		trx;
	int		ret = 1;

	ut_a(save != NULL);
	ut_a(value != NULL);

	stopword_table_name = value->val_str(value, buff, &len);

	trx = check_trx_exists(thd);

	row_mysql_lock_data_dictionary(trx);

	/* Validate the stopword table's (if supplied) existence and
	of the right format */
	if (!stopword_table_name
	    || fts_valid_stopword_table(stopword_table_name)) {
		*static_cast<const char**>(save) = stopword_table_name;
		ret = 0;
	}

	row_mysql_unlock_data_dictionary(trx);

	return(ret);
}

/*************************************************************//**
Check whether valid argument given to "innodb_fts_internal_tbl_name"
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_internal_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	table_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	int		ret = 1;
	dict_table_t*	user_table;

	ut_a(save != NULL);
	ut_a(value != NULL);

	table_name = value->val_str(value, buff, &len);

	if (!table_name) {
		*static_cast<const char**>(save) = NULL;
		return(0);
	}

	user_table = dict_table_open_on_name(
		table_name, FALSE, TRUE, DICT_ERR_IGNORE_NONE);

	if (user_table) {
		if (dict_table_has_fts_index(user_table)) {
			*static_cast<const char**>(save) = table_name;
			ret = 0;
		}

		dict_table_close(user_table, FALSE, TRUE);

		DBUG_EXECUTE_IF("innodb_evict_autoinc_table",
			mutex_enter(&dict_sys->mutex);
			dict_table_remove_from_cache_low(user_table, TRUE);
			mutex_exit(&dict_sys->mutex);
		);
	}

	return(ret);
}

/****************************************************************//**
Update global variable "fts_internal_tbl_name" with the "saved"
stopword table name value. This function is registered as a callback
with MySQL. */
static
void
innodb_internal_table_update(
/*=========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	const char*	table_name;
	char*		old;

	ut_a(save != NULL);
	ut_a(var_ptr != NULL);

	table_name = *static_cast<const char*const*>(save);
	old = *(char**) var_ptr;

	if (table_name) {
		*(char**) var_ptr =  my_strdup(table_name,  MYF(0));
	} else {
		*(char**) var_ptr = NULL;
	}

	if (old) {
		my_free(old);
	}

	fts_internal_tbl_name2 = *(char**) var_ptr;
	if (fts_internal_tbl_name2 == NULL) {
		fts_internal_tbl_name = const_cast<char*>("default");
	} else {
		fts_internal_tbl_name = fts_internal_tbl_name2;
	}
}

/****************************************************************//**
Update the system variable innodb_adaptive_hash_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_adaptive_hash_index_update(
/*==============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	if (*(my_bool*) save) {
		btr_search_enable();
	} else {
		btr_search_disable();
	}
}

/****************************************************************//**
Update the system variable innodb_cmp_per_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_cmp_per_index_update(
/*========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	/* Reset the stats whenever we enable the table
	INFORMATION_SCHEMA.innodb_cmp_per_index. */
	if (!srv_cmp_per_index_enabled && *(my_bool*) save) {
		page_zip_reset_stat_per_index();
	}

	srv_cmp_per_index_enabled = !!(*(my_bool*) save);
}

/****************************************************************//**
Update the system variable innodb_old_blocks_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_old_blocks_pct_update(
/*=========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innobase_old_blocks_pct = static_cast<uint>(
		buf_LRU_old_ratio_update(
			*static_cast<const uint*>(save), TRUE));
}

/****************************************************************//**
Update the system variable innodb_old_blocks_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_change_buffer_max_size_update(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innobase_change_buffer_max_size =
			(*static_cast<const uint*>(save));
	ibuf_max_size_update(innobase_change_buffer_max_size);
}

#ifdef UNIV_DEBUG
ulong srv_fil_make_page_dirty_debug = 0;
ulong srv_saved_page_number_debug = 0;

/****************************************************************//**
Save an InnoDB page number. */
static
void
innodb_save_page_no(
/*================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	srv_saved_page_number_debug = *static_cast<const ulong*>(save);

	ib_logf(IB_LOG_LEVEL_INFO,
		"Saving InnoDB page number: %lu",
		srv_saved_page_number_debug);
}

/****************************************************************//**
Make the first page of given user tablespace dirty. */
static
void
innodb_make_page_dirty(
/*===================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	mtr_t mtr;
	ulong space_id = *static_cast<const ulong*>(save);

	mtr_start(&mtr);

	buf_block_t* block = buf_page_get(
		space_id, 0, srv_saved_page_number_debug, RW_X_LATCH, &mtr);

	if (block) {
		byte* page = block->frame;
		ib_logf(IB_LOG_LEVEL_INFO,
			"Dirtying page:%lu of space:%lu",
			page_get_page_no(page),
			page_get_space_id(page));
		mlog_write_ulint(page + FIL_PAGE_TYPE,
				 fil_page_get_type(page),
				 MLOG_2BYTES, &mtr);
	}
	mtr_commit(&mtr);
}
#endif // UNIV_DEBUG

/*************************************************************//**
Find the corresponding ibuf_use_t value that indexes into
innobase_change_buffering_values[] array for the input
change buffering option name.
@return	corresponding IBUF_USE_* value for the input variable
name, or IBUF_USE_COUNT if not able to find a match */
static
ibuf_use_t
innodb_find_change_buffering_value(
/*===============================*/
	const char*	input_name)	/*!< in: input change buffering
					option name */
{
	ulint	use;

	for (use = 0; use < UT_ARR_SIZE(innobase_change_buffering_values);
	     use++) {
		/* found a match */
		if (!innobase_strcasecmp(
			input_name, innobase_change_buffering_values[use])) {
			return((ibuf_use_t) use);
		}
	}

	/* Did not find any match */
	return(IBUF_USE_COUNT);
}

/*************************************************************//**
Check if it is a valid value of innodb_change_buffering. This function is
registered as a callback with MySQL.
@return	0 for valid innodb_change_buffering */
static
int
innodb_change_buffering_validate(
/*=============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	change_buffering_input;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);

	ut_a(save != NULL);
	ut_a(value != NULL);

	change_buffering_input = value->val_str(value, buff, &len);

	if (change_buffering_input != NULL) {
		ibuf_use_t	use;

		use = innodb_find_change_buffering_value(
			change_buffering_input);

		if (use != IBUF_USE_COUNT) {
			/* Find a matching change_buffering option value. */
			*static_cast<const char**>(save) =
				innobase_change_buffering_values[use];

			return(0);
		}
	}

	/* No corresponding change buffering option for user supplied
	"change_buffering_input" */
	return(1);
}

/****************************************************************//**
Update the system variable innodb_change_buffering using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_change_buffering_update(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ibuf_use_t	use;

	ut_a(var_ptr != NULL);
	ut_a(save != NULL);

	use = innodb_find_change_buffering_value(
		*static_cast<const char*const*>(save));

	ut_a(use < IBUF_USE_COUNT);

	ibuf_use = use;
	*static_cast<const char**>(var_ptr) =
		 *static_cast<const char*const*>(save);
}

/*************************************************************//**
Just emit a warning that the usage of the variable is deprecated.
@return	0 */
static
void
innodb_stats_sample_pages_update(
/*=============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
#define STATS_SAMPLE_PAGES_DEPRECATED_MSG \
	"Using innodb_stats_sample_pages is deprecated and " \
	"the variable may be removed in future releases. " \
	"Please use innodb_stats_transient_sample_pages " \
	"instead."

	push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
		     HA_ERR_WRONG_COMMAND, STATS_SAMPLE_PAGES_DEPRECATED_MSG);

	ut_print_timestamp(stderr);
	fprintf(stderr,
		" InnoDB: Warning: %s\n",
		STATS_SAMPLE_PAGES_DEPRECATED_MSG);

	srv_stats_transient_sample_pages =
		*static_cast<const unsigned long long*>(save);
}

/****************************************************************//**
Update the monitor counter according to the "set_option",  turn
on/off or reset specified monitor counter. */
static
void
innodb_monitor_set_option(
/*======================*/
	const monitor_info_t* monitor_info,/*!< in: monitor info for the monitor
					to set */
	mon_option_t	set_option)	/*!< in: Turn on/off reset the
					counter */
{
	monitor_id_t	monitor_id = monitor_info->monitor_id;

	/* If module type is MONITOR_GROUP_MODULE, it cannot be
	turned on/off individually. It should never use this
	function to set options */
	ut_a(!(monitor_info->monitor_type & MONITOR_GROUP_MODULE));

	switch (set_option) {
	case MONITOR_TURN_ON:
		MONITOR_ON(monitor_id);
		MONITOR_INIT(monitor_id);
		MONITOR_SET_START(monitor_id);

		/* If the monitor to be turned on uses
		exisitng monitor counter (status variable),
		make special processing to remember existing
		counter value. */
		if (monitor_info->monitor_type
		    & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_ON);
		}
		break;

	case MONITOR_TURN_OFF:
		if (monitor_info->monitor_type & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_OFF);
		}

		MONITOR_OFF(monitor_id);
		MONITOR_SET_OFF(monitor_id);
		break;

	case MONITOR_RESET_VALUE:
		srv_mon_reset(monitor_id);
		break;

	case MONITOR_RESET_ALL_VALUE:
		srv_mon_reset_all(monitor_id);
		break;

	default:
		ut_error;
	}
}

/****************************************************************//**
Find matching InnoDB monitor counters and update their status
according to the "set_option",  turn on/off or reset specified
monitor counter. */
static
void
innodb_monitor_update_wildcard(
/*===========================*/
	const char*	name,		/*!< in: monitor name to match */
	mon_option_t	set_option)	/*!< in: the set option, whether
					to turn on/off or reset the counter */
{
	ut_a(name);

	for (ulint use = 0; use < NUM_MONITOR; use++) {
		ulint		type;
		monitor_id_t	monitor_id = static_cast<monitor_id_t>(use);
		monitor_info_t*	monitor_info;

		if (!innobase_wildcasecmp(
			srv_mon_get_name(monitor_id), name)) {
			monitor_info = srv_mon_get_info(monitor_id);

			type = monitor_info->monitor_type;

			/* If the monitor counter is of MONITOR_MODULE
			type, skip it. Except for those also marked with
			MONITOR_GROUP_MODULE flag, which can be turned
			on only as a module. */
			if (!(type & MONITOR_MODULE)
			     && !(type & MONITOR_GROUP_MODULE)) {
				innodb_monitor_set_option(monitor_info,
							  set_option);
			}

			/* Need to special handle counters marked with
			MONITOR_GROUP_MODULE, turn on the whole module if
			any one of it comes here. Currently, only
			"module_buf_page" is marked with MONITOR_GROUP_MODULE */
			if (type & MONITOR_GROUP_MODULE) {
				if ((monitor_id >= MONITOR_MODULE_BUF_PAGE)
				     && (monitor_id < MONITOR_MODULE_OS)) {
					if (set_option == MONITOR_TURN_ON
					    && MONITOR_IS_ON(
						MONITOR_MODULE_BUF_PAGE)) {
						continue;
					}

					srv_mon_set_module_control(
						MONITOR_MODULE_BUF_PAGE,
						set_option);
				} else {
					/* If new monitor is added with
					MONITOR_GROUP_MODULE, it needs
					to be added here. */
					ut_ad(0);
				}
			}
		}
	}
}

/*************************************************************//**
Given a configuration variable name, find corresponding monitor counter
and return its monitor ID if found.
@return	monitor ID if found, MONITOR_NO_MATCH if there is no match */
static
ulint
innodb_monitor_id_by_name_get(
/*==========================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	ut_a(name);

	/* Search for wild character '%' in the name, if
	found, we treat it as a wildcard match. We do not search for
	single character wildcard '_' since our monitor names already contain
	such character. To avoid confusion, we request user must include
	at least one '%' character to activate the wildcard search. */
	if (strchr(name, '%')) {
		return(MONITOR_WILDCARD_MATCH);
	}

	/* Not wildcard match, check for an exact match */
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (!innobase_strcasecmp(
			name, srv_mon_get_name(static_cast<monitor_id_t>(i)))) {
			return(i);
		}
	}

	return(MONITOR_NO_MATCH);
}
/*************************************************************//**
Validate that the passed in monitor name matches at least one
monitor counter name with wildcard compare.
@return	TRUE if at least one monitor name matches */
static
ibool
innodb_monitor_validate_wildcard_name(
/*==================================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (!innobase_wildcasecmp(
			srv_mon_get_name(static_cast<monitor_id_t>(i)), name)) {
			return(TRUE);
		}
	}

	return(FALSE);
}
/*************************************************************//**
Validate the passed in monitor name, find and save the
corresponding monitor name in the function parameter "save".
@return	0 if monitor name is valid */
static
int
innodb_monitor_valid_byname(
/*========================*/
	void*			save,	/*!< out: immediate result
					for update function */
	const char*		name)	/*!< in: incoming monitor name */
{
	ulint		use;
	monitor_info_t*	monitor_info;

	if (!name) {
		return(1);
	}

	use = innodb_monitor_id_by_name_get(name);

	/* No monitor name matches, nor it is wildcard match */
	if (use == MONITOR_NO_MATCH) {
		return(1);
	}

	if (use < NUM_MONITOR) {
		monitor_info = srv_mon_get_info((monitor_id_t) use);

		/* If the monitor counter is marked with
		MONITOR_GROUP_MODULE flag, then this counter
		cannot be turned on/off individually, instead
		it shall be turned on/off as a group using
		its module name */
		if ((monitor_info->monitor_type & MONITOR_GROUP_MODULE)
		    && (!(monitor_info->monitor_type & MONITOR_MODULE))) {
			sql_print_warning(
				"Monitor counter '%s' cannot"
				" be turned on/off individually."
				" Please use its module name"
				" to turn on/off the counters"
				" in the module as a group.\n",
				name);

			return(1);
		}

	} else {
		ut_a(use == MONITOR_WILDCARD_MATCH);

		/* For wildcard match, if there is not a single monitor
		counter name that matches, treat it as an invalid
		value for the system configuration variables */
		if (!innodb_monitor_validate_wildcard_name(name)) {
			return(1);
		}
	}

	/* Save the configure name for innodb_monitor_update() */
	*static_cast<const char**>(save) = name;

	return(0);
}
/*************************************************************//**
Validate passed-in "value" is a valid monitor counter name.
This function is registered as a callback with MySQL.
@return	0 for valid name */
static
int
innodb_monitor_validate(
/*====================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	name;
	char*		monitor_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	int		ret;

	ut_a(save != NULL);
	ut_a(value != NULL);

	name = value->val_str(value, buff, &len);

	/* monitor_name could point to memory from MySQL
	or buff[]. Always dup the name to memory allocated
	by InnoDB, so we can access it in another callback
	function innodb_monitor_update() and free it appropriately */
	if (name) {
		monitor_name = my_strdup(name, MYF(0));
	} else {
		return(1);
	}

	ret = innodb_monitor_valid_byname(save, monitor_name);

	if (ret) {
		/* Validation failed */
		my_free(monitor_name);
	} else {
		/* monitor_name will be freed in separate callback function
		innodb_monitor_update(). Assert "save" point to
		the "monitor_name" variable */
		ut_ad(*static_cast<char**>(save) == monitor_name);
	}

	return(ret);
}

/****************************************************************//**
Update the system variable innodb_enable(disable/reset/reset_all)_monitor
according to the "set_option" and turn on/off or reset specified monitor
counter. */
static
void
innodb_monitor_update(
/*==================*/
	THD*			thd,		/*!< in: thread handle */
	void*			var_ptr,	/*!< out: where the
						formal string goes */
	const void*		save,		/*!< in: immediate result
						from check function */
	mon_option_t		set_option,	/*!< in: the set option,
						whether to turn on/off or
						reset the counter */
	ibool			free_mem)	/*!< in: whether we will
						need to free the memory */
{
	monitor_info_t*	monitor_info;
	ulint		monitor_id;
	ulint		err_monitor = 0;
	const char*	name;

	ut_a(save != NULL);

	name = *static_cast<const char*const*>(save);

	if (!name) {
		monitor_id = MONITOR_DEFAULT_START;
	} else {
		monitor_id = innodb_monitor_id_by_name_get(name);

		/* Double check we have a valid monitor ID */
		if (monitor_id == MONITOR_NO_MATCH) {
			return;
		}
	}

	if (monitor_id == MONITOR_DEFAULT_START) {
		/* If user set the variable to "default", we will
		print a message and make this set operation a "noop".
		The check is being made here is because "set default"
		does not go through validation function */
		if (thd) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_DEFAULT,
				"Default value is not defined for "
				"this set option. Please specify "
				"correct counter or module name.");
		} else {
			sql_print_error(
				"Default value is not defined for "
				"this set option. Please specify "
				"correct counter or module name.\n");
		}

		if (var_ptr) {
			*(const char**) var_ptr = NULL;
		}
	} else if (monitor_id == MONITOR_WILDCARD_MATCH) {
		innodb_monitor_update_wildcard(name, set_option);
	} else {
		monitor_info = srv_mon_get_info(
			static_cast<monitor_id_t>(monitor_id));

		ut_a(monitor_info);

		/* If monitor is already truned on, someone could already
		collect monitor data, exit and ask user to turn off the
		monitor before turn it on again. */
		if (set_option == MONITOR_TURN_ON
		    && MONITOR_IS_ON(monitor_id)) {
			err_monitor = monitor_id;
			goto exit;
		}

		if (var_ptr) {
			*(const char**) var_ptr = monitor_info->monitor_name;
		}

		/* Depending on the monitor name is for a module or
		a counter, process counters in the whole module or
		individual counter. */
		if (monitor_info->monitor_type & MONITOR_MODULE) {
			srv_mon_set_module_control(
				static_cast<monitor_id_t>(monitor_id),
				set_option);
		} else {
			innodb_monitor_set_option(monitor_info, set_option);
		}
	}
exit:
	/* Only if we are trying to turn on a monitor that already
	been turned on, we will set err_monitor. Print related
	information */
	if (err_monitor) {
		sql_print_warning("Monitor %s is already enabled.",
				  srv_mon_get_name((monitor_id_t) err_monitor));
	}

	if (free_mem && name) {
		my_free((void*) name);
	}

	return;
}

#ifdef __WIN__
/*************************************************************//**
Validate if passed-in "value" is a valid value for
innodb_buffer_pool_filename. On Windows, file names with colon (:)
are not allowed.

@return	0 for valid name */
static
int
innodb_srv_buf_dump_filename_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	buf_name;
	char		buff[OS_FILE_MAX_PATH];
	int		len= sizeof(buff);

	ut_a(save != NULL);
	ut_a(value != NULL);

	buf_name = value->val_str(value, buff, &len);

	if (buf_name) {
		if (is_filename_allowed(buf_name, len, FALSE)){
			*static_cast<const char**>(save) = buf_name;
			return(0);
		} else {
			push_warning_printf(thd,
				Sql_condition::WARN_LEVEL_WARN,
				ER_WRONG_ARGUMENTS,
				"InnoDB: innodb_buffer_pool_filename "
				"cannot have colon (:) in the file name.");

		}
	}

	return(1);
}
#else /* __WIN__ */
# define innodb_srv_buf_dump_filename_validate NULL
#endif /* __WIN__ */

#ifdef UNIV_DEBUG
static char* srv_buffer_pool_evict;

/****************************************************************//**
Evict all uncompressed pages of compressed tables from the buffer pool.
Keep the compressed pages in the buffer pool.
@return whether all uncompressed pages were evicted */
static MY_ATTRIBUTE((warn_unused_result))
bool
innodb_buffer_pool_evict_uncompressed(void)
/*=======================================*/
{
	bool	all_evicted = true;

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool = &buf_pool_ptr[i];

		mutex_enter(&buf_pool->LRU_list_mutex);

		for (buf_block_t* block = UT_LIST_GET_LAST(
			     buf_pool->unzip_LRU);
		     block != NULL; ) {
			buf_block_t*	prev_block = UT_LIST_GET_PREV(
				unzip_LRU, block);
			ut_ad(buf_block_get_state(block)
			      == BUF_BLOCK_FILE_PAGE);
			ut_ad(block->in_unzip_LRU_list);
			ut_ad(block->page.in_LRU_list);

			mutex_enter(&block->mutex);
			all_evicted = buf_LRU_free_page(&block->page, false);
			mutex_exit(&block->mutex);

			if (all_evicted) {

				mutex_enter(&buf_pool->LRU_list_mutex);
				block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
			} else {

				block = prev_block;
			}
		}

		mutex_exit(&buf_pool->LRU_list_mutex);
	}

	return(all_evicted);
}

/****************************************************************//**
Called on SET GLOBAL innodb_buffer_pool_evict=...
Handles some values specially, to evict pages from the buffer pool.
SET GLOBAL innodb_buffer_pool_evict='uncompressed'
evicts all uncompressed page frames of compressed tablespaces. */
static
void
innodb_buffer_pool_evict_update(
/*============================*/
	THD*			thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*var,	/*!< in: pointer to system variable */
	void*			var_ptr,/*!< out: ignored */
	const void*		save)	/*!< in: immediate result
					from check function */
{
	if (const char* op = *static_cast<const char*const*>(save)) {
		if (!strcmp(op, "uncompressed")) {
			for (uint tries = 0; tries < 10000; tries++) {
				if (innodb_buffer_pool_evict_uncompressed()) {
					return;
				}

				os_thread_sleep(10000);
			}

			/* We failed to evict all uncompressed pages. */
			ut_ad(0);
		}
	}
}
#endif /* UNIV_DEBUG */

/****************************************************************//**
Update the system variable innodb_monitor_enable and enable
specified monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_enable_monitor_update(
/*=========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_ON, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_disable and turn
off specified monitor counter. */
static
void
innodb_disable_monitor_update(
/*==========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_OFF, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset and reset
specified monitor counter(s).
This function is registered as a callback with MySQL. */
static
void
innodb_reset_monitor_update(
/*========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_VALUE, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset_all and reset
all value related monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_reset_all_monitor_update(
/*============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_ALL_VALUE,
			      TRUE);
}

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can list the monitor counters/groups to be enable by specifying
"loose-innodb_monitor_enable=monitor_name1;monitor_name2..."
in server configuration file or at the command line. The string
separate could be ";", "," or empty space. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str)	/*!< in/out: monitor counter enable list */
{
	static const char*	sep = " ;,";
	char*			last;

	ut_a(str);

	/* Walk through the string, and separate each monitor counter
	and/or counter group name, and calling innodb_monitor_update()
	if successfully updated. Please note that the "str" would be
	changed by strtok_r() as it walks through it. */
	for (char* option = strtok_r(str, sep, &last);
	     option;
	     option = strtok_r(NULL, sep, &last)) {
		ulint	ret;
		char*	option_name;

		ret = innodb_monitor_valid_byname(&option_name, option);

		/* The name is validated if ret == 0 */
		if (!ret) {
			innodb_monitor_update(NULL, NULL, &option,
					      MONITOR_TURN_ON, FALSE);
		} else {
			sql_print_warning("Invalid monitor counter"
					  " name: '%s'", option);
		}
	}
}

#ifdef UNIV_LINUX

/****************************************************************//**
Update the innodb_sched_priority_cleaner variable and set the thread
priorities accordingly.  */
static
void
innodb_sched_priority_cleaner_update(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulint	priority = *static_cast<const ulint *>(save);
	ulint	actual_priority;

	/* Set the priority for the LRU manager thread */
	ut_ad(buf_lru_manager_is_active);
	actual_priority = os_thread_set_priority(srv_lru_manager_tid,
						 priority);
	if (UNIV_UNLIKELY(actual_priority != priority)) {

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Failed to set the LRU manager thread "
				    "priority to %lu,  "
				    "the current priority is %lu", priority,
				    actual_priority);
	} else {

		srv_sched_priority_cleaner = priority;
	}

	/* Set the priority for the page cleaner thread */
	if (srv_read_only_mode) {

		return;
	}

	ut_ad(buf_page_cleaner_is_active);
	actual_priority = os_thread_set_priority(srv_cleaner_tid, priority);
	if (UNIV_UNLIKELY(actual_priority != priority)) {

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Failed to set the page cleaner thread "
				    "priority to %lu,  "
				    "the current priority is %lu", priority,
				    actual_priority);
	}
}

#if defined(UNIV_DEBUG) || (UNIV_PERF_DEBUG)

/****************************************************************//**
Update the innodb_sched_priority_purge variable and set the thread
priorities accordingly.  */
static
void
innodb_sched_priority_purge_update(
/*===============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulint	priority = *static_cast<const ulint *>(save);

	if (srv_read_only_mode) {
		return;
	}

	for (ulint i = 0; i < srv_n_purge_threads; i++) {

		ulint actual_priority
			= os_thread_set_priority(srv_purge_tids[i], priority);
		if (UNIV_UNLIKELY(actual_priority != priority)) {

			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_ARGUMENTS,
					    "Failed to set the purge "
					    "thread priority to %lu, the "
					    "current priority is %lu, "
					    "aborting priority update",
					    priority, actual_priority);
			return;
		}
	}

	srv_sched_priority_purge = priority;
}

/****************************************************************//**
Update the innodb_sched_priority_io variable and set the thread
priorities accordingly.  */
static
void
innodb_sched_priority_io_update(
/*============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
					        system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulint	priority = *static_cast<const ulint *>(save);

	for (ulint i = 0; i < srv_n_file_io_threads; i++) {

		ulint actual_priority = os_thread_set_priority(srv_io_tids[i],
							       priority);

		if (UNIV_UNLIKELY(actual_priority != priority)) {

			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_ARGUMENTS,
					    "Failed to set the I/O "
					    "thread priority to %lu, the "
					    "current priority is %lu, "
					    "aborting priority update",
					    priority, actual_priority);
			return;
		}
	}

	srv_sched_priority_io = priority;
}

/****************************************************************//**
Update the innodb_sched_priority_master variable and set the thread
priorities accordingly.  */
static
void
innodb_sched_priority_master_update(
/*================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to
						system variable */
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulint	priority = *static_cast<const lint *>(save);
	ulint	actual_priority;

	if (srv_read_only_mode) {
		return;
	}

	actual_priority = os_thread_set_priority(srv_master_tid, priority);
	if (UNIV_UNLIKELY(actual_priority != priority)) {

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Failed to set the master thread "
				    "priority to %lu, "
				    "the current priority is %lu", priority,
				    actual_priority);
	} else {

		srv_sched_priority_master = priority;
	}
}

#endif /* defined(UNIV_DEBUG) || (UNIV_PERF_DEBUG) */

#endif /* UNIV_LINUX */

#ifdef UNIV_DEBUG
/*************************************************************//**
Check if it is a valid value of innodb_track_changed_pages.
Changed pages tracking is not working correctly without initialization
procedure on server startup. The function allows to temporary
disable tracking, but only if the feature was enabled on startup.
This function is registered as a callback with MySQL.
@return	0 for valid innodb_track_changed_pages */
static
int
innodb_track_changed_pages_validate(
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming bool */
{
	long long	intbuf = 0;

	if (value->val_int(value, &intbuf)) {
		/* The value is NULL. That is invalid. */
		return 1;
	}

	if (srv_redo_log_thread_started) {
		*reinterpret_cast<ulong*>(save)
			= static_cast<ulong>(intbuf);
		return 0;
	}

	if (intbuf == srv_track_changed_pages) { // == 0
		*reinterpret_cast<ulong*>(save) = srv_track_changed_pages;
		return 0;
	}

	return 1;
}
#endif

/****************************************************************//**
Callback function for accessing the InnoDB variables from MySQL:
SHOW VARIABLES. */
static
int
show_innodb_vars(
/*=============*/
	THD*		thd,
	SHOW_VAR*	var,
	char*		buff)
{
	innodb_export_status();
	var->type = SHOW_ARRAY;
	var->value = (char*) &innodb_status_variables;

	return(0);
}

/****************************************************************//**
This function checks each index name for a table against reserved
system default primary index name 'GEN_CLUST_INDEX'. If a name
matches, this function pushes an warning message to the client,
and returns true.
@return true if the index name matches the reserved name */
UNIV_INTERN
bool
innobase_index_name_is_reserved(
/*============================*/
	THD*		thd,		/*!< in/out: MySQL connection */
	const KEY*	key_info,	/*!< in: Indexes to be created */
	ulint		num_of_keys)	/*!< in: Number of indexes to
					be created. */
{
	const KEY*	key;
	uint		key_num;	/* index number */

	for (key_num = 0; key_num < num_of_keys; key_num++) {
		key = &key_info[key_num];

		if (innobase_strcasecmp(key->name,
					innobase_index_reserve_name) == 0) {
			/* Push warning to mysql */
			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    "Cannot Create Index with name "
					    "'%s'. The name is reserved "
					    "for the system default primary "
					    "index.",
					    innobase_index_reserve_name);

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 innobase_index_reserve_name);

			return(true);
		}
	}

	return(false);
}

/***********************************************************************
Retrieve the FTS Relevance Ranking result for doc with doc_id
of prebuilt->fts_doc_id
@return the relevance ranking value */
UNIV_INTERN
float
innobase_fts_retrieve_ranking(
/*============================*/
		FT_INFO * fts_hdl)	/*!< in: FTS handler */
{
	row_prebuilt_t*	ft_prebuilt;
	fts_result_t*	result;

	result = ((NEW_FT_INFO*) fts_hdl)->ft_result;

	ft_prebuilt = ((NEW_FT_INFO*) fts_hdl)->ft_prebuilt;

	if (ft_prebuilt->read_just_key) {
		fts_ranking_t*  ranking =
			rbt_value(fts_ranking_t, result->current);
		return(ranking->rank);
	}

	/* Retrieve the ranking value for doc_id with value of
	prebuilt->fts_doc_id */
	return(fts_retrieve_ranking(result, ft_prebuilt->fts_doc_id));
}

/***********************************************************************
Free the memory for the FTS handler */
UNIV_INTERN
void
innobase_fts_close_ranking(
/*=======================*/
		FT_INFO * fts_hdl)
{
	fts_result_t*	result;

	result = ((NEW_FT_INFO*) fts_hdl)->ft_result;

	fts_query_free_result(result);

	my_free((uchar*) fts_hdl);

	return;
}

/***********************************************************************
Find and Retrieve the FTS Relevance Ranking result for doc with doc_id
of prebuilt->fts_doc_id
@return the relevance ranking value */
UNIV_INTERN
float
innobase_fts_find_ranking(
/*======================*/
		FT_INFO*	fts_hdl,	/*!< in: FTS handler */
		uchar*		record,		/*!< in: Unused */
		uint		len)		/*!< in: Unused */
{
	row_prebuilt_t*	ft_prebuilt;
	fts_result_t*	result;

	ft_prebuilt = ((NEW_FT_INFO*) fts_hdl)->ft_prebuilt;
	result = ((NEW_FT_INFO*) fts_hdl)->ft_result;

	/* Retrieve the ranking value for doc_id with value of
	prebuilt->fts_doc_id */
	return(fts_retrieve_ranking(result, ft_prebuilt->fts_doc_id));
}

#ifdef UNIV_DEBUG
static my_bool	innodb_purge_run_now = TRUE;
static my_bool	innodb_purge_stop_now = TRUE;
static my_bool	innodb_log_checkpoint_now = TRUE;
static my_bool	innodb_buf_flush_list_now = TRUE;
static my_bool	innodb_track_redo_log_now = TRUE;

/****************************************************************//**
Set the purge state to RUN. If purge is disabled then it
is a no-op. This function is registered as a callback with MySQL. */
static
void
purge_run_now_set(
/*==============*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && trx_purge_state() != PURGE_STATE_DISABLED) {
		trx_purge_run();
	}
}

/****************************************************************//**
Set the purge state to STOP. If purge is disabled then it
is a no-op. This function is registered as a callback with MySQL. */
static
void
purge_stop_now_set(
/*===============*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && trx_purge_state() != PURGE_STATE_DISABLED) {
		trx_purge_stop();
	}
}

/****************************************************************//**
Force innodb to checkpoint. */
static
void
checkpoint_now_set(
/*===============*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save) {
		while (log_sys->last_checkpoint_lsn < log_sys->lsn) {
			log_make_checkpoint_at(LSN_MAX, TRUE);
			fil_flush_file_spaces(FIL_LOG);
		}
		fil_write_flushed_lsn_to_data_files(log_sys->lsn, 0);
		fil_flush_file_spaces(FIL_TABLESPACE);
	}
}

/****************************************************************//**
Force a dirty pages flush now. */
static
void
buf_flush_list_now_set(
/*===================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						  check function */
{
	if (*(my_bool*) save) {
		buf_flush_list(ULINT_MAX, LSN_MAX, NULL);
		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
	}
}

/****************************************************************//**
Force log tracker to track the log synchronously.  */
static
void
track_redo_log_now_set(
/*===================*/
	THD*				thd	/*!< in: thread handle */
	MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						  variable */
	MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						  string goes */
	MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						  check function */
{
	if (*(my_bool*) save && srv_track_changed_pages) {

		log_online_follow_redo_log();
	}
}

#endif /* UNIV_DEBUG */

/***********************************************************************
@return version of the extended FTS API */
uint
innobase_fts_get_version()
/*======================*/
{
	/* Currently this doesn't make much sense as returning
	HA_CAN_FULLTEXT_EXT automatically mean this version is supported.
	This supposed to ease future extensions.  */
	return(2);
}

/***********************************************************************
@return Which part of the extended FTS API is supported */
ulonglong
innobase_fts_flags()
/*================*/
{
	return(FTS_ORDERED_RESULT | FTS_DOCID_IN_RESULT);
}


/***********************************************************************
Find and Retrieve the FTS doc_id for the current result row
@return the document ID */
ulonglong
innobase_fts_retrieve_docid(
/*========================*/
		FT_INFO_EXT * fts_hdl)	/*!< in: FTS handler */
{
	row_prebuilt_t* ft_prebuilt;
	fts_result_t*	result;

	ft_prebuilt = ((NEW_FT_INFO *)fts_hdl)->ft_prebuilt;
	result = ((NEW_FT_INFO *)fts_hdl)->ft_result;

	if (ft_prebuilt->read_just_key) {
		fts_ranking_t* ranking =
			rbt_value(fts_ranking_t, result->current);
		return(ranking->doc_id);
	}

	return(ft_prebuilt->fts_doc_id);
}


/***********************************************************************
Find and retrieve the size of the current result
@return number of matching rows */
ulonglong
innobase_fts_count_matches(
/*=======================*/
	FT_INFO_EXT* fts_hdl)	/*!< in: FTS handler */
{
	NEW_FT_INFO*	handle = (NEW_FT_INFO *) fts_hdl;

	if (handle->ft_result->rankings_by_id != 0) {
		return rbt_size(handle->ft_result->rankings_by_id);
	} else {
		return(0);
	}
}

/* These variables are never read by InnoDB or changed. They are a kind of
dummies that are needed by the MySQL infrastructure to call
buffer_pool_dump_now(), buffer_pool_load_now() and buffer_pool_load_abort()
by the user by doing:
  SET GLOBAL innodb_buffer_pool_dump_now=ON;
  SET GLOBAL innodb_buffer_pool_load_now=ON;
  SET GLOBAL innodb_buffer_pool_load_abort=ON;
Their values are read by MySQL and displayed to the user when the variables
are queried, e.g.:
  SELECT @@innodb_buffer_pool_dump_now;
  SELECT @@innodb_buffer_pool_load_now;
  SELECT @@innodb_buffer_pool_load_abort; */
static my_bool	innodb_buffer_pool_dump_now = FALSE;
static my_bool	innodb_buffer_pool_load_now = FALSE;
static my_bool	innodb_buffer_pool_load_abort = FALSE;

/****************************************************************//**
Trigger a dump of the buffer pool if innodb_buffer_pool_dump_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_dump_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		buf_dump_start();
	}
}

/****************************************************************//**
Trigger a load of the buffer pool if innodb_buffer_pool_load_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		buf_load_start();
	}
}

/****************************************************************//**
Abort a load of the buffer pool if innodb_buffer_pool_load_abort
is set to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_abort(
/*===================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save) {
		buf_load_abort();
	}
}

/** Update innodb_status_output or innodb_status_output_locks,
which control InnoDB "status monitor" output to the error log.
@param[in]	thd	thread handle
@param[in]	var	system variable
@param[out]	var_ptr	current value
@param[in]	save	to-be-assigned value */
static
void
innodb_status_output_update(
	THD*				thd MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var MY_ATTRIBUTE((unused)),
	void*				var_ptr MY_ATTRIBUTE((unused)),
	const void*			save MY_ATTRIBUTE((unused)))
{
	*static_cast<my_bool*>(var_ptr) = *static_cast<const my_bool*>(save);
	/* The lock timeout monitor thread also takes care of this
	output. */
	os_event_set(lock_sys->timeout_event);
}

/*************************************************************//**
Empty free list algorithm. This function is registered as
a callback with MySQL. 
@return	0 for valid algorithm */
static
int
innodb_srv_empty_free_list_algorithm_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	algorithm_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	ulint	algo;
	srv_empty_free_list_t 		algorithm;

	algorithm_name = value->val_str(value, buff, &len);

	if (!algorithm_name) {
		return(1);
	}

	for (algo = 0; algo < array_elements(
				innodb_empty_free_list_algorithm_names
				) - 1;
			algo++) {
		if (!innobase_strcasecmp(
				algorithm_name,
				innodb_empty_free_list_algorithm_names[algo]))
			break;
	}

	if (algo == array_elements( innodb_empty_free_list_algorithm_names) - 1)
		return(1);

	algorithm = static_cast<srv_empty_free_list_t>(algo);
	if (!innodb_empty_free_list_algorithm_allowed(algorithm)) {
		sql_print_warning(
				"InnoDB: innodb_empty_free_list_algorithm "
				"= 'backoff' requires at least"
				" 20MB buffer pool instances.\n");
		return(1);
	}

	*reinterpret_cast<ulong*>(save) = static_cast<ulong>(algorithm);
	return(0);
}

static SHOW_VAR innodb_status_variables_export[]= {
	{"Innodb", (char*) &show_innodb_vars, SHOW_FUNC},
	{NullS, NullS, SHOW_LONG}
};

static struct st_mysql_storage_engine innobase_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

#ifdef WITH_WSREP
void
wsrep_abort_slave_trx(wsrep_seqno_t bf_seqno, wsrep_seqno_t victim_seqno)
{
	WSREP_ERROR("Trx %lld tries to abort slave trx %lld. This could be "
		"caused by:\n\t"
		"1) unsupported configuration options combination, please check documentation.\n\t"
		"2) a bug in the code.\n\t"
		"3) a database corruption.\n Node consistency compromized, "
		"need to abort. Restart the node to resync with cluster.",
		(long long)bf_seqno, (long long)victim_seqno);
	abort();
}

int
wsrep_innobase_kill_one_trx(void * const bf_thd_ptr,
                            const trx_t * const bf_trx,
                            trx_t *victim_trx, ibool signal)
{
        ut_ad(lock_mutex_own());
        // This is there in upstream codership 5.6 but causes 
        // crashes, hence disabled
        //ut_ad(trx_mutex_own(victim_trx));
        ut_ad(bf_thd_ptr);
        ut_ad(victim_trx);

	DBUG_ENTER("wsrep_innobase_kill_one_trx");
        THD *bf_thd       = bf_thd_ptr ? (THD*) bf_thd_ptr : NULL;
	THD *thd          = (THD *) victim_trx->mysql_thd;
	int64_t bf_seqno  = (bf_thd) ? wsrep_thd_trx_seqno(bf_thd) : 0;

	if (!thd) {
		DBUG_PRINT("wsrep", ("no thd for conflicting lock"));
		WSREP_WARN("no THD for trx: %llu", (long long)victim_trx->id);
		DBUG_RETURN(1);
	}
	if (!bf_thd) {
		DBUG_PRINT("wsrep", ("no BF thd for conflicting lock"));
		WSREP_WARN("no BF THD for trx: %llu",
			   (bf_trx) ? (long long)bf_trx->id : 0);
		DBUG_RETURN(1);
	}

	WSREP_LOG_CONFLICT(bf_thd, thd, TRUE);

	WSREP_DEBUG("BF kill (%lu, seqno: %lld), victim: (%llu) trx: %llu",
 		    signal, (long long)bf_seqno,
 		    (long long)wsrep_thd_thread_id(thd),
		    (long long)victim_trx->id);

	WSREP_DEBUG("Aborting query: %s conf %d trx: %llu",
		    (thd && wsrep_thd_query(thd)) ? wsrep_thd_query(thd) : "void",
		    wsrep_thd_conflict_state(thd),
		    (unsigned long long) wsrep_thd_ws_handle(thd)->trx_id);

	wsrep_thd_LOCK(thd);
        DBUG_EXECUTE_IF("sync.wsrep_after_BF_victim_lock",
                 {
                   const char act[]=
                     "now "
                     "wait_for signal.wsrep_after_BF_victim_lock";
                   DBUG_ASSERT(!debug_sync_set_action(bf_thd,
                                                      STRING_WITH_LEN(act)));
                 };);


	if (wsrep_thd_query_state(thd) == QUERY_EXITING) {
		WSREP_DEBUG("kill trx EXITING for %llu",
			    (long long)victim_trx->id);
		wsrep_thd_UNLOCK(thd);
		DBUG_RETURN(0);
	}
	if(wsrep_thd_exec_mode(thd) != LOCAL_STATE) {
		WSREP_DEBUG("withdraw for BF trx: %llu, state: %d",
			    (long long)victim_trx->id,
		wsrep_thd_conflict_state(thd));
	}

	switch (wsrep_thd_conflict_state(thd)) {
	case NO_CONFLICT: 
		wsrep_thd_set_conflict_state(thd, MUST_ABORT);
		break;
        case MUST_ABORT:
		WSREP_DEBUG("victim %llu in MUST ABORT state",
			    (long long)victim_trx->id);
		wsrep_thd_UNLOCK(thd);
		wsrep_thd_awake(thd, signal);
		DBUG_RETURN(0);
		break;
	case ABORTED:
	case ABORTING: // fall through
	default:
		WSREP_DEBUG("victim %llu in state %d",
			    (long long)victim_trx->id,
			    wsrep_thd_conflict_state(thd));
		wsrep_thd_UNLOCK(thd);
		DBUG_RETURN(0);
		break;
	}

	switch (wsrep_thd_query_state(thd)) {
	case QUERY_COMMITTING:
		enum wsrep_status rcode;

		WSREP_DEBUG("kill trx QUERY_COMMITTING for %llu", 
			    (long long)victim_trx->id);
		wsrep_thd_awake(thd, signal); 

		if (wsrep_thd_exec_mode(thd) == REPL_RECV) {
			wsrep_abort_slave_trx(bf_seqno,
					      wsrep_thd_trx_seqno(thd));
		} else {
			rcode = wsrep->abort_pre_commit(
				wsrep, bf_seqno,
				(wsrep_trx_id_t)wsrep_thd_ws_handle(thd)->trx_id
			);
			switch (rcode) {
			case WSREP_WARNING:
				WSREP_DEBUG("cancel commit warning: %llu",
					    (long long)victim_trx->id);
				wsrep_thd_UNLOCK(thd);
				DBUG_RETURN(1);
				break;
			case WSREP_OK:
				break;
			default:
				WSREP_ERROR(
					"cancel commit bad exit: %d %llu", 
					rcode, 
					(long long)victim_trx->id);
				/* unable to interrupt, must abort */
				/* note: kill_mysql() will block, if we cannot.
				 * kill the lock holder first.
				 */
				abort();
				break;
			}
		}
		break;
	case QUERY_EXEC:
		/* it is possible that victim trx is itself waiting for some 
		 * other lock. We need to cancel this waiting
		 */
		WSREP_DEBUG("kill trx QUERY_EXEC for %llu", (long long)victim_trx->id);

		victim_trx->lock.was_chosen_as_deadlock_victim= TRUE;
		if (victim_trx->lock.wait_lock) {
			WSREP_DEBUG("victim has wait flag: %ld",
				wsrep_thd_thread_id(thd));
			lock_t*  wait_lock = victim_trx->lock.wait_lock;
			if (wait_lock) {
				WSREP_DEBUG("canceling wait lock");
				victim_trx->lock.was_chosen_as_deadlock_victim= TRUE;
				lock_cancel_waiting_and_release(wait_lock);
			}

			wsrep_thd_awake(thd, signal); 
		} else {
			/* abort currently executing query */
			DBUG_PRINT("wsrep",("sending KILL_QUERY to: %ld", 
                                            wsrep_thd_thread_id(thd)));
			WSREP_DEBUG("kill query for: %ld",
				wsrep_thd_thread_id(thd));
			wsrep_thd_awake(thd, signal); 

			/* for BF thd, we need to prevent him from committing */
			if (wsrep_thd_exec_mode(thd) == REPL_RECV) {
				wsrep_abort_slave_trx(bf_seqno,
						    wsrep_thd_trx_seqno(thd));
			}
		}
		break;
	case QUERY_IDLE:
	{
		bool skip_abort= false;
		wsrep_aborting_thd_t abortees;

		WSREP_DEBUG("kill IDLE for %llu", (long long)victim_trx->id);

		if (wsrep_thd_exec_mode(thd) == REPL_RECV) {
			WSREP_DEBUG("kill BF IDLE, seqno: %lld",
				    (long long)wsrep_thd_trx_seqno(thd));
			wsrep_thd_UNLOCK(thd);
			wsrep_abort_slave_trx(bf_seqno,
					      wsrep_thd_trx_seqno(thd));
			DBUG_RETURN(0);
		}
                /* This will lock thd from proceeding after net_read() */
		wsrep_thd_set_conflict_state(thd, ABORTING);

		mysql_mutex_lock(&LOCK_wsrep_rollback);

		abortees = wsrep_aborting_thd;
		while (abortees && !skip_abort) {
			/* check if we have a kill message for this already */
			if (abortees->aborting_thd == thd) {
				skip_abort = true;
				WSREP_WARN("duplicate thd aborter %lu", 
					  wsrep_thd_thread_id(thd));
			}
			abortees = abortees->next;
		}
		if (!skip_abort) {
			wsrep_aborting_thd_t aborting = (wsrep_aborting_thd_t)
				my_malloc(sizeof(struct wsrep_aborting_thd), 
					  MYF(0));
			aborting->aborting_thd  = thd;
			aborting->next          = wsrep_aborting_thd;
			wsrep_aborting_thd      = aborting;
 			DBUG_PRINT("wsrep",("enqueuing trx abort for %lu",
                                             wsrep_thd_thread_id(thd)));
			WSREP_DEBUG("enqueuing trx abort for (%lu)",
				    wsrep_thd_thread_id(thd));
		}

		DBUG_PRINT("wsrep",("signalling wsrep rollbacker"));
		WSREP_DEBUG("signaling aborter");
		mysql_cond_signal(&COND_wsrep_rollback);
		mysql_mutex_unlock(&LOCK_wsrep_rollback);

		break;
	}
	default:
		WSREP_WARN("bad wsrep query state: %d", 
			  wsrep_thd_query_state(thd));
		break;
	}
	wsrep_thd_UNLOCK(thd);

	DBUG_RETURN(0);
}
static int
wsrep_abort_transaction(handlerton* hton, THD *bf_thd, THD *victim_thd,
			my_bool signal)
{
	DBUG_ENTER("wsrep_innobase_abort_thd");
	trx_t* victim_trx = thd_to_trx(victim_thd);
	trx_t* bf_trx     = (bf_thd) ? thd_to_trx(bf_thd) : NULL;
	WSREP_DEBUG("abort transaction: BF: %s victim: %s victim conf: %d",
		    wsrep_thd_query(bf_thd),
		    wsrep_thd_query(victim_thd),
		    wsrep_thd_conflict_state(victim_thd));

	if (victim_trx)
	{
                lock_mutex_enter();
                trx_mutex_enter(victim_trx);
		int rcode = wsrep_innobase_kill_one_trx(bf_thd, bf_trx,
                                                        victim_trx, signal);
                trx_mutex_exit(victim_trx);
                lock_mutex_exit();
		wsrep_srv_conc_cancel_wait(victim_trx);

 		DBUG_RETURN(rcode);
	} else {
		WSREP_DEBUG("victim does not have transaction");
		wsrep_thd_LOCK(victim_thd);
		wsrep_thd_set_conflict_state(victim_thd, MUST_ABORT);
		wsrep_thd_UNLOCK(victim_thd);
		wsrep_thd_awake(victim_thd, signal); 
	}
	DBUG_RETURN(-1);
}

static int innobase_wsrep_set_checkpoint(handlerton* hton, const XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);
        if (wsrep_is_wsrep_xid(xid)) {
                mtr_t mtr;
                mtr_start(&mtr);
                trx_sysf_t* sys_header = trx_sysf_get(&mtr);
                trx_sys_update_wsrep_checkpoint(xid, sys_header, &mtr);
                mtr_commit(&mtr);
                innobase_flush_logs(hton);
                return 0;
        } else {
                return 1;
        }
}

static int innobase_wsrep_get_checkpoint(handlerton* hton, XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);
        trx_sys_read_wsrep_checkpoint(xid);
        return 0;
}

static void
wsrep_fake_trx_id(
	handlerton	*hton,
	THD		*thd)	/*!< in: user thread handle */
{
	mutex_enter(&trx_sys->mutex);
	trx_id_t trx_id = trx_sys_get_new_trx_id();
	mutex_exit(&trx_sys->mutex);
	WSREP_DEBUG("innodb fake trx id: %llu thd: %s",
			(unsigned long long) trx_id,
			wsrep_thd_query(thd));
	(void *)wsrep_ws_handle_for_trx(wsrep_thd_ws_handle(thd), trx_id);
}

#endif /* WITH_WSREP */
/* plugin options */

static MYSQL_SYSVAR_ENUM(checksum_algorithm, srv_checksum_algorithm,
  PLUGIN_VAR_RQCMDARG,
  "The algorithm InnoDB uses for page checksumming. Possible values are "
  "CRC32 (hardware accelerated if the CPU supports it) "
    "write crc32, allow any of the other checksums to match when reading; "
  "STRICT_CRC32 "
    "write crc32, do not allow other algorithms to match when reading; "
  "INNODB "
    "write a software calculated checksum, allow any other checksums "
    "to match when reading; "
  "STRICT_INNODB "
    "write a software calculated checksum, do not allow other algorithms "
    "to match when reading; "
  "NONE "
    "write a constant magic number, do not do any checksum verification "
    "when reading (same as innodb_checksums=OFF); "
  "STRICT_NONE "
    "write a constant magic number, do not allow values other than that "
    "magic number when reading; "
  "Files updated when this option is set to crc32 or strict_crc32 will "
  "not be readable by MySQL versions older than 5.6.3",
  NULL, NULL, SRV_CHECKSUM_ALGORITHM_INNODB,
  &innodb_checksum_algorithm_typelib);


static MYSQL_SYSVAR_ENUM(log_checksum_algorithm, srv_log_checksum_algorithm,
  PLUGIN_VAR_RQCMDARG,
  "The algorithm InnoDB uses for log block checksums. Possible values are "
  "CRC32 (hardware accelerated if the CPU supports it) "
    "write crc32, allow any of the other checksums to match when reading; "
  "STRICT_CRC32 "
    "write crc32, do not allow other algorithms to match when reading; "
  "INNODB "
    "write a software calculated checksum, allow any other checksums "
    "to match when reading; "
  "STRICT_INNODB "
    "write a software calculated checksum, do not allow other algorithms "
    "to match when reading; "
  "NONE "
    "write a constant magic number, do not do any checksum verification "
    "when reading (same as innodb_checksums=OFF); "
  "STRICT_NONE "
    "write a constant magic number, do not allow values other than that "
    "magic number when reading; "
  "Logs created when this option is set to crc32/strict_crc32/none/strict_none "
  "will not be readable by any MySQL version or Percona Server versions that do"
  "not support this feature",
  NULL, innodb_log_checksum_algorithm_update, SRV_CHECKSUM_ALGORITHM_INNODB,
  &innodb_checksum_algorithm_typelib);


static MYSQL_SYSVAR_BOOL(checksums, innobase_use_checksums,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "DEPRECATED. Use innodb_checksum_algorithm=NONE instead of setting "
  "this to OFF. "
  "Enable InnoDB checksums validation (enabled by default). "
  "Disable with --skip-innodb-checksums.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(log_block_size, innobase_log_block_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "###EXPERIMENTAL###: The log block size of the transaction log file. Changing for created log file is not supported. Use on your own risk!",
  NULL, NULL, (1 << 9)/*512*/, OS_MIN_LOG_BLOCK_SIZE,
  (1 << UNIV_PAGE_SIZE_SHIFT_MAX), 0);

static MYSQL_SYSVAR_STR(data_home_dir, innobase_data_home_dir,
  PLUGIN_VAR_READONLY,
  "The common part for InnoDB table spaces.",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_BOOL(doublewrite, innobase_use_doublewrite,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable InnoDB doublewrite buffer (enabled by default). "
  "Disable with --skip-innodb-doublewrite.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_include_delete_marked,
  srv_stats_include_delete_marked,
  PLUGIN_VAR_OPCMDARG,
  "Scan delete marked records for persistent stat",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(use_atomic_writes, innobase_use_atomic_writes,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Prevent partial page writes, via atomic writes (beta). "
  "The option is used to prevent partial writes in case of a crash/poweroff, "
  "as faster alternative to doublewrite buffer. "
  "Currently this option works only "
  "on Linux only with FusionIO device, and directFS filesystem.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(io_capacity, srv_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Number of IOPs the server can do. Tunes the background IO rate",
  NULL, innodb_io_capacity_update, 200, 100, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(io_capacity_max, srv_max_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Limit to which innodb_io_capacity can be inflated.",
  NULL, innodb_io_capacity_max_update,
  SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT, 100,
  SRV_MAX_IO_CAPACITY_LIMIT, 0);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_BOOL(purge_run_now, innodb_purge_run_now,
  PLUGIN_VAR_OPCMDARG,
  "Set purge state to RUN",
  NULL, purge_run_now_set, FALSE);

static MYSQL_SYSVAR_BOOL(purge_stop_now, innodb_purge_stop_now,
  PLUGIN_VAR_OPCMDARG,
  "Set purge state to STOP",
  NULL, purge_stop_now_set, FALSE);

static MYSQL_SYSVAR_BOOL(log_checkpoint_now, innodb_log_checkpoint_now,
  PLUGIN_VAR_OPCMDARG,
  "Force checkpoint now",
  NULL, checkpoint_now_set, FALSE);

static MYSQL_SYSVAR_BOOL(buf_flush_list_now, innodb_buf_flush_list_now,
  PLUGIN_VAR_OPCMDARG,
  "Force dirty page flush now",
  NULL, buf_flush_list_now_set, FALSE);

static MYSQL_SYSVAR_BOOL(track_redo_log_now,
  innodb_track_redo_log_now,
  PLUGIN_VAR_OPCMDARG,
  "Force log tracker to catch up with checkpoint now",
  NULL, track_redo_log_now_set, FALSE);

#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_ULONG(purge_batch_size, srv_purge_batch_size,
  PLUGIN_VAR_OPCMDARG,
  "Number of UNDO log pages to purge in one batch from the history list.",
  NULL, NULL,
  300,			/* Default setting */
  1,			/* Minimum value */
  5000, 0);		/* Maximum value */

static MYSQL_SYSVAR_ULONG(purge_threads, srv_n_purge_threads,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Purge threads can be from 1 to 32. Default is 1.",
  NULL, NULL,
  1,			/* Default setting */
  1,			/* Minimum value */
  SRV_MAX_N_PURGE_THREADS, 0);		/* Maximum value */

static MYSQL_SYSVAR_ULONG(sync_array_size, srv_sync_array_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Size of the mutex/lock wait array.",
  NULL, NULL,
  1,			/* Default setting */
  1,			/* Minimum value */
  1024, 0);		/* Maximum value */

static MYSQL_SYSVAR_ULONG(fast_shutdown, innobase_fast_shutdown,
  PLUGIN_VAR_OPCMDARG,
  "Speeds up the shutdown process of the InnoDB storage engine. Possible "
  "values are 0, 1 (faster) or 2 (fastest - crash-like).",
  NULL, NULL, 1, 0, 2, 0);

static MYSQL_SYSVAR_BOOL(file_per_table, srv_file_per_table,
  PLUGIN_VAR_NOCMDARG,
  "Stores each InnoDB table to an .ibd file in the database dir.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_STR(file_format, innobase_file_format_name,
  PLUGIN_VAR_RQCMDARG,
  "File format to use for new tables in .ibd files.",
  innodb_file_format_name_validate,
  innodb_file_format_name_update, "Antelope");

/* "innobase_file_format_check" decides whether we would continue
booting the server if the file format stamped on the system
table space exceeds the maximum file format supported
by the server. Can be set during server startup at command
line or configure file, and a read only variable after
server startup */
static MYSQL_SYSVAR_BOOL(file_format_check, innobase_file_format_check,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Whether to perform system file format check.",
  NULL, NULL, TRUE);

/* If a new file format is introduced, the file format
name needs to be updated accordingly. Please refer to
file_format_name_map[] defined in trx0sys.cc for the next
file format name. */
static MYSQL_SYSVAR_STR(file_format_max, innobase_file_format_max,
  PLUGIN_VAR_OPCMDARG,
  "The highest file format in the tablespace.",
  innodb_file_format_max_validate,
  innodb_file_format_max_update, "Antelope");

static MYSQL_SYSVAR_STR(ft_server_stopword_table, innobase_server_stopword_table,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "The user supplied stopword table name.",
  innodb_stopword_table_validate,
  NULL,
  NULL);

static MYSQL_SYSVAR_UINT(flush_log_at_timeout, srv_flush_log_at_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Write and flush logs every (n) second.",
  NULL, NULL, 1, 0, 2700, 0);

/* Changed to the THDVAR */
//static MYSQL_SYSVAR_ULONG(flush_log_at_trx_commit, srv_flush_log_at_trx_commit,
//  PLUGIN_VAR_OPCMDARG,
//  "Set to 0 (write and flush once per second),"
//  " 1 (write and flush at each commit)"
//  " or 2 (write at commit, flush once per second).",
//  NULL, NULL, 1, 0, 2, 0);

static MYSQL_SYSVAR_BOOL(use_global_flush_log_at_trx_commit, srv_use_global_flush_log_at_trx_commit,
  PLUGIN_VAR_NOCMDARG,
  "Use global innodb_flush_log_at_trx_commit value. (default: ON).",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_STR(flush_method, innobase_file_flush_method,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "With which method to flush data.", NULL, NULL, NULL);

static MYSQL_SYSVAR_BOOL(large_prefix, innobase_large_prefix,
  PLUGIN_VAR_NOCMDARG,
  "Support large index prefix length of REC_VERSION_56_MAX_INDEX_COL_LEN (3072) bytes.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(force_load_corrupted, srv_load_corrupted,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Force InnoDB to load metadata of corrupted table.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(locks_unsafe_for_binlog, innobase_locks_unsafe_for_binlog,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "DEPRECATED. This option may be removed in future releases. "
  "Please use READ COMMITTED transaction isolation level instead. "
  "Force InnoDB to not use next-key locking, to use only row-level locking.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(show_verbose_locks, srv_show_verbose_locks,
  PLUGIN_VAR_OPCMDARG,
  "Whether to show records locked in SHOW INNODB STATUS.",
  NULL, NULL, 0, 0, 1, 0);

static MYSQL_SYSVAR_ULONG(show_locks_held, srv_show_locks_held,
  PLUGIN_VAR_RQCMDARG,
  "Number of locks held to print for each InnoDB transaction in SHOW INNODB STATUS.",
  NULL, NULL, 10, 0, 1000, 0);

#ifdef UNIV_LOG_ARCHIVE
static MYSQL_SYSVAR_STR(log_arch_dir, innobase_log_arch_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Where full logs should be archived.", NULL, NULL, NULL);

static MYSQL_SYSVAR_BOOL(log_archive, innobase_log_archive,
  PLUGIN_VAR_OPCMDARG,
  "Set to 1 if you want to have logs archived.",
  NULL, innodb_log_archive_update, FALSE);
#endif /* UNIV_LOG_ARCHIVE */

static MYSQL_SYSVAR_STR(log_group_home_dir, srv_log_group_home_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to InnoDB log files.", NULL, NULL, NULL);

static MYSQL_SYSVAR_ULONG(log_arch_expire_sec,
  srv_log_arch_expire_sec, PLUGIN_VAR_OPCMDARG,
  "Expiration time for archived innodb transaction logs.",
  NULL, innodb_log_archive_expire_update, 0, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(max_dirty_pages_pct, srv_max_buf_pool_modified_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages allowed in bufferpool.",
  NULL, innodb_max_dirty_pages_pct_update, 75, 0, 99, 0);

static MYSQL_SYSVAR_ULONG(max_dirty_pages_pct_lwm,
  srv_max_dirty_pages_pct_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages at which flushing kicks in.",
  NULL, innodb_max_dirty_pages_pct_lwm_update, 0, 0, 99, 0);

static MYSQL_SYSVAR_ULONG(adaptive_flushing_lwm,
  srv_adaptive_flushing_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of log capacity below which no adaptive flushing happens.",
  NULL, NULL, 10, 0, 70, 0);

static MYSQL_SYSVAR_BOOL(adaptive_flushing, srv_adaptive_flushing,
  PLUGIN_VAR_NOCMDARG,
  "Attempt flushing dirty pages to avoid IO bursts at checkpoints.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(flushing_avg_loops,
  srv_flushing_avg_loops,
  PLUGIN_VAR_RQCMDARG,
  "Number of iterations over which the background flushing is averaged.",
  NULL, NULL, 30, 1, 1000, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag, srv_max_purge_lag,
  PLUGIN_VAR_RQCMDARG,
  "Desired maximum length of the purge queue (0 = no limit)",
  NULL, NULL, 0, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag_delay, srv_max_purge_lag_delay,
   PLUGIN_VAR_RQCMDARG,
   "Maximum delay of user threads in micro-seconds",
   NULL, NULL,
   0L,			/* Default seting */
   0L,			/* Minimum value */
   10000000UL, 0);	/* Maximum value */

static MYSQL_SYSVAR_BOOL(rollback_on_timeout, innobase_rollback_on_timeout,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Roll back the complete transaction on lock wait timeout, for 4.x compatibility (disabled by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(status_file, innobase_create_status_file,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_NOSYSVAR,
  "Enable SHOW ENGINE INNODB STATUS output in the innodb_status.<pid> file",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(stats_on_metadata, innobase_stats_on_metadata,
  PLUGIN_VAR_OPCMDARG,
  "Enable statistics gathering for metadata commands such as "
  "SHOW TABLE STATUS for tables that use transient statistics (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONGLONG(stats_sample_pages, srv_stats_transient_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated, use innodb_stats_transient_sample_pages instead",
  NULL, innodb_stats_sample_pages_update, 8, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(stats_transient_sample_pages,
  srv_stats_transient_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating transient "
  "statistics (if persistent statistics are not used, default 8)",
  NULL, NULL, 8, 1, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(stats_persistent, srv_stats_persistent,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB persistent statistics enabled for all tables unless overridden "
  "at table level",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_auto_recalc, srv_stats_auto_recalc,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB automatic recalculation of persistent statistics enabled for all "
  "tables unless overridden at table level (automatic recalculation is only "
  "done when InnoDB decides that the table has changed too much and needs a "
  "new statistics)",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONGLONG(stats_persistent_sample_pages,
  srv_stats_persistent_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating persistent "
  "statistics (by ANALYZE, default 20)",
  NULL, NULL, 20, 1, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(adaptive_hash_index, btr_search_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB adaptive hash index (enabled by default).  "
  "Disable with --skip-innodb-adaptive-hash-index.",
  NULL, innodb_adaptive_hash_index_update, TRUE);

/* btr_search_index_num is constrained to machine word size for historical
reasons. This limitation can be easily removed later. */
static MYSQL_SYSVAR_ULONG(adaptive_hash_index_partitions, btr_search_index_num,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of InnoDB adaptive hash index partitions (default 1: disable "
  "partitioning)",
  NULL, NULL, 1, 1, sizeof(ulint) * 8, 0);

static MYSQL_SYSVAR_ULONG(replication_delay, srv_replication_delay,
  PLUGIN_VAR_RQCMDARG,
  "Replication thread delay (ms) on the slave server if "
  "innodb_thread_concurrency is reached (0 by default)",
  NULL, NULL, 0, 0, ~0UL, 0);

static MYSQL_SYSVAR_UINT(compression_level, page_zip_level,
  PLUGIN_VAR_RQCMDARG,
  "Compression level used for compressed row format.  0 is no compression"
  ", 1 is fastest, 9 is best compression and default is 6.",
  NULL, NULL, DEFAULT_COMPRESSION_LEVEL, 0, 9, 0);

static MYSQL_SYSVAR_BOOL(log_compressed_pages, page_zip_log_pages,
       PLUGIN_VAR_OPCMDARG,
  "Enables/disables the logging of entire compressed page images."
  " InnoDB logs the compressed pages to prevent corruption if"
  " the zlib compression algorithm changes."
  " When turned OFF, InnoDB will assume that the zlib"
  " compression algorithm doesn't change.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_LONG(additional_mem_pool_size, innobase_additional_mem_pool_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "DEPRECATED. This option may be removed in future releases, "
  "together with the option innodb_use_sys_malloc and with the InnoDB's "
  "internal memory allocator. "
  "Size of a memory pool InnoDB uses to store data dictionary information and other internal data structures.",
  NULL, NULL, 8*1024*1024L, 512*1024L, LONG_MAX, 1024);

static MYSQL_SYSVAR_ULONG(autoextend_increment, srv_auto_extend_increment,
  PLUGIN_VAR_RQCMDARG,
  "Data file autoextend increment in megabytes",
  NULL, NULL, 64L, 1L, 1000L, 0);

static MYSQL_SYSVAR_LONGLONG(buffer_pool_size, innobase_buffer_pool_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The size of the memory buffer InnoDB uses to cache data and indexes of its tables.",
  NULL, NULL, 128*1024*1024L, 5*1024*1024L, LONGLONG_MAX, 1024*1024L);

static MYSQL_SYSVAR_BOOL(buffer_pool_populate, srv_numa_interleave,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Depricated. This option is temporary alias of --innodb-numa-interleave.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ENUM(foreground_preflush, srv_foreground_preflush,
  PLUGIN_VAR_OPCMDARG,
  "The algorithm InnoDB uses for the query threads at sync preflush.  "
  "Possible values are "
  "SYNC_PREFLUSH: perform a sync preflush as Oracle MySQL; "
  "EXPONENTIAL_BACKOFF: (default) wait for the page cleaner flush.",
  NULL, NULL, SRV_FOREGROUND_PREFLUSH_EXP_BACKOFF,
  &innodb_foreground_preflush_typelib);

#ifdef UNIV_LINUX

static MYSQL_SYSVAR_ULONG(sched_priority_cleaner, srv_sched_priority_cleaner,
  PLUGIN_VAR_RQCMDARG,
  "Nice value for the cleaner and LRU manager thread scheduling",
  NULL, innodb_sched_priority_cleaner_update, 19, 0, 39, 0);

#endif /* UNIV_LINUX */

#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
static MYSQL_SYSVAR_ULONG(page_hash_locks, srv_n_page_hash_locks,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Number of rw_locks protecting buffer pool page_hash. Rounded up to the next power of 2",
  NULL, NULL, 16, 1, MAX_PAGE_HASH_LOCKS, 0);

static MYSQL_SYSVAR_ULONG(doublewrite_batch_size, srv_doublewrite_batch_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Number of pages reserved in doublewrite buffer for batch flushing",
  NULL, NULL, 120, 1, 127, 0);

#ifdef UNIV_LINUX

static MYSQL_SYSVAR_ULONG(sched_priority_purge, srv_sched_priority_purge,
  PLUGIN_VAR_RQCMDARG,
  "Nice value for the purge thread scheduling",
  NULL, innodb_sched_priority_purge_update, 19, 0, 39, 0);

static MYSQL_SYSVAR_ULONG(sched_priority_io, srv_sched_priority_io,
  PLUGIN_VAR_RQCMDARG,
  "Nice value for the I/O handler thread scheduling",
  NULL, innodb_sched_priority_io_update, 19, 0, 39, 0);

static MYSQL_SYSVAR_ULONG(sched_priority_master, srv_sched_priority_master,
  PLUGIN_VAR_RQCMDARG,
  "Nice value for the master thread scheduling",
  NULL, innodb_sched_priority_master_update, 19, 0, 39, 0);

static MYSQL_SYSVAR_BOOL(priority_purge, srv_purge_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Make purge coordinator and worker threads acquire shared resources with "
  "priority", NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(priority_io, srv_io_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Make I/O threads acquire shared resources with priority",
   NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(priority_cleaner, srv_cleaner_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Make buffer pool cleaner and LRU manager threads acquire shared resources "
  "with priority",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(priority_master, srv_master_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Make buffer pool cleaner thread acquire shared resources with priority",
   NULL, NULL, FALSE);

#endif /* UNIV_LINUX */

static MYSQL_SYSVAR_ULONG(cleaner_max_lru_time, srv_cleaner_max_lru_time,
  PLUGIN_VAR_RQCMDARG,
  "The maximum time limit for a single LRU tail flush iteration by the page "
  "cleaner thread in miliseconds",
  NULL, NULL, 1000, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(cleaner_max_flush_time, srv_cleaner_max_flush_time,
  PLUGIN_VAR_RQCMDARG,
  "The maximum time limit for a single flush list flush iteration by the page "
  "cleaner thread in miliseconds",
  NULL, NULL, 1000, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(cleaner_flush_chunk_size,
  srv_cleaner_flush_chunk_size,
  PLUGIN_VAR_RQCMDARG,
  "Divide page cleaner flush list flush batches into chunks of this size",
  NULL, NULL, 100, 1, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(cleaner_lru_chunk_size,
  srv_cleaner_lru_chunk_size,
  PLUGIN_VAR_RQCMDARG,
  "Divide page cleaner LRU list flush batches into chunks of this size",
  NULL, NULL, 100, 1, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(cleaner_free_list_lwm, srv_cleaner_free_list_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Page cleaner will keep on flushing the same buffer pool instance if its "
  "free list length is below this percentage of innodb_lru_scan_depth",
  NULL, NULL, 10, 0, 100, 0);

static MYSQL_SYSVAR_BOOL(cleaner_eviction_factor, srv_cleaner_eviction_factor,
  PLUGIN_VAR_OPCMDARG,
  "Make page cleaner LRU flushes use evicted instead of flushed page counts "
  "for its heuristics",
  NULL, NULL, FALSE);

#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */

static MYSQL_SYSVAR_ENUM(cleaner_lsn_age_factor,
  srv_cleaner_lsn_age_factor,
  PLUGIN_VAR_OPCMDARG,
  "The formula for LSN age factor for page cleaner adaptive flushing. "
  "LEGACY: Original Oracle MySQL 5.6 formula. "
  "HIGH_CHECKPOINT: (the default) Percona Server 5.6 formula.",
  NULL, NULL, SRV_CLEANER_LSN_AGE_FACTOR_HIGH_CHECKPOINT,
  &innodb_cleaner_lsn_age_factor_typelib);

static MYSQL_SYSVAR_ENUM(empty_free_list_algorithm,
  srv_empty_free_list_algorithm,
  PLUGIN_VAR_OPCMDARG,
  "The algorithm to use for empty free list handling.  Allowed values: "
  "LEGACY: Original Oracle MySQL 5.6 handling with single page flushes; "
  "BACKOFF: (default) Wait until cleaner produces a free page.",
  innodb_srv_empty_free_list_algorithm_validate, NULL, SRV_EMPTY_FREE_LIST_BACKOFF,
  &innodb_empty_free_list_algorithm_typelib);

static MYSQL_SYSVAR_LONG(buffer_pool_instances, innobase_buffer_pool_instances,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of buffer pool instances, set to higher value on high-end machines to increase scalability",
  NULL, NULL, 0L, 0L, MAX_BUFFER_POOLS, 1L);

static MYSQL_SYSVAR_STR(buffer_pool_filename, srv_buf_dump_filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Filename to/from which to dump/load the InnoDB buffer pool",
  innodb_srv_buf_dump_filename_validate, NULL, SRV_BUF_DUMP_FILENAME_DEFAULT);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_now, innodb_buffer_pool_dump_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate dump of the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_dump_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_at_shutdown, srv_buffer_pool_dump_at_shutdown,
  PLUGIN_VAR_RQCMDARG,
  "Dump the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, NULL, FALSE);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_STR(buffer_pool_evict, srv_buffer_pool_evict,
  PLUGIN_VAR_RQCMDARG,
  "Evict pages from the buffer pool",
  NULL, innodb_buffer_pool_evict_update, "");
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_BOOL(buffer_pool_load_now, innodb_buffer_pool_load_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate load of the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_load_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_load_abort, innodb_buffer_pool_load_abort,
  PLUGIN_VAR_RQCMDARG,
  "Abort a currently running load of the buffer pool",
  NULL, buffer_pool_load_abort, FALSE);

/* there is no point in changing this during runtime, thus readonly */
static MYSQL_SYSVAR_BOOL(buffer_pool_load_at_startup, srv_buffer_pool_load_at_startup,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Load the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(lru_scan_depth, srv_LRU_scan_depth,
  PLUGIN_VAR_RQCMDARG,
  "How deep to scan LRU to keep it clean",
  NULL, NULL, 1024, 100, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(flush_neighbors, srv_flush_neighbors,
  PLUGIN_VAR_OPCMDARG,
  "Set to 0 (don't flush neighbors from buffer pool),"
  " 1 (flush contiguous neighbors from buffer pool)"
  " or 2 (flush neighbors from buffer pool),"
  " when flushing a block",
  NULL, NULL, 1, 0, 2, 0);

static MYSQL_SYSVAR_ULONG(commit_concurrency, innobase_commit_concurrency,
  PLUGIN_VAR_RQCMDARG,
  "Helps in performance tuning in heavily concurrent environments.",
  innobase_commit_concurrency_validate, NULL, 0, 0, 1000, 0);

static MYSQL_SYSVAR_ULONG(concurrency_tickets, srv_n_free_tickets_to_enter,
  PLUGIN_VAR_RQCMDARG,
  "Number of times a thread is allowed to enter InnoDB within the same SQL query after it has once got the ticket",
  NULL, NULL, 5000L, 1L, ~0UL, 0);

static MYSQL_SYSVAR_LONG(kill_idle_transaction, srv_kill_idle_transaction,
  PLUGIN_VAR_RQCMDARG,
  "A deprecated alias of kill_idle_transaction server variable.",
  NULL, innodb_kill_idle_transaction_update, 0, 0, LONG_TIMEOUT, 0);

static MYSQL_SYSVAR_LONG(file_io_threads, innobase_file_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR,
  "Number of file I/O threads in InnoDB.",
  NULL, NULL, 4, 4, 64, 0);

static MYSQL_SYSVAR_BOOL(ft_enable_diag_print, fts_enable_diag_print,
  PLUGIN_VAR_OPCMDARG,
  "Whether to enable additional FTS diagnostic printout ",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(disable_sort_file_cache, srv_disable_sort_file_cache,
  PLUGIN_VAR_OPCMDARG,
  "Whether to disable OS system file cache for sort I/O",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(ft_aux_table, fts_internal_tbl_name2,
  PLUGIN_VAR_NOCMDARG,
  "FTS internal auxiliary table to be checked",
  innodb_internal_table_validate,
  innodb_internal_table_update, NULL);

static MYSQL_SYSVAR_ULONG(ft_cache_size, fts_max_cache_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search cache size in bytes",
  NULL, NULL, 8000000, 1600000, 80000000, 0);

static MYSQL_SYSVAR_ULONG(ft_total_cache_size, fts_max_total_cache_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Total memory allocated for InnoDB Fulltext Search cache",
  NULL, NULL, 640000000, 32000000, 1600000000, 0);

static MYSQL_SYSVAR_ULONG(ft_result_cache_limit, fts_result_cache_limit,
  PLUGIN_VAR_RQCMDARG,
  "InnoDB Fulltext search query result cache limit in bytes",
  NULL, NULL, 2000000000L, 1000000L, 4294967295UL, 0);

static MYSQL_SYSVAR_ULONG(ft_min_token_size, fts_min_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search minimum token size in characters",
  NULL, NULL, 3, 0, 16, 0);

static MYSQL_SYSVAR_ULONG(ft_max_token_size, fts_max_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search maximum token size in characters",
  NULL, NULL, FTS_MAX_WORD_LEN_IN_CHAR, 10, FTS_MAX_WORD_LEN_IN_CHAR, 0);


static MYSQL_SYSVAR_ULONG(ft_num_word_optimize, fts_num_word_optimize,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB Fulltext search number of words to optimize for each optimize table call ",
  NULL, NULL, 2000, 1000, 10000, 0);

static MYSQL_SYSVAR_ULONG(ft_sort_pll_degree, fts_sort_pll_degree,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search parallel sort degree, will round up to nearest power of 2 number",
  NULL, NULL, 2, 1, 16, 0);

static MYSQL_SYSVAR_ULONG(sort_buffer_size, srv_sort_buf_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Memory buffer size for index creation",
  NULL, NULL, 1048576, 65536, 64<<20, 0);

static MYSQL_SYSVAR_ULONGLONG(online_alter_log_max_size, srv_online_max_size,
  PLUGIN_VAR_RQCMDARG,
  "Maximum modification log file size for online index creation",
  NULL, NULL, 128<<20, 65536, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(optimize_fulltext_only, innodb_optimize_fulltext_only,
  PLUGIN_VAR_NOCMDARG,
  "Only optimize the Fulltext index of the table",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(read_io_threads, innobase_read_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of background read I/O threads in InnoDB.",
  NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(write_io_threads, innobase_write_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of background write I/O threads in InnoDB.",
  NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(force_recovery, srv_force_recovery,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Helps to save your data in case the disk image of the database becomes corrupt.",
  NULL, NULL, 0, 0, 6, 0);

#ifndef DBUG_OFF
static MYSQL_SYSVAR_ULONG(force_recovery_crash, srv_force_recovery_crash,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Kills the server during crash recovery.",
  NULL, NULL, 0, 0, 10, 0);
#endif /* !DBUG_OFF */

static MYSQL_SYSVAR_ULONG(page_size, srv_page_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Page size to use for all InnoDB tablespaces.",
  NULL, NULL, UNIV_PAGE_SIZE_DEF,
  UNIV_PAGE_SIZE_MIN, UNIV_PAGE_SIZE_MAX, 0);

static MYSQL_SYSVAR_LONG(log_buffer_size, innobase_log_buffer_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The size of the buffer which InnoDB uses to write log to the log files on disk.",
  NULL, NULL, 8*1024*1024L, 256*1024L, LONG_MAX, 1024);

static MYSQL_SYSVAR_LONGLONG(log_file_size, innobase_log_file_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Size of each log file in a log group.",
  NULL, NULL, 48*1024*1024L, 1*1024*1024L, LONGLONG_MAX, 1024*1024L);

static MYSQL_SYSVAR_ULONG(log_files_in_group, srv_n_log_files,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of log files in the log group. InnoDB writes to the files in a circular fashion.",
  NULL, NULL, 2, 2, SRV_N_LOG_FILES_MAX, 0);

/* Note that the default and minimum values are set to 0 to
detect if the option is passed and print deprecation message */
static MYSQL_SYSVAR_LONG(mirrored_log_groups, innobase_mirrored_log_groups,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of identical copies of log groups we keep for the database. Currently this should be set to 1.",
  NULL, NULL, 0, 0, 10, 0);

static MYSQL_SYSVAR_UINT(old_blocks_pct, innobase_old_blocks_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of the buffer pool to reserve for 'old' blocks.",
  NULL, innodb_old_blocks_pct_update, 100 * 3 / 8, 5, 95, 0);

static MYSQL_SYSVAR_UINT(old_blocks_time, buf_LRU_old_threshold_ms,
  PLUGIN_VAR_RQCMDARG,
  "Move blocks to the 'new' end of the buffer pool if the first access"
  " was at least this many milliseconds ago."
  " The timeout is disabled if 0.",
  NULL, NULL, 1000, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_LONG(open_files, innobase_open_files,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "How many files at the maximum InnoDB keeps open at the same time.",
  NULL, NULL, 0L, 0L, LONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(sync_spin_loops, srv_n_spin_wait_rounds,
  PLUGIN_VAR_RQCMDARG,
  "Count of spin-loop rounds in InnoDB mutexes (30 by default)",
  NULL, NULL, 30L, 0L, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(spin_wait_delay, srv_spin_wait_delay,
  PLUGIN_VAR_OPCMDARG,
  "Maximum delay between polling for a spin lock (6 by default)",
  NULL, NULL, 6L, 0L, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(thread_concurrency, srv_thread_concurrency,
  PLUGIN_VAR_RQCMDARG,
  "Helps in performance tuning in heavily concurrent environments. Sets the maximum number of threads allowed inside InnoDB. Value 0 will disable the thread throttling.",
  NULL, NULL, 0, 0, 1000, 0);

#ifdef HAVE_ATOMIC_BUILTINS
static MYSQL_SYSVAR_ULONG(
  adaptive_max_sleep_delay, srv_adaptive_max_sleep_delay,
  PLUGIN_VAR_RQCMDARG,
  "The upper limit of the sleep delay in usec. Value of 0 disables it.",
  NULL, NULL,
  150000,			/* Default setting */
  0,				/* Minimum value */
  1000000, 0);			/* Maximum value */
#endif /* HAVE_ATOMIC_BUILTINS */

static MYSQL_SYSVAR_ULONG(thread_sleep_delay, srv_thread_sleep_delay,
  PLUGIN_VAR_RQCMDARG,
  "Time of innodb thread sleeping before joining InnoDB queue (usec). "
  "Value 0 disable a sleep",
  NULL, NULL,
  10000L,
  0L,
  1000000L, 0);

static MYSQL_SYSVAR_STR(data_file_path, innobase_data_file_path,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to individual files and their sizes.",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(undo_directory, srv_undo_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Directory where undo tablespace files live, this path can be absolute.",
  NULL, NULL, ".");

static MYSQL_SYSVAR_ULONG(undo_tablespaces, srv_undo_tablespaces,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of undo tablespaces to use. ",
  NULL, NULL,
  0L,			/* Default seting */
  0L,			/* Minimum value */
  126L, 0);		/* Maximum value */

static MYSQL_SYSVAR_ULONG(undo_logs, srv_undo_logs,
  PLUGIN_VAR_OPCMDARG,
  "Number of undo logs to use.",
  NULL, NULL,
  TRX_SYS_N_RSEGS,	/* Default setting */
  1,			/* Minimum value */
  TRX_SYS_N_RSEGS, 0);	/* Maximum value */

/* Alias for innodb_undo_logs, this config variable is deprecated. */
static MYSQL_SYSVAR_ULONG(rollback_segments, srv_undo_logs,
  PLUGIN_VAR_OPCMDARG,
  "Number of undo logs to use (deprecated).",
  NULL, NULL,
  TRX_SYS_N_RSEGS,	/* Default setting */
  1,			/* Minimum value */
  TRX_SYS_N_RSEGS, 0);	/* Maximum value */

static MYSQL_SYSVAR_LONG(autoinc_lock_mode, innobase_autoinc_lock_mode,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The AUTOINC lock modes supported by InnoDB:               "
  "0 => Old style AUTOINC locking (for backward"
  " compatibility)                                           "
  "1 => New style AUTOINC locking                            "
  "2 => No AUTOINC locking (unsafe for SBR)",
  NULL, NULL,
#ifdef WITH_WSREP
  AUTOINC_NO_LOCKING,           /* Default setting: changed for galera Bug#1243228 */
#else 
  AUTOINC_NEW_STYLE_LOCKING,   /*  Default setting */
#endif
  AUTOINC_OLD_STYLE_LOCKING,	/* Minimum value */
  AUTOINC_NO_LOCKING, 0);	/* Maximum value */

static MYSQL_SYSVAR_STR(version, innodb_version_str,
  PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
  "Percona-InnoDB-plugin version", NULL, NULL, INNODB_VERSION_STR);

static MYSQL_SYSVAR_BOOL(use_sys_malloc, srv_use_sys_malloc,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "DEPRECATED. This option may be removed in future releases, "
  "together with the InnoDB's internal memory allocator. "
  "Use OS memory allocator instead of InnoDB's internal memory allocator",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(use_native_aio, srv_use_native_aio,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use native AIO if supported on this platform.",
  NULL, NULL, TRUE);

#ifdef HAVE_LIBNUMA
static MYSQL_SYSVAR_BOOL(numa_interleave, srv_numa_interleave,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use NUMA interleave memory policy to allocate InnoDB buffer pool.",
  NULL, NULL, FALSE);
#endif // HAVE_LIBNUMA

static MYSQL_SYSVAR_BOOL(api_enable_binlog, ib_binlog_enabled,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable binlog for applications direct access InnoDB through InnoDB APIs",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(api_enable_mdl, ib_mdl_enabled,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable MDL for applications direct access InnoDB through InnoDB APIs",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(api_disable_rowlock, ib_disable_row_lock,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Disable row lock when direct access InnoDB through InnoDB APIs",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(api_trx_level, ib_trx_level_setting,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB API transaction isolation level",
  NULL, NULL,
  0,		/* Default setting */
  0,		/* Minimum value */
  3, 0);	/* Maximum value */

static MYSQL_SYSVAR_ULONG(api_bk_commit_interval, ib_bk_commit_interval,
  PLUGIN_VAR_OPCMDARG,
  "Background commit interval in seconds",
  NULL, NULL,
  5,		/* Default setting */
  1,		/* Minimum value */
  1024 * 1024 * 1024, 0);	/* Maximum value */

static MYSQL_SYSVAR_STR(change_buffering, innobase_change_buffering,
  PLUGIN_VAR_RQCMDARG,
  "Buffer changes to reduce random access: "
  "OFF, ON, inserting, deleting, changing, or purging.",
  innodb_change_buffering_validate,
  innodb_change_buffering_update, "all");

static MYSQL_SYSVAR_UINT(change_buffer_max_size,
  innobase_change_buffer_max_size,
  PLUGIN_VAR_RQCMDARG,
  "Maximum on-disk size of change buffer in terms of percentage"
  " of the buffer pool.",
  NULL, innodb_change_buffer_max_size_update,
  CHANGE_BUFFER_DEFAULT_SIZE, 0, 50, 0);

static MYSQL_SYSVAR_ENUM(stats_method, srv_innodb_stats_method,
   PLUGIN_VAR_RQCMDARG,
  "Specifies how InnoDB index statistics collection code should "
  "treat NULLs. Possible values are NULLS_EQUAL (default), "
  "NULLS_UNEQUAL and NULLS_IGNORED",
   NULL, NULL, SRV_STATS_NULLS_EQUAL, &innodb_stats_method_typelib);

static MYSQL_SYSVAR_BOOL(track_changed_pages, srv_track_changed_pages,
  PLUGIN_VAR_NOCMDARG
#ifndef UNIV_DEBUG
  /* Make this variable dynamic for debug builds to
  provide a testcase sync facility */
  | PLUGIN_VAR_READONLY
#endif
  ,
  "Track the redo log for changed pages and output a changed page bitmap",
#ifdef UNIV_DEBUG
  innodb_track_changed_pages_validate,
#else
  NULL,
#endif
  NULL, FALSE);

static MYSQL_SYSVAR_ULONGLONG(max_bitmap_file_size, srv_max_bitmap_file_size,
    PLUGIN_VAR_RQCMDARG,
    "The maximum size of changed page bitmap files",
    NULL, NULL, 100*1024*1024ULL, 4096ULL, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_ULONGLONG(max_changed_pages, srv_max_changed_pages,
  PLUGIN_VAR_RQCMDARG,
  "The maximum number of rows for "
  "INFORMATION_SCHEMA.INNODB_CHANGED_PAGES table, "
  "0 - unlimited",
  NULL, NULL, 1000000, 0, ~0ULL, 0);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
static MYSQL_SYSVAR_UINT(change_buffering_debug, ibuf_debug,
  PLUGIN_VAR_RQCMDARG,
  "Debug flags for InnoDB change buffering (0=none, 2=crash at merge)",
  NULL, NULL, 0, 0, 2, 0);

static MYSQL_SYSVAR_BOOL(disable_background_merge,
  srv_ibuf_disable_background_merge,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_RQCMDARG,
  "Disable change buffering merges by the master thread",
  NULL, NULL, FALSE);
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

#ifdef WITH_INNODB_DISALLOW_WRITES
/*******************************************************
 *    innobase_disallow_writes variable definition     *
 *******************************************************/
 
/* Must always init to FALSE. */
static my_bool	innobase_disallow_writes	= FALSE;

/**************************************************************************
An "update" method for innobase_disallow_writes variable. */
static
void
innobase_disallow_writes_update(
/*============================*/
	THD*			thd,		/* in: thread handle */
	st_mysql_sys_var*	var,		/* in: pointer to system
						variable */
	void*			var_ptr,	/* out: pointer to dynamic
						variable */
	const void*		save)		/* in: temporary storage */
{
	*(my_bool*)var_ptr = *(my_bool*)save;
	ut_a(srv_allow_writes_event);
	if (*(my_bool*)var_ptr)
		os_event_reset(srv_allow_writes_event);
	else
		os_event_set(srv_allow_writes_event);
}

static MYSQL_SYSVAR_BOOL(disallow_writes, innobase_disallow_writes,
  PLUGIN_VAR_NOCMDOPT,
  "Tell InnoDB to stop any writes to disk",
  NULL, innobase_disallow_writes_update, FALSE);
#endif /* WITH_INNODB_DISALLOW_WRITES */
static MYSQL_SYSVAR_BOOL(random_read_ahead, srv_random_read_ahead,
  PLUGIN_VAR_NOCMDARG,
  "Whether to use read ahead for random access within an extent.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(read_ahead_threshold, srv_read_ahead_threshold,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages that must be accessed sequentially for InnoDB to "
  "trigger a readahead.",
  NULL, NULL, 56, 0, 64, 0);

static MYSQL_SYSVAR_STR(monitor_enable, innobase_enable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn on a monitor counter",
  innodb_monitor_validate,
  innodb_enable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_disable, innobase_disable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn off a monitor counter",
  innodb_monitor_validate,
  innodb_disable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset, innobase_reset_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset a monitor counter",
  innodb_monitor_validate,
  innodb_reset_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset_all, innobase_reset_all_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset all values for a monitor counter",
  innodb_monitor_validate,
  innodb_reset_all_monitor_update, NULL);

static MYSQL_SYSVAR_BOOL(status_output, srv_print_innodb_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB monitor output to the error log.",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(status_output_locks, srv_print_innodb_lock_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB lock monitor output to the error log."
  " Requires innodb_status_output=ON.",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(print_all_deadlocks, srv_print_all_deadlocks,
  PLUGIN_VAR_OPCMDARG,
  "Print all deadlocks to MySQL error log (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(
  print_lock_wait_timeout_info,
  srv_print_lock_wait_timeout_info,
  PLUGIN_VAR_OPCMDARG,
  "Print lock wait timeout info to MySQL error log (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(compression_failure_threshold_pct,
  zip_failure_threshold_pct, PLUGIN_VAR_OPCMDARG,
  "If the compression failure rate of a table is greater than this number"
  " more padding is added to the pages to reduce the failures. A value of"
  " zero implies no padding",
  NULL, NULL, 5, 0, 100, 0);

static MYSQL_SYSVAR_ULONG(compression_pad_pct_max,
  zip_pad_max, PLUGIN_VAR_OPCMDARG,
  "Percentage of empty space on a data page that can be reserved"
  " to make the page compressible.",
  NULL, NULL, 50, 0, 75, 0);

static MYSQL_SYSVAR_BOOL(read_only, srv_read_only_mode,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Start InnoDB in read only mode (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(cmp_per_index_enabled, srv_cmp_per_index_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable INFORMATION_SCHEMA.innodb_cmp_per_index, "
  "may have negative impact on performance (off by default)",
  NULL, innodb_cmp_per_index_update, FALSE);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_UINT(trx_rseg_n_slots_debug, trx_rseg_n_slots_debug,
  PLUGIN_VAR_RQCMDARG,
  "Debug flags for InnoDB to limit TRX_RSEG_N_SLOTS for trx_rsegf_undo_find_free()",
  NULL, NULL, 0, 0, 1024, 0);

static MYSQL_SYSVAR_UINT(limit_optimistic_insert_debug,
  btr_cur_limit_optimistic_insert_debug, PLUGIN_VAR_RQCMDARG,
  "Artificially limit the number of records per B-tree page (0=unlimited).",
  NULL, NULL, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(trx_purge_view_update_only_debug,
  srv_purge_view_update_only_debug, PLUGIN_VAR_NOCMDARG,
  "Pause actual purging any delete-marked records, but merely update the purge view. "
  "It is to create artificially the situation the purge view have been updated "
  "but the each purges were not done yet.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(fil_make_page_dirty_debug,
  srv_fil_make_page_dirty_debug, PLUGIN_VAR_OPCMDARG,
  "Make the first page of the given tablespace dirty.",
  NULL, innodb_make_page_dirty, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_ULONG(saved_page_number_debug,
  srv_saved_page_number_debug, PLUGIN_VAR_OPCMDARG,
  "An InnoDB page number.",
  NULL, innodb_save_page_no, 0, 0, UINT_MAX32, 0);
#endif /* UNIV_DEBUG */

static const char *corrupt_table_action_names[]=
{
  "assert", /* 0 */
  "warn", /* 1 */
  "salvage", /* 2 */
  NullS
};
static TYPELIB corrupt_table_action_typelib=
{
  array_elements(corrupt_table_action_names) - 1, "corrupt_table_action_typelib",
  corrupt_table_action_names, NULL
};
static	MYSQL_SYSVAR_ENUM(corrupt_table_action, srv_pass_corrupt_table,
  PLUGIN_VAR_RQCMDARG,
  "Warn corruptions of user tables as 'corrupt table' instead of not crashing itself, "
  "when used with file_per_table. "
  "All file io for the datafile after detected as corrupt are disabled, "
  "except for the deletion.",
  NULL, NULL, 0, &corrupt_table_action_typelib);

static MYSQL_SYSVAR_UINT(compressed_columns_zip_level,
  srv_compressed_columns_zip_level,
  PLUGIN_VAR_RQCMDARG,
  "Compression level used for compressed columns.  0 is no compression"
  ", 1 is fastest and 9 is best compression. Default is 6.",
  NULL, NULL, DEFAULT_COMPRESSION_LEVEL, 0, 9, 0);

static MYSQL_SYSVAR_ULONG(compressed_columns_threshold,
  srv_compressed_columns_threshold,
  PLUGIN_VAR_RQCMDARG,
  "Compress column data if its length exceeds this value. Default is 96",
  NULL, NULL, 96, 1, ~0UL, 0);

static struct st_mysql_sys_var* innobase_system_variables[]= {
  MYSQL_SYSVAR(log_block_size),
  MYSQL_SYSVAR(additional_mem_pool_size),
  MYSQL_SYSVAR(api_trx_level),
  MYSQL_SYSVAR(api_bk_commit_interval),
  MYSQL_SYSVAR(autoextend_increment),
  MYSQL_SYSVAR(buffer_pool_size),
  MYSQL_SYSVAR(buffer_pool_populate),
  MYSQL_SYSVAR(buffer_pool_instances),
  MYSQL_SYSVAR(buffer_pool_filename),
  MYSQL_SYSVAR(buffer_pool_dump_now),
  MYSQL_SYSVAR(buffer_pool_dump_at_shutdown),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buffer_pool_evict),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(buffer_pool_load_now),
  MYSQL_SYSVAR(buffer_pool_load_abort),
  MYSQL_SYSVAR(buffer_pool_load_at_startup),
  MYSQL_SYSVAR(lru_scan_depth),
  MYSQL_SYSVAR(flush_neighbors),
  MYSQL_SYSVAR(checksum_algorithm),
  MYSQL_SYSVAR(log_checksum_algorithm),
  MYSQL_SYSVAR(checksums),
  MYSQL_SYSVAR(commit_concurrency),
  MYSQL_SYSVAR(concurrency_tickets),
  MYSQL_SYSVAR(compression_level),
  MYSQL_SYSVAR(kill_idle_transaction),
  MYSQL_SYSVAR(data_file_path),
  MYSQL_SYSVAR(data_home_dir),
  MYSQL_SYSVAR(doublewrite),
  MYSQL_SYSVAR(stats_include_delete_marked),
  MYSQL_SYSVAR(api_enable_binlog),
  MYSQL_SYSVAR(api_enable_mdl),
  MYSQL_SYSVAR(api_disable_rowlock),
  MYSQL_SYSVAR(use_atomic_writes),
  MYSQL_SYSVAR(fast_shutdown),
  MYSQL_SYSVAR(file_io_threads),
  MYSQL_SYSVAR(read_io_threads),
  MYSQL_SYSVAR(write_io_threads),
  MYSQL_SYSVAR(file_per_table),
  MYSQL_SYSVAR(file_format),
  MYSQL_SYSVAR(file_format_check),
  MYSQL_SYSVAR(file_format_max),
  MYSQL_SYSVAR(flush_log_at_timeout),
  MYSQL_SYSVAR(flush_log_at_trx_commit),
  MYSQL_SYSVAR(use_global_flush_log_at_trx_commit),
  MYSQL_SYSVAR(flush_method),
  MYSQL_SYSVAR(force_recovery),
#ifndef DBUG_OFF
  MYSQL_SYSVAR(force_recovery_crash),
#endif /* !DBUG_OFF */
  MYSQL_SYSVAR(ft_cache_size),
  MYSQL_SYSVAR(ft_total_cache_size),
  MYSQL_SYSVAR(ft_result_cache_limit),
  MYSQL_SYSVAR(ft_enable_stopword),
  MYSQL_SYSVAR(ft_max_token_size),
  MYSQL_SYSVAR(ft_min_token_size),
  MYSQL_SYSVAR(ft_num_word_optimize),
  MYSQL_SYSVAR(ft_sort_pll_degree),
  MYSQL_SYSVAR(large_prefix),
  MYSQL_SYSVAR(force_load_corrupted),
  MYSQL_SYSVAR(locks_unsafe_for_binlog),
  MYSQL_SYSVAR(lock_wait_timeout),
#ifdef UNIV_LOG_ARCHIVE
  MYSQL_SYSVAR(log_arch_dir),
  MYSQL_SYSVAR(log_archive),
  MYSQL_SYSVAR(log_arch_expire_sec),
#endif /* UNIV_LOG_ARCHIVE */
  MYSQL_SYSVAR(page_size),
  MYSQL_SYSVAR(log_buffer_size),
  MYSQL_SYSVAR(log_file_size),
  MYSQL_SYSVAR(log_files_in_group),
  MYSQL_SYSVAR(log_group_home_dir),
  MYSQL_SYSVAR(log_compressed_pages),
  MYSQL_SYSVAR(max_dirty_pages_pct),
  MYSQL_SYSVAR(max_dirty_pages_pct_lwm),
  MYSQL_SYSVAR(adaptive_flushing_lwm),
  MYSQL_SYSVAR(adaptive_flushing),
  MYSQL_SYSVAR(flushing_avg_loops),
  MYSQL_SYSVAR(max_purge_lag),
  MYSQL_SYSVAR(max_purge_lag_delay),
  MYSQL_SYSVAR(mirrored_log_groups),
  MYSQL_SYSVAR(old_blocks_pct),
  MYSQL_SYSVAR(old_blocks_time),
  MYSQL_SYSVAR(open_files),
  MYSQL_SYSVAR(optimize_fulltext_only),
  MYSQL_SYSVAR(rollback_on_timeout),
  MYSQL_SYSVAR(ft_aux_table),
  MYSQL_SYSVAR(ft_enable_diag_print),
  MYSQL_SYSVAR(ft_server_stopword_table),
  MYSQL_SYSVAR(ft_user_stopword_table),
  MYSQL_SYSVAR(disable_sort_file_cache),
  MYSQL_SYSVAR(stats_on_metadata),
  MYSQL_SYSVAR(stats_sample_pages),
  MYSQL_SYSVAR(stats_transient_sample_pages),
  MYSQL_SYSVAR(stats_persistent),
  MYSQL_SYSVAR(stats_persistent_sample_pages),
  MYSQL_SYSVAR(stats_auto_recalc),
  MYSQL_SYSVAR(adaptive_hash_index),
  MYSQL_SYSVAR(adaptive_hash_index_partitions),
  MYSQL_SYSVAR(stats_method),
  MYSQL_SYSVAR(replication_delay),
  MYSQL_SYSVAR(status_file),
  MYSQL_SYSVAR(strict_mode),
  MYSQL_SYSVAR(support_xa),
  MYSQL_SYSVAR(sort_buffer_size),
  MYSQL_SYSVAR(online_alter_log_max_size),
  MYSQL_SYSVAR(sync_spin_loops),
  MYSQL_SYSVAR(spin_wait_delay),
  MYSQL_SYSVAR(table_locks),
  MYSQL_SYSVAR(thread_concurrency),
#ifdef HAVE_ATOMIC_BUILTINS
  MYSQL_SYSVAR(adaptive_max_sleep_delay),
#endif /* HAVE_ATOMIC_BUILTINS */
  MYSQL_SYSVAR(thread_sleep_delay),
  MYSQL_SYSVAR(autoinc_lock_mode),
  MYSQL_SYSVAR(show_verbose_locks),
  MYSQL_SYSVAR(show_locks_held),
  MYSQL_SYSVAR(version),
  MYSQL_SYSVAR(use_sys_malloc),
  MYSQL_SYSVAR(use_native_aio),
#ifdef HAVE_LIBNUMA
  MYSQL_SYSVAR(numa_interleave),
#endif // HAVE_LIBNUMA
  MYSQL_SYSVAR(change_buffering),
  MYSQL_SYSVAR(change_buffer_max_size),
  MYSQL_SYSVAR(track_changed_pages),
  MYSQL_SYSVAR(max_bitmap_file_size),
  MYSQL_SYSVAR(max_changed_pages),
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
  MYSQL_SYSVAR(change_buffering_debug),
  MYSQL_SYSVAR(disable_background_merge),
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
#ifdef WITH_INNODB_DISALLOW_WRITES
  MYSQL_SYSVAR(disallow_writes),
#endif /* WITH_INNODB_DISALLOW_WRITES */
  MYSQL_SYSVAR(random_read_ahead),
  MYSQL_SYSVAR(read_ahead_threshold),
  MYSQL_SYSVAR(read_only),
  MYSQL_SYSVAR(io_capacity),
  MYSQL_SYSVAR(io_capacity_max),
  MYSQL_SYSVAR(monitor_enable),
  MYSQL_SYSVAR(monitor_disable),
  MYSQL_SYSVAR(monitor_reset),
  MYSQL_SYSVAR(monitor_reset_all),
  MYSQL_SYSVAR(purge_threads),
  MYSQL_SYSVAR(purge_batch_size),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(purge_run_now),
  MYSQL_SYSVAR(purge_stop_now),
  MYSQL_SYSVAR(log_checkpoint_now),
  MYSQL_SYSVAR(buf_flush_list_now),
  MYSQL_SYSVAR(track_redo_log_now),
#endif /* UNIV_DEBUG */
#ifdef UNIV_LINUX
  MYSQL_SYSVAR(sched_priority_cleaner),
#endif
#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
  MYSQL_SYSVAR(page_hash_locks),
  MYSQL_SYSVAR(doublewrite_batch_size),
#ifdef UNIV_LINUX
  MYSQL_SYSVAR(sched_priority_purge),
  MYSQL_SYSVAR(sched_priority_io),
  MYSQL_SYSVAR(sched_priority_master),
  MYSQL_SYSVAR(priority_purge),
  MYSQL_SYSVAR(priority_io),
  MYSQL_SYSVAR(priority_cleaner),
  MYSQL_SYSVAR(priority_master),
#endif /* UNIV_LINUX */
  MYSQL_SYSVAR(cleaner_max_lru_time),
  MYSQL_SYSVAR(cleaner_max_flush_time),
  MYSQL_SYSVAR(cleaner_flush_chunk_size),
  MYSQL_SYSVAR(cleaner_lru_chunk_size),
  MYSQL_SYSVAR(cleaner_free_list_lwm),
  MYSQL_SYSVAR(cleaner_eviction_factor),
#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */
  MYSQL_SYSVAR(status_output),
  MYSQL_SYSVAR(status_output_locks),
  MYSQL_SYSVAR(cleaner_lsn_age_factor),
  MYSQL_SYSVAR(foreground_preflush),
  MYSQL_SYSVAR(empty_free_list_algorithm),
  MYSQL_SYSVAR(print_all_deadlocks),
  MYSQL_SYSVAR(print_lock_wait_timeout_info),
  MYSQL_SYSVAR(cmp_per_index_enabled),
  MYSQL_SYSVAR(undo_logs),
  MYSQL_SYSVAR(rollback_segments),
  MYSQL_SYSVAR(undo_directory),
  MYSQL_SYSVAR(undo_tablespaces),
  MYSQL_SYSVAR(sync_array_size),
  MYSQL_SYSVAR(compression_failure_threshold_pct),
  MYSQL_SYSVAR(compression_pad_pct_max),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(trx_rseg_n_slots_debug),
  MYSQL_SYSVAR(limit_optimistic_insert_debug),
  MYSQL_SYSVAR(trx_purge_view_update_only_debug),
  MYSQL_SYSVAR(fil_make_page_dirty_debug),
  MYSQL_SYSVAR(saved_page_number_debug),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(corrupt_table_action),
  MYSQL_SYSVAR(tmpdir),
  MYSQL_SYSVAR(compressed_columns_zip_level),
  MYSQL_SYSVAR(compressed_columns_threshold),
  NULL
};

mysql_declare_plugin(innobase)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &innobase_storage_engine,
  innobase_hton_name,
  plugin_author,
  "Percona-XtraDB, Supports transactions, row-level locking, and foreign keys",
  PLUGIN_LICENSE_GPL,
  innobase_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  INNODB_VERSION_SHORT,
  innodb_status_variables_export,/* status variables             */
  innobase_system_variables, /* system variables */
  NULL, /* reserved */
  0,    /* flags */
},
i_s_xtradb_read_view,
i_s_xtradb_internal_hash_tables,
i_s_xtradb_rseg,
i_s_xtradb_zip_dict,
i_s_xtradb_zip_dict_cols,
i_s_innodb_trx,
i_s_innodb_locks,
i_s_innodb_lock_waits,
i_s_innodb_cmp,
i_s_innodb_cmp_reset,
i_s_innodb_cmpmem,
i_s_innodb_cmpmem_reset,
i_s_innodb_cmp_per_index,
i_s_innodb_cmp_per_index_reset,
i_s_innodb_buffer_page,
i_s_innodb_buffer_page_lru,
i_s_innodb_buffer_stats,
i_s_innodb_metrics,
i_s_innodb_ft_default_stopword,
i_s_innodb_ft_deleted,
i_s_innodb_ft_being_deleted,
i_s_innodb_ft_config,
i_s_innodb_ft_index_cache,
i_s_innodb_ft_index_table,
i_s_innodb_sys_tables,
i_s_innodb_sys_tablestats,
i_s_innodb_sys_indexes,
i_s_innodb_sys_columns,
i_s_innodb_sys_fields,
i_s_innodb_sys_foreign,
i_s_innodb_sys_foreign_cols,
i_s_innodb_sys_tablespaces,
i_s_innodb_sys_datafiles,
i_s_innodb_changed_pages
mysql_declare_plugin_end;

/** @brief Initialize the default value of innodb_commit_concurrency.

Once InnoDB is running, the innodb_commit_concurrency must not change
from zero to nonzero. (Bug #42101)

The initial default value is 0, and without this extra initialization,
SET GLOBAL innodb_commit_concurrency=DEFAULT would set the parameter
to 0, even if it was initially set to nonzero at the command line
or configuration file. */
static
void
innobase_commit_concurrency_init_default()
/*======================================*/
{
	MYSQL_SYSVAR_NAME(commit_concurrency).def_val
		= innobase_commit_concurrency;
}

/** @brief Initialize the default and max value of innodb_undo_logs.

Once InnoDB is running, the default value and the max value of
innodb_undo_logs must be equal to the available undo logs,
given by srv_available_undo_logs. */
static
void
innobase_undo_logs_init_default_max()
/*=================================*/
{
	MYSQL_SYSVAR_NAME(undo_logs).max_val
		= MYSQL_SYSVAR_NAME(undo_logs).def_val
		= static_cast<unsigned long>(srv_available_undo_logs);
}

#ifdef UNIV_COMPILE_TEST_FUNCS

struct innobase_convert_name_test_t {
	char*		buf;
	ulint		buflen;
	const char*	id;
	ulint		idlen;
	void*		thd;
	ibool		file_id;

	const char*	expected;
};

void
test_innobase_convert_name()
{
	char	buf[1024];
	ulint	i;

	innobase_convert_name_test_t test_input[] = {
		{buf, sizeof(buf), "abcd", 4, NULL, TRUE, "\"abcd\""},
		{buf, 7, "abcd", 4, NULL, TRUE, "\"abcd\""},
		{buf, 6, "abcd", 4, NULL, TRUE, "\"abcd\""},
		{buf, 5, "abcd", 4, NULL, TRUE, "\"abc\""},
		{buf, 4, "abcd", 4, NULL, TRUE, "\"ab\""},

		{buf, sizeof(buf), "ab@0060cd", 9, NULL, TRUE, "\"ab`cd\""},
		{buf, 9, "ab@0060cd", 9, NULL, TRUE, "\"ab`cd\""},
		{buf, 8, "ab@0060cd", 9, NULL, TRUE, "\"ab`cd\""},
		{buf, 7, "ab@0060cd", 9, NULL, TRUE, "\"ab`cd\""},
		{buf, 6, "ab@0060cd", 9, NULL, TRUE, "\"ab`c\""},
		{buf, 5, "ab@0060cd", 9, NULL, TRUE, "\"ab`\""},
		{buf, 4, "ab@0060cd", 9, NULL, TRUE, "\"ab\""},

		{buf, sizeof(buf), "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\"\"cd\""},
		{buf, 17, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\"\"cd\""},
		{buf, 16, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\"\"c\""},
		{buf, 15, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\"\"\""},
		{buf, 14, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\""},
		{buf, 13, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#ab\""},
		{buf, 12, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#a\""},
		{buf, 11, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50#\""},
		{buf, 10, "ab\"cd", 5, NULL, TRUE,
			"\"#mysql50\""},

		{buf, sizeof(buf), "ab/cd", 5, NULL, TRUE, "\"ab\".\"cd\""},
		{buf, 9, "ab/cd", 5, NULL, TRUE, "\"ab\".\"cd\""},
		{buf, 8, "ab/cd", 5, NULL, TRUE, "\"ab\".\"c\""},
		{buf, 7, "ab/cd", 5, NULL, TRUE, "\"ab\".\"\""},
		{buf, 6, "ab/cd", 5, NULL, TRUE, "\"ab\"."},
		{buf, 5, "ab/cd", 5, NULL, TRUE, "\"ab\"."},
		{buf, 4, "ab/cd", 5, NULL, TRUE, "\"ab\""},
		{buf, 3, "ab/cd", 5, NULL, TRUE, "\"a\""},
		{buf, 2, "ab/cd", 5, NULL, TRUE, "\"\""},
		/* XXX probably "" is a better result in this case
		{buf, 1, "ab/cd", 5, NULL, TRUE, "."},
		*/
		{buf, 0, "ab/cd", 5, NULL, TRUE, ""},
	};

	for (i = 0; i < sizeof(test_input) / sizeof(test_input[0]); i++) {

		char*	end;
		ibool	ok = TRUE;
		size_t	res_len;

		fprintf(stderr, "TESTING %lu, %s, %lu, %s\n",
			test_input[i].buflen,
			test_input[i].id,
			test_input[i].idlen,
			test_input[i].expected);

		end = innobase_convert_name(
			test_input[i].buf,
			test_input[i].buflen,
			test_input[i].id,
			test_input[i].idlen,
			test_input[i].thd,
			test_input[i].file_id);

		res_len = (size_t) (end - test_input[i].buf);

		if (res_len != strlen(test_input[i].expected)) {

			fprintf(stderr, "unexpected len of the result: %u, "
				"expected: %u\n", (unsigned) res_len,
				(unsigned) strlen(test_input[i].expected));
			ok = FALSE;
		}

		if (memcmp(test_input[i].buf,
			   test_input[i].expected,
			   strlen(test_input[i].expected)) != 0
		    || !ok) {

			fprintf(stderr, "unexpected result: %.*s, "
				"expected: %s\n", (int) res_len,
				test_input[i].buf,
				test_input[i].expected);
			ok = FALSE;
		}

		if (ok) {
			fprintf(stderr, "OK: res: %.*s\n\n", (int) res_len,
				buf);
		} else {
			fprintf(stderr, "FAILED\n\n");
			return;
		}
	}
}

#endif /* UNIV_COMPILE_TEST_FUNCS */

/****************************************************************************
 * DS-MRR implementation
 ***************************************************************************/

/**
 * Multi Range Read interface, DS-MRR calls
 */

int
ha_innobase::multi_range_read_init(
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint		mode,
	HANDLER_BUFFER*	buf)
{
	return(ds_mrr.dsmrr_init(this, seq, seq_init_param,
				 n_ranges, mode, buf));
}

int
ha_innobase::multi_range_read_next(
	char**		range_info)
{
	return(ds_mrr.dsmrr_next(range_info));
}

ha_rows
ha_innobase::multi_range_read_info_const(
	uint		keyno,
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost)
{
	/* See comments in ha_myisam::multi_range_read_info_const */
	ds_mrr.init(this, table);
	return(ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param,
				       n_ranges, bufsz, flags, cost));
}

ha_rows
ha_innobase::multi_range_read_info(
	uint		keyno,
	uint		n_ranges,
	uint		keys,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost)
{
	ds_mrr.init(this, table);
	return(ds_mrr.dsmrr_info(keyno, n_ranges, keys, bufsz, flags, cost));
}


/**
 * Index Condition Pushdown interface implementation
 */

/*************************************************************//**
InnoDB index push-down condition check
@return ICP_NO_MATCH, ICP_MATCH, or ICP_OUT_OF_RANGE */
UNIV_INTERN
enum icp_result
innobase_index_cond(
/*================*/
	void*	file)	/*!< in/out: pointer to ha_innobase */
{
	DBUG_ENTER("innobase_index_cond");

	ha_innobase*	h = reinterpret_cast<class ha_innobase*>(file);

	DBUG_ASSERT(h->pushed_idx_cond);
	DBUG_ASSERT(h->pushed_idx_cond_keyno != MAX_KEY);

	if (h->end_range && h->compare_key_icp(h->end_range) > 0) {

		/* caller should return HA_ERR_END_OF_FILE already */
		DBUG_RETURN(ICP_OUT_OF_RANGE);
	}

	DBUG_RETURN(h->pushed_idx_cond->val_int() ? ICP_MATCH : ICP_NO_MATCH);
}

/** Attempt to push down an index condition.
* @param[in] keyno	MySQL key number
* @param[in] idx_cond	Index condition to be checked
* @return Part of idx_cond which the handler will not evaluate
*/
UNIV_INTERN
class Item*
ha_innobase::idx_cond_push(
	uint		keyno,
	class Item*	idx_cond)
{
	DBUG_ENTER("ha_innobase::idx_cond_push");
	DBUG_ASSERT(keyno != MAX_KEY);
	DBUG_ASSERT(idx_cond != NULL);

	pushed_idx_cond = idx_cond;
	pushed_idx_cond_keyno = keyno;
	in_range_check_pushed_down = TRUE;
	/* We will evaluate the condition entirely */
	DBUG_RETURN(NULL);
}

/******************************************************************//**
Use this when the args are passed to the format string from
errmsg-utf8.txt directly as is.

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_warning_level level,
	uint code, const char *format, ...);
*/
UNIV_INTERN
void
ib_senderrf(
/*========*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	...)				/*!< Args */
{
	char*		str;
	va_list         args;
	const char*	format = innobase_get_err_msg(code);

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);

	/* The error code must exist in the errmsg-utf8.txt file. */
	ut_a(format != 0);

	va_start(args, code);

#ifdef __WIN__
	int		size = _vscprintf(format, args) + 1;
	str = static_cast<char*>(malloc(size));
	str[size - 1] = 0x0;
	vsnprintf(str, size, format, args);
#elif HAVE_VASPRINTF
	int	ret;
	ret = vasprintf(&str, format, args);
	ut_a(ret != -1);
#else
	/* Use a fixed length string. */
	str = static_cast<char*>(malloc(BUFSIZ));
	my_vsnprintf(str, BUFSIZ, format, args);
#endif /* __WIN__ */

	Sql_condition::enum_warning_level	l;

	l = Sql_condition::WARN_LEVEL_NOTE;

	switch(level) {
	case IB_LOG_LEVEL_INFO:
		break;
	case IB_LOG_LEVEL_WARN:
		l = Sql_condition::WARN_LEVEL_WARN;
		break;
	case IB_LOG_LEVEL_ERROR:
		/* We can't use push_warning_printf(), it is a hard error. */
		my_printf_error(code, "%s", MYF(0), str);
		break;
	case IB_LOG_LEVEL_FATAL:
		l = Sql_condition::WARN_LEVEL_END;
		break;
	}

	if (level != IB_LOG_LEVEL_ERROR) {
		push_warning_printf(thd, l, code, "InnoDB: %s", str);
	}

	va_end(args);
	free(str);

	if (level == IB_LOG_LEVEL_FATAL) {
		ut_error;
	}
}

/******************************************************************//**
Use this when the args are first converted to a formatted string and then
passed to the format string from errmsg-utf8.txt. The error message format
must be: "Some string ... %s".

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_warning_level level,
	uint code, const char *format, ...);
*/
UNIV_INTERN
void
ib_errf(
/*====*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
{
	char*		str;
	va_list         args;

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);
	ut_a(format != 0);

	va_start(args, format);

#ifdef __WIN__
	int		size = _vscprintf(format, args) + 1;
	str = static_cast<char*>(malloc(size));
	str[size - 1] = 0x0;
	vsnprintf(str, size, format, args);
#elif HAVE_VASPRINTF
	int	ret;
	ret = vasprintf(&str, format, args);
	ut_a(ret != -1);
#else
	/* Use a fixed length string. */
	str = static_cast<char*>(malloc(BUFSIZ));
	my_vsnprintf(str, BUFSIZ, format, args);
#endif /* __WIN__ */

	ib_senderrf(thd, level, code, str);

	va_end(args);
	free(str);
}

/******************************************************************//**
Write a message to the MySQL log, prefixed with "InnoDB: " */
UNIV_INTERN
void
ib_logf(
/*====*/
	ib_log_level_t	level,		/*!< in: warning level */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
{
	char*		str;
	va_list         args;

	va_start(args, format);

#ifdef __WIN__
	int		size = _vscprintf(format, args) + 1;
	str = static_cast<char*>(malloc(size));
	str[size - 1] = 0x0;
	vsnprintf(str, size, format, args);
#elif HAVE_VASPRINTF
	int	ret;
	ret = vasprintf(&str, format, args);
	ut_a(ret != -1);
#else
	/* Use a fixed length string. */
	str = static_cast<char*>(malloc(BUFSIZ));
	my_vsnprintf(str, BUFSIZ, format, args);
#endif /* __WIN__ */

	switch(level) {
	case IB_LOG_LEVEL_INFO:
		sql_print_information("InnoDB: %s", str);
		break;
	case IB_LOG_LEVEL_WARN:
		sql_print_warning("InnoDB: %s", str);
		break;
	case IB_LOG_LEVEL_ERROR:
		sql_print_error("InnoDB: %s", str);
		break;
	case IB_LOG_LEVEL_FATAL:
		sql_print_error("InnoDB: %s", str);
		break;
	}

	va_end(args);
	free(str);

	if (level == IB_LOG_LEVEL_FATAL) {
		ut_error;
	}
}

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset.
@return result string length, as returned by strconvert() */
uint
innobase_convert_to_filename_charset(
/*=================================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len)	/* in: length of 'to', in bytes */
{
	uint		errors;
	CHARSET_INFO*	cs_to = &my_charset_filename;
	CHARSET_INFO*	cs_from = system_charset_info;

	return(strconvert(
		cs_from, from, cs_to, to, static_cast<uint>(len), &errors));
}

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset.
@return result string length, as returned by strconvert() */
uint
innobase_convert_to_system_charset(
/*===============================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len,	/* in: length of 'to', in bytes */
	uint*		errors)	/* out: error return */
{
	CHARSET_INFO*	cs1 = &my_charset_filename;
	CHARSET_INFO*	cs2 = system_charset_info;

	return(strconvert(
		cs1, from, cs2, to, static_cast<uint>(len), errors));
}

/**********************************************************************
Issue a warning that the row is too big. */
void
ib_warn_row_too_big(const dict_table_t*	table)
{
	/* If prefix is true then a 768-byte prefix is stored
	locally for BLOB fields. Refer to dict_table_get_format() */
	const bool prefix = (dict_tf_get_format(table->flags)
			     == UNIV_FORMAT_A);

	const ulint	free_space = page_get_free_space_of_empty(
		table->flags & DICT_TF_COMPACT) / 2;

	THD*	thd = current_thd;

	push_warning_printf(
		thd, Sql_condition::WARN_LEVEL_WARN, HA_ERR_TO_BIG_ROW,
		"Row size too large (> %lu). Changing some columns to TEXT"
		" or BLOB %smay help. In current row format, BLOB prefix of"
		" %d bytes is stored inline.", free_space
		, prefix ? "or using ROW_FORMAT=DYNAMIC or"
		" ROW_FORMAT=COMPRESSED ": ""
		, prefix ? DICT_MAX_FIXED_COL_LEN : 0);
}

/*****************************************************************//**
Checks if the file name is reserved in InnoDB. Currently
redo log files(ib_logfile*) is reserved.
@return true if the name is reserved */
static
bool
innobase_check_reserved_file_name(
/*===================*/
	handlerton*     hton,		/*!< in: handlerton of Innodb */
	const char*	name)		/*!< in: Name of the database */
{
	CHARSET_INFO *ci= system_charset_info;
	size_t logname_size = strlen(ib_logfile_basename);

	/* Name is smaller than reserved name */
	if (strlen(name) < logname_size) {
		return (false);
	}
	/* Do case insensitive comparison for name. */
	for (uint i=0; i < logname_size; i++) {
		if (my_tolower(ci, name[i]) != ib_logfile_basename[i]){
			return (false);
		}
	}
	return (true);
}
