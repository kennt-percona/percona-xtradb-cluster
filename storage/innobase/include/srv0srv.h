/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2008, 2009, Google Inc.
Copyright (c) 2009, Percona Inc.

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

/**************************************************//**
@file include/srv0srv.h
The server main program

Created 10/10/1995 Heikki Tuuri
*******************************************************/

#ifndef srv0srv_h
#define srv0srv_h

#include "univ.i"
#ifndef UNIV_HOTBACKUP
#include "log0log.h"
#include "sync0sync.h"
#include "os0sync.h"
#include "que0types.h"
#include "trx0types.h"
#include "srv0conc.h"
#include "buf0checksum.h"
#include "ut0counter.h"

#include <sql_class.h>

/* Global counters used inside InnoDB. */
struct srv_stats_t {
	typedef ib_counter_t<lsn_t, 1, single_indexer_t> lsn_ctr_1_t;
	typedef ib_counter_t<ulint, 1, single_indexer_t> ulint_ctr_1_t;
	typedef ib_counter_t<lint, 1, single_indexer_t> lint_ctr_1_t;
	typedef ib_counter_t<ulint, 64> ulint_ctr_64_t;
	typedef ib_counter_t<ib_int64_t, 1, single_indexer_t> ib_int64_ctr_1_t;

	/** Count the amount of data written in total (in bytes) */
	ulint_ctr_1_t		data_written;

	/** Number of the log write requests done */
	ulint_ctr_1_t		log_write_requests;

	/** Number of physical writes to the log performed */
	ulint_ctr_1_t		log_writes;

	/** Amount of data written to the log files in bytes */
	lsn_ctr_1_t		os_log_written;

	/** Number of writes being done to the log files */
	lint_ctr_1_t		os_log_pending_writes;

	/** We increase this counter, when we don't have enough
	space in the log buffer and have to flush it */
	ulint_ctr_1_t		log_waits;

	/** Count the number of times the doublewrite buffer was flushed */
	ulint_ctr_1_t		dblwr_writes;

	/** Store the number of pages that have been flushed to the
	doublewrite buffer */
	ulint_ctr_1_t		dblwr_pages_written;

	/** Store the number of write requests issued */
	ulint_ctr_1_t		buf_pool_write_requests;

	/** Store the number of times when we had to wait for a free page
	in the buffer pool. It happens when the buffer pool is full and we
	need to make a flush, in order to be able to read or create a page. */
	ulint_ctr_1_t		buf_pool_wait_free;

	/** Count the number of pages that were written from buffer
	pool to the disk */
	ulint_ctr_1_t		buf_pool_flushed;

	/** Number of buffer pool reads that led to the reading of
	a disk page */
	ulint_ctr_1_t		buf_pool_reads;

	/** Number of data read in total (in bytes) */
	ulint_ctr_1_t		data_read;

	/** Wait time of database locks */
	ib_int64_ctr_1_t	n_lock_wait_time;

	/** Number of database lock waits */
	ulint_ctr_1_t		n_lock_wait_count;

	/** Number of threads currently waiting on database locks */
	lint_ctr_1_t		n_lock_wait_current_count;

	/** Number of rows read. */
	ulint_ctr_64_t		n_rows_read;

	/** Number of rows updated */
	ulint_ctr_64_t		n_rows_updated;

	/** Number of rows deleted */
	ulint_ctr_64_t		n_rows_deleted;

	/** Number of rows inserted */
	ulint_ctr_64_t		n_rows_inserted;

	ulint_ctr_1_t		lock_deadlock_count;

	ulint_ctr_1_t		n_lock_max_wait_time;

	/** Number of buffered aio requests submitted */
	ulint_ctr_64_t		n_aio_submitted;
};

extern const char*	srv_main_thread_op_info;

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
extern const char	srv_mysql50_table_name_prefix[10];

/* The monitor thread waits on this event. */
extern os_event_t	srv_monitor_event;

/* The error monitor thread waits on this event. */
extern os_event_t	srv_error_event;

/** The buffer pool dump/load thread waits on this event. */
extern os_event_t	srv_buf_dump_event;

/** The buffer pool dump/load file name */
#define SRV_BUF_DUMP_FILENAME_DEFAULT	"ib_buffer_pool"
extern char*		srv_buf_dump_filename;

/** Boolean config knobs that tell InnoDB to dump the buffer pool at shutdown
and/or load it during startup. */
extern char		srv_buffer_pool_dump_at_shutdown;
extern char		srv_buffer_pool_load_at_startup;

/* Whether to disable file system cache if it is defined */
extern char		srv_disable_sort_file_cache;

/* This event is set on checkpoint completion to wake the redo log parser
thread */
extern os_event_t	srv_checkpoint_completed_event;

/* This event is set on the online redo log following thread after a successful
log tracking iteration */
extern os_event_t	srv_redo_log_tracked_event;

/** Whether the redo log tracker thread has been started. Does not take into
account whether the tracking is currently enabled (see srv_track_changed_pages
for that) */
extern bool		srv_redo_log_thread_started;

/* If the last data file is auto-extended, we add this many pages to it
at a time */
#define SRV_AUTO_EXTEND_INCREMENT	\
	(srv_auto_extend_increment * ((1024 * 1024) / UNIV_PAGE_SIZE))

/* Mutex for locking srv_monitor_file. Not created if srv_read_only_mode */
extern ib_mutex_t	srv_monitor_file_mutex;

/* prototypes for new functions added to ha_innodb.cc */
ibool	innobase_get_slow_log();

/* Temporary file for innodb monitor output */
extern FILE*	srv_monitor_file;
/* Mutex for locking srv_dict_tmpfile. Only created if !srv_read_only_mode.
This mutex has a very high rank; threads reserving it should not
be holding any InnoDB latches. */
extern ib_mutex_t	srv_dict_tmpfile_mutex;
/* Temporary file for output from the data dictionary */
extern FILE*	srv_dict_tmpfile;
/* Mutex for locking srv_misc_tmpfile. Only created if !srv_read_only_mode.
This mutex has a very low rank; threads reserving it should not
acquire any further latches or sleep before releasing this one. */
extern ib_mutex_t	srv_misc_tmpfile_mutex;
/* Temporary file for miscellanous diagnostic output */
extern FILE*	srv_misc_tmpfile;

/* Server parameters which are read from the initfile */

extern char*	srv_data_home;

#ifdef UNIV_LOG_ARCHIVE
extern char*	srv_arch_dir;
#endif /* UNIV_LOG_ARCHIVE */

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
extern my_bool	srv_read_only_mode;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_TRX_UNDO. */
extern my_bool	high_level_read_only;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
extern my_bool	srv_file_per_table;
/** Sleep delay for threads waiting to enter InnoDB. In micro-seconds. */
extern	ulong	srv_thread_sleep_delay;
#if defined(HAVE_ATOMIC_BUILTINS)
/** Maximum sleep delay (in micro-seconds), value of 0 disables it.*/
extern	ulong	srv_adaptive_max_sleep_delay;
#endif /* HAVE_ATOMIC_BUILTINS */

/** The file format to use on new *.ibd files. */
extern ulint	srv_file_format;
/** Whether to check file format during startup.  A value of
UNIV_FORMAT_MAX + 1 means no checking ie. FALSE.  The default is to
set it to the highest format we support. */
extern ulint	srv_max_file_format_at_startup;
/** Place locks to records only i.e. do not use next-key locking except
on duplicate key checking and foreign key checking */
extern ibool	srv_locks_unsafe_for_binlog;

/** Sort buffer size in index creation */
extern ulong	srv_sort_buf_size;
/** Maximum modification log file size for online index creation */
extern unsigned long long	srv_online_max_size;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio we build below with threads.
Currently we support native aio on windows and linux */
extern my_bool	srv_use_native_aio;
extern my_bool	srv_numa_interleave;
#ifdef __WIN__
extern ibool	srv_use_native_conditions;
#endif /* __WIN__ */
#endif /* !UNIV_HOTBACKUP */

/** Server undo tablespaces directory, can be absolute path. */
extern char*	srv_undo_dir;

/** Number of undo tablespaces to use. */
extern ulong	srv_undo_tablespaces;

/** The number of UNDO tablespaces that are open and ready to use. */
extern ulint	srv_undo_tablespaces_open;

/* The number of undo segments to use */
extern ulong	srv_undo_logs;

extern ulint	srv_n_data_files;
extern char**	srv_data_file_names;
extern ulint*	srv_data_file_sizes;
extern ulint*	srv_data_file_is_raw_partition;


/** Whether the redo log tracking is currently enabled. Note that it is
possible for the log tracker thread to be running and the tracking to be
disabled */
extern my_bool		srv_track_changed_pages;
extern ulonglong	srv_max_bitmap_file_size;

extern
ulonglong       srv_max_changed_pages;

extern ibool	srv_auto_extend_last_data_file;
extern ulint	srv_last_file_size_max;
extern char*	srv_log_group_home_dir;
#ifndef UNIV_HOTBACKUP
extern ulong	srv_auto_extend_increment;

extern ibool	srv_created_new_raw;

/** Maximum number of srv_n_log_files, or innodb_log_files_in_group */
#define SRV_N_LOG_FILES_MAX 100
extern ulong	srv_n_log_files;
extern ib_uint64_t	srv_log_file_size;
extern ib_uint64_t	srv_log_file_size_requested;
extern ulint	srv_log_buffer_size;
extern uint	srv_flush_log_at_timeout;
extern char	srv_use_global_flush_log_at_trx_commit;
extern char	srv_adaptive_flushing;

#ifdef WITH_INNODB_DISALLOW_WRITES
/* When this event is reset we do not allow any file writes to take place. */
extern os_event_t	srv_allow_writes_event;
#endif /* WITH_INNODB_DISALLOW_WRITES */
/* If this flag is TRUE, then we will load the indexes' (and tables') metadata
even if they are marked as "corrupted". Mostly it is for DBA to process
corrupted index and table */
extern my_bool	srv_load_corrupted;

extern ulint    srv_show_locks_held;
extern ulint    srv_show_verbose_locks;

/* The sort order table of the MySQL latin1_swedish_ci character set
collation */
extern const byte*	srv_latin1_ordering;
#ifndef UNIV_HOTBACKUP
extern my_bool	srv_use_sys_malloc;
#else
extern ibool	srv_use_sys_malloc;
#endif /* UNIV_HOTBACKUP */
extern ulint	srv_buf_pool_size;	/*!< requested size in bytes */
extern ulint    srv_buf_pool_instances; /*!< requested number of buffer pool instances */
extern ulong	srv_n_page_hash_locks;	/*!< number of locks to
					protect buf_pool->page_hash */
extern ulong	srv_LRU_scan_depth;	/*!< Scan depth for LRU
					flush batch */
extern ulong	srv_flush_neighbors;	/*!< whether or not to flush
					neighbors of a block */
extern ulint	srv_buf_pool_old_size;	/*!< previously requested size */
extern ulint	srv_buf_pool_curr_size;	/*!< current size in bytes */
extern ulint	srv_mem_pool_size;
extern ulint	srv_lock_table_size;

extern ulint	srv_foreground_preflush;/*!< Query thread preflush algorithm */

extern ulint	srv_cleaner_max_lru_time;/*!< the maximum time limit for a
					single LRU tail flush iteration by the
					page cleaner thread */

extern ulint	srv_cleaner_max_flush_time;/*!< the maximum time limit for a
					single flush list flush iteration by
					the page cleaner thread */

extern ulint	srv_cleaner_flush_chunk_size;
					/*!< page cleaner flush list flush
					batches are further divided into this
					chunk size  */

extern ulint	srv_cleaner_lru_chunk_size;
					/*!< page cleaner LRU list flush
					batches are further divided into this
					chunk size  */

extern ulint	srv_cleaner_free_list_lwm;/*!< if free list length is lower
					than this percentage of
					srv_LRU_scan_depth, page cleaner LRU
					flushes will issue flush batches to the
					same instance in a row  */

extern my_bool	srv_cleaner_eviction_factor;
					/*!< if TRUE, page cleaner heuristics
					use evicted instead of flushed page
					counts for its heuristics  */

extern ulong	srv_cleaner_lsn_age_factor;
					/*!< page cleaner LSN age factor
					formula option */

extern ulong	srv_empty_free_list_algorithm;
					/*!< Empty free list for a query thread
					handling algorithm option */

extern ulint	srv_n_file_io_threads;
extern my_bool	srv_random_read_ahead;
extern ulong	srv_read_ahead_threshold;
extern ulint	srv_n_read_io_threads;
extern ulint	srv_n_write_io_threads;

/* Number of IO operations per second the server can do */
extern ulong    srv_io_capacity;

/* We use this dummy default value at startup for max_io_capacity.
The real value is set based on the value of io_capacity. */
#define SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT	(~0UL)
#define SRV_MAX_IO_CAPACITY_LIMIT		(~0UL)
extern ulong    srv_max_io_capacity;
/* Returns the number of IO operations that is X percent of the
capacity. PCT_IO(5) -> returns the number of IO operations that
is 5% of the max where max is srv_io_capacity.  */
#define PCT_IO(p) ((ulong) (srv_io_capacity * ((double) (p) / 100.0)))

/* The "innodb_stats_method" setting, decides how InnoDB is going
to treat NULL value when collecting statistics. It is not defined
as enum type because the configure option takes unsigned integer type. */
extern ulong	srv_innodb_stats_method;

#ifdef UNIV_LOG_ARCHIVE
extern ibool		srv_log_archive_on;
#endif /* UNIV_LOG_ARCHIVE */

extern char*	srv_file_flush_method_str;
extern ulint	srv_unix_file_flush_method;
extern ulint	srv_win_file_flush_method;

extern ulint	srv_max_n_open_files;

extern ulong	srv_max_dirty_pages_pct;
extern ulong	srv_max_dirty_pages_pct_lwm;

extern ulong	srv_adaptive_flushing_lwm;
extern ulong	srv_flushing_avg_loops;

extern ulong	srv_force_recovery;
#ifndef DBUG_OFF
extern ulong	srv_force_recovery_crash;
#endif /* !DBUG_OFF */

extern ulint	srv_fast_shutdown;	/*!< If this is 1, do not do a
					purge and index buffer merge.
					If this 2, do not even flush the
					buffer pool to data files at the
					shutdown: we effectively 'crash'
					InnoDB (but lose no committed
					transactions). */
extern ibool	srv_innodb_status;

extern unsigned long long	srv_stats_transient_sample_pages;
extern my_bool			srv_stats_persistent;
extern unsigned long long	srv_stats_persistent_sample_pages;
extern my_bool			srv_stats_auto_recalc;
extern my_bool			srv_stats_include_delete_marked;

extern ibool	srv_use_doublewrite_buf;
extern ulong	srv_doublewrite_batch_size;
extern ibool	srv_use_atomic_writes;
#ifdef HAVE_POSIX_FALLOCATE
extern ibool	srv_use_posix_fallocate;
#endif

extern ulong	srv_log_arch_expire_sec;

extern ulong	srv_max_buf_pool_modified_pct;
extern ulong	srv_max_purge_lag;
extern ulong	srv_max_purge_lag_delay;

extern ulong	srv_replication_delay;

extern ulint	srv_pass_corrupt_table;

extern ulint	srv_log_checksum_algorithm;

/* Helper macro to support srv_pass_corrupt_table checks. If 'cond' is FALSE,
execute 'code' if srv_pass_corrupt_table is non-zero, or trigger a fatal error
otherwise. The break statement in 'code' will obviously not work as
expected. */

#define SRV_CORRUPT_TABLE_CHECK(cond,code)		\
	do {						\
		if (UNIV_UNLIKELY(!(cond))) {		\
			if (srv_pass_corrupt_table) {	\
				code			\
			} else {			\
				ut_error;		\
			}				\
		}					\
	} while(0)

/*-------------------------------------------*/

extern ulint	srv_read_views_memory;
extern ulint	srv_descriptors_memory;

extern my_bool	srv_print_innodb_monitor;
extern my_bool	srv_print_innodb_lock_monitor;
extern ibool	srv_print_innodb_tablespace_monitor;
extern ibool	srv_print_verbose_log;
#define DEPRECATED_MSG_INNODB_TABLE_MONITOR \
	"Using innodb_table_monitor is deprecated and it may be removed " \
	"in future releases. Please use the InnoDB INFORMATION_SCHEMA " \
	"tables instead, see " REFMAN "innodb-i_s-tables.html"
extern ibool	srv_print_innodb_table_monitor;

extern ibool	srv_monitor_active;
extern ibool	srv_error_monitor_active;

/* TRUE during the lifetime of the buffer pool dump/load thread */
extern ibool	srv_buf_dump_thread_active;

/* TRUE during the lifetime of the stats thread */
extern ibool	srv_dict_stats_thread_active;

extern ulong	srv_n_spin_wait_rounds;
extern ulong	srv_n_free_tickets_to_enter;
extern ulong	srv_thread_sleep_delay;
extern ulong	srv_spin_wait_delay;
extern ibool	srv_priority_boost;

extern ulint	srv_truncated_status_writes;
extern ulint	srv_available_undo_logs;

extern ulint	srv_column_compressed;
extern ulint	srv_column_decompressed;

extern	ulint	srv_mem_pool_size;
extern	ulint	srv_lock_table_size;

#ifdef UNIV_DEBUG
extern	ibool	srv_print_thread_releases;
extern	ibool	srv_print_lock_waits;
extern	ibool	srv_print_buf_io;
extern	ibool	srv_print_log_io;
extern	ibool	srv_print_latch_waits;
#else /* UNIV_DEBUG */
# define srv_print_thread_releases	FALSE
# define srv_print_lock_waits		FALSE
# define srv_print_buf_io		FALSE
# define srv_print_log_io		FALSE
# define srv_print_latch_waits		FALSE
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
extern my_bool	srv_ibuf_disable_background_merge;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

#ifdef UNIV_DEBUG
extern my_bool	srv_purge_view_update_only_debug;
#endif /* UNIV_DEBUG */

extern ulint	srv_fatal_semaphore_wait_threshold;
#define SRV_SEMAPHORE_WAIT_EXTENSION	7200
extern ulint	srv_dml_needed_delay;
extern lint	srv_kill_idle_transaction;

#ifndef HAVE_ATOMIC_BUILTINS
/** Mutex protecting some server global variables. */
extern ib_mutex_t	server_mutex;
#endif /* !HAVE_ATOMIC_BUILTINS */

#define SRV_MAX_N_IO_THREADS	130

#define SRV_MAX_N_PURGE_THREADS 32

/* Array of English strings describing the current state of an
i/o handler thread */
extern const char* srv_io_thread_op_info[];
extern const char* srv_io_thread_function[];

/* The tid of the cleaner thread */
extern os_tid_t	srv_cleaner_tid;

/* The tid of the LRU manager thread */
extern os_tid_t srv_lru_manager_tid;

/* The tids of the purge threads */
extern os_tid_t srv_purge_tids[];

/* The tids of the I/O threads */
extern os_tid_t	srv_io_tids[];

/* The tid of the master thread */
extern os_tid_t	srv_master_tid;

/* The relative scheduling priority of the cleaner and LRU manager threads */
extern ulint	srv_sched_priority_cleaner;

/* The relative scheduling priority of the purge threads */
extern ulint	srv_sched_priority_purge;

/* The relative scheduling priority of the I/O threads */
extern ulint	srv_sched_priority_io;

/* The relative scheduling priority of the master thread */
extern ulint	srv_sched_priority_master;

/* The relative priority of the purge coordinator and worker threads.  */
extern my_bool srv_purge_thread_priority;

/* The relative priority of the I/O threads.  */
extern my_bool srv_io_thread_priority;

/* The relative priority of the cleaner thread.  */
extern my_bool srv_cleaner_thread_priority;

/* The relative priority of the master thread.  */
extern my_bool srv_master_thread_priority;

/* the number of purge threads to use from the worker pool (currently 0 or 1) */
extern ulong srv_n_purge_threads;

/* the number of pages to purge in one batch */
extern ulong srv_purge_batch_size;

/* the number of sync wait arrays */
extern ulong srv_sync_array_size;

/* print all user-level transactions deadlocks to mysqld stderr */
extern my_bool srv_print_all_deadlocks;

/* print lock wait timeout info to mysqld stderr */
extern my_bool srv_print_lock_wait_timeout_info;

extern my_bool	srv_cmp_per_index_enabled;

/** Number of times secondary index lookup triggered cluster lookup */
extern ulint	srv_sec_rec_cluster_reads;
/** Number of times prefix optimization avoided triggering cluster lookup */
extern ulint	srv_sec_rec_cluster_reads_avoided;

/** Status variables to be passed to MySQL */
extern struct export_var_t export_vars;

/** Global counters */
extern srv_stats_t	srv_stats;

/** When TRUE, fake change transcations take S rather than X row locks.
When FALSE, row locks are not taken at all. */
extern my_bool srv_fake_changes_locks;


# ifdef UNIV_PFS_THREAD
/* Keys to register InnoDB threads with performance schema */
extern mysql_pfs_key_t	buf_page_cleaner_thread_key;
extern mysql_pfs_key_t	buf_lru_manager_thread_key;
extern mysql_pfs_key_t	trx_rollback_clean_thread_key;
extern mysql_pfs_key_t	io_handler_thread_key;
extern mysql_pfs_key_t	srv_lock_timeout_thread_key;
extern mysql_pfs_key_t	srv_error_monitor_thread_key;
extern mysql_pfs_key_t	srv_monitor_thread_key;
extern mysql_pfs_key_t	srv_master_thread_key;
extern mysql_pfs_key_t	srv_purge_thread_key;
extern mysql_pfs_key_t	recv_writer_thread_key;
extern mysql_pfs_key_t	srv_log_tracking_thread_key;

/* This macro register the current thread and its key with performance
schema */
#  define pfs_register_thread(key)			\
do {								\
	struct PSI_thread* psi = PSI_THREAD_CALL(new_thread)(key, NULL, 0);\
	PSI_THREAD_CALL(set_thread)(psi);			\
} while (0)

/* This macro delist the current thread from performance schema */
#  define pfs_delete_thread()				\
do {								\
	PSI_THREAD_CALL(delete_current_thread)();		\
} while (0)
# endif /* UNIV_PFS_THREAD */

#endif /* !UNIV_HOTBACKUP */

/** Types of raw partitions in innodb_data_file_path */
enum {
	SRV_NOT_RAW = 0,	/*!< Not a raw partition */
	SRV_NEW_RAW,		/*!< A 'newraw' partition, only to be
				initialized */
	SRV_OLD_RAW		/*!< An initialized raw partition */
};

/** Alternatives for the file flush option in Unix; see the InnoDB manual
about what these mean */
enum {
	SRV_UNIX_FSYNC = 1,	/*!< fsync, the default */
	SRV_UNIX_O_DSYNC,	/*!< open log files in O_SYNC mode */
	SRV_UNIX_LITTLESYNC,	/*!< do not call os_file_flush()
				when writing data files, but do flush
				after writing to log files */
	SRV_UNIX_NOSYNC,	/*!< do not flush after writing */
	SRV_UNIX_O_DIRECT,	/*!< invoke os_file_set_nocache() on
				data files. This implies using
				non-buffered IO but still using fsync,
				the reason for which is that some FS
				do not flush meta-data when
				unbuffered IO happens */
	SRV_UNIX_O_DIRECT_NO_FSYNC,
				/*!< do not use fsync() when using
				direct IO i.e.: it can be set to avoid
				the fsync() call that we make when
				using SRV_UNIX_O_DIRECT. However, in
				this case user/DBA should be sure about
				the integrity of the meta-data */
	SRV_UNIX_ALL_O_DIRECT   /*!< similar to O_DIRECT, invokes
				os_file_set_nocache() on data and log files.
				This implies using non-buffered IO but still
				using fsync for data but not log files. */
};

/** Alternatives for file i/o in Windows */
enum {
	SRV_WIN_IO_NORMAL = 1,	/*!< buffered I/O */
	SRV_WIN_IO_UNBUFFERED	/*!< unbuffered I/O; this is the default */
};

/** Alternatives for srv_force_recovery. Non-zero values are intended
to help the user get a damaged database up so that he can dump intact
tables and rows with SELECT INTO OUTFILE. The database must not otherwise
be used with these options! A bigger number below means that all precautions
of lower numbers are included. */
enum {
	SRV_FORCE_IGNORE_CORRUPT = 1,	/*!< let the server run even if it
					detects a corrupt page */
	SRV_FORCE_NO_BACKGROUND	= 2,	/*!< prevent the main thread from
					running: if a crash would occur
					in purge, this prevents it */
	SRV_FORCE_NO_TRX_UNDO = 3,	/*!< do not run trx rollback after
					recovery */
	SRV_FORCE_NO_IBUF_MERGE = 4,	/*!< prevent also ibuf operations:
					if they would cause a crash, better
					not do them */
	SRV_FORCE_NO_UNDO_LOG_SCAN = 5,	/*!< do not look at undo logs when
					starting the database: InnoDB will
					treat even incomplete transactions
					as committed */
	SRV_FORCE_NO_LOG_REDO = 6	/*!< do not do the log roll-forward
					in connection with recovery */
};

/* Alternatives for srv_innodb_stats_method, which could be changed by
setting innodb_stats_method */
enum srv_stats_method_name_enum {
	SRV_STATS_NULLS_EQUAL,		/* All NULL values are treated as
					equal. This is the default setting
					for innodb_stats_method */
	SRV_STATS_NULLS_UNEQUAL,	/* All NULL values are treated as
					NOT equal. */
	SRV_STATS_NULLS_IGNORED		/* NULL values are ignored */
};

typedef enum srv_stats_method_name_enum		srv_stats_method_name_t;

#ifndef UNIV_HOTBACKUP
/** Types of threads existing in the system. */
enum srv_thread_type {
	SRV_NONE,			/*!< None */
	SRV_WORKER,			/*!< threads serving parallelized
					queries and queries released from
					lock wait */
	SRV_PURGE,			/*!< Purge coordinator thread */
	SRV_MASTER			/*!< the master thread, (whose type
					number must be biggest) */
};

/*********************************************************************//**
Boots Innobase server. */
UNIV_INTERN
void
srv_boot(void);
/*==========*/
/*********************************************************************//**
Initializes the server. */
UNIV_INTERN
void
srv_init(void);
/*==========*/
/*********************************************************************//**
Frees the data structures created in srv_init(). */
UNIV_INTERN
void
srv_free(void);
/*==========*/
/*********************************************************************//**
Initializes the synchronization primitives, memory system, and the thread
local storage. */
UNIV_INTERN
void
srv_general_init(void);
/*==================*/
/*********************************************************************//**
Sets the info describing an i/o thread current state. */
UNIV_INTERN
void
srv_set_io_thread_op_info(
/*======================*/
	ulint		i,	/*!< in: the 'segment' of the i/o thread */
	const char*	str);	/*!< in: constant char string describing the
				state */
/*********************************************************************//**
Resets the info describing an i/o thread current state. */
UNIV_INTERN
void
srv_reset_io_thread_op_info();
/*=========================*/
/*******************************************************************//**
Tells the purge thread that there has been activity in the database
and wakes up the purge thread if it is suspended (not sleeping).  Note
that there is a small chance that the purge thread stays suspended
(we do not protect our operation with the srv_sys_t:mutex, for
performance reasons). */
UNIV_INTERN
void
srv_wake_purge_thread_if_not_active(void);
/*=====================================*/
/*******************************************************************//**
Tells the Innobase server that there has been activity in the database
and wakes up the master thread if it is suspended (not sleeping). Used
in the MySQL interface. Note that there is a small chance that the master
thread stays suspended (we do not protect our operation with the kernel
mutex, for performace reasons). */
UNIV_INTERN
void
srv_active_wake_master_thread(void);
/*===============================*/
/*******************************************************************//**
Wakes up the master thread if it is suspended or being suspended. */
UNIV_INTERN
void
srv_wake_master_thread(void);
/*========================*/
/******************************************************************//**
A thread which follows the redo log and outputs the changed page bitmap.
@return a dummy value */
extern "C"
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_redo_log_follow_thread)(
/*=======================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */
/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
UNIV_INTERN
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for the
				lock_sys_t::mutex */
	ulint*	trx_start,	/*!< out: file position of the start of
				the list of active transactions */
	ulint*	trx_end);	/*!< out: file position of the end of
				the list of active transactions */

/******************************************************************//**
Function to pass InnoDB status variables to MySQL */
UNIV_INTERN
void
srv_export_innodb_status(void);
/*==========================*/
/*************************************************************//**
Removes old archived transaction log files.
Both parameters couldn't be provided at the same time.
@return DB_SUCCESS on success, otherwise DB_ERROR */
UNIV_INTERN
dberr_t
purge_archived_logs(
	time_t	before_date,		/*!< in: all files modified
					before timestamp should be removed */
	lsn_t	before_lsn);		/*!< in: files with this lsn in name
					and earler should be removed */
/*==========================*/
/*******************************************************************//**
Get current server activity count. We don't hold srv_sys::mutex while
reading this value as it is only used in heuristics.
@return activity count. */
UNIV_INTERN
ulint
srv_get_activity_count(void);
/*========================*/
/*******************************************************************//**
Check if there has been any activity. Considers background change buffer
merge as regular server activity unless a non-default
old_ibuf_merge_activity_count value is passed, in which case the merge will be
treated as keeping server idle.
@return FALSE if no change in activity counter. */
UNIV_INTERN
ibool
srv_check_activity(
/*===============*/
	ulint		old_activity_count,	/*!< old activity count */
						/*!< old change buffer merge
						activity count, or
						ULINT_UNDEFINED */
	ulint		old_ibuf_merge_activity_count = ULINT_UNDEFINED);
/******************************************************************//**
Increment the server activity counter. */
UNIV_INTERN
void
srv_inc_activity_count(
/*===================*/
	bool ibuf_merge_activity = false);	/*!< whether this activity bump
						is caused by the background
						change buffer merge */

/**********************************************************************//**
Enqueues a task to server task queue and releases a worker thread, if there
is a suspended one. */
UNIV_INTERN
void
srv_que_task_enqueue_low(
/*=====================*/
	que_thr_t*	thr);	/*!< in: query thread */

/**********************************************************************//**
Check whether any background thread is active. If so, return the thread
type.
@return SRV_NONE if all are are suspended or have exited, thread
type if any are still active. */
UNIV_INTERN
enum srv_thread_type
srv_get_active_thread_type(void);
/*============================*/

extern "C" {

/*********************************************************************//**
A thread which prints the info output by various InnoDB monitors.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_monitor_thread)(
/*===============================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*********************************************************************//**
The master thread controlling the server.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_master_thread)(
/*==============================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*************************************************************************
A thread which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_error_monitor_thread)(
/*=====================================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*********************************************************************//**
Purge coordinator thread that schedules the purge tasks.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_purge_coordinator_thread)(
/*=========================================*/
	void*	arg MY_ATTRIBUTE((unused)));	/*!< in: a dummy parameter
						required by os_thread_create */

/*********************************************************************//**
Worker thread that reads tasks from the work queue and executes them.
@return	a dummy parameter */
UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(srv_worker_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)));	/*!< in: a dummy parameter
						required by os_thread_create */
} /* extern "C" */

/**********************************************************************//**
Get count of tasks in the queue.
@return number of tasks in queue  */
UNIV_INTERN
ulint
srv_get_task_queue_length(void);
/*===========================*/

/*********************************************************************//**
Releases threads of the type given from suspension in the thread table.
NOTE! The server mutex has to be reserved by the caller!
@return number of threads released: this may be less than n if not
enough threads were suspended at the moment */
UNIV_INTERN
ulint
srv_release_threads(
/*================*/
	enum srv_thread_type	type,	/*!< in: thread type */
	ulint			n);	/*!< in: number of threads to release */

/**********************************************************************//**
Check whether any background thread are active. If so print which thread
is active. Send the threads wakeup signal.
@return name of thread that is active or NULL */
UNIV_INTERN
const char*
srv_any_background_threads_are_active(void);
/*=======================================*/

/**********************************************************************//**
Wakeup the purge threads. */
UNIV_INTERN
void
srv_purge_wakeup(void);
/*==================*/

/** Check whether given space id is undo tablespace id
@param[in]	space_id	space id to check
@return true if it is undo tablespace else false. */
bool
srv_is_undo_tablespace(
	ulint	space_id);

/** Status variables to be passed to MySQL */
struct export_var_t{
	ulint innodb_adaptive_hash_hash_searches;
	ulint innodb_adaptive_hash_non_hash_searches;
	ulint innodb_background_log_sync;
	ulint innodb_data_pending_reads;	/*!< Pending reads */
	ulint innodb_data_pending_writes;	/*!< Pending writes */
	ulint innodb_data_pending_fsyncs;	/*!< Pending fsyncs */
	ulint innodb_data_fsyncs;		/*!< Number of fsyncs so far */
	ulint innodb_data_read;			/*!< Data bytes read */
	ulint innodb_data_writes;		/*!< I/O write requests */
	ulint innodb_data_written;		/*!< Data bytes written */
	ulint innodb_data_reads;		/*!< I/O read requests */
	char  innodb_buffer_pool_dump_status[512];/*!< Buf pool dump status */
	char  innodb_buffer_pool_load_status[512];/*!< Buf pool load status */
	ulint innodb_buffer_pool_pages_total;	/*!< Buffer pool size */
	ulint innodb_buffer_pool_pages_data;	/*!< Data pages */
	ulint innodb_buffer_pool_bytes_data;	/*!< File bytes used */
	ulint innodb_buffer_pool_pages_dirty;	/*!< Dirty data pages */
	ulint innodb_buffer_pool_bytes_dirty;	/*!< File bytes modified */
	ulint innodb_buffer_pool_pages_misc;	/*!< Miscellanous pages */
	ulint innodb_buffer_pool_pages_free;	/*!< Free pages */
#ifdef UNIV_DEBUG
	ulint innodb_buffer_pool_pages_latched;	/*!< Latched pages */
#endif /* UNIV_DEBUG */
	ulint innodb_buffer_pool_pages_made_not_young;
	ulint innodb_buffer_pool_pages_made_young;
	ulint innodb_buffer_pool_pages_old;
	ulint innodb_buffer_pool_read_requests;	/*!< buf_pool->stat.n_page_gets */
	ulint innodb_buffer_pool_reads;		/*!< srv_buf_pool_reads */
	ulint innodb_buffer_pool_wait_free;	/*!< srv_buf_pool_wait_free */
	ulint innodb_buffer_pool_pages_flushed;	/*!< srv_buf_pool_flushed */
	ulint innodb_buffer_pool_pages_LRU_flushed;	/*!< buf_lru_flush_page_count */
	ulint innodb_buffer_pool_write_requests;/*!< srv_buf_pool_write_requests */
	ulint innodb_buffer_pool_read_ahead_rnd;/*!< srv_read_ahead_rnd */
	ulint innodb_buffer_pool_read_ahead;	/*!< srv_read_ahead */
	ulint innodb_buffer_pool_read_ahead_evicted;/*!< srv_read_ahead evicted*/
	ulint innodb_checkpoint_age;
	ulint innodb_checkpoint_max_age;
	ulint innodb_dblwr_pages_written;	/*!< srv_dblwr_pages_written */
	ulint innodb_dblwr_writes;		/*!< srv_dblwr_writes */
	ulint innodb_deadlocks;
	ibool innodb_have_atomic_builtins;	/*!< HAVE_ATOMIC_BUILTINS */
	ulint innodb_history_list_length;
	ulint innodb_ibuf_size;
	ulint innodb_ibuf_free_list;
	ulint innodb_ibuf_segment_size;
	ulint innodb_ibuf_merges;
	ulint innodb_ibuf_merged_inserts;
	ulint innodb_ibuf_merged_delete_marks;
	ulint innodb_ibuf_merged_deletes;
	ulint innodb_ibuf_discarded_inserts;
	ulint innodb_ibuf_discarded_delete_marks;
	ulint innodb_ibuf_discarded_deletes;
	ulint innodb_log_waits;			/*!< srv_log_waits */
	ulint innodb_log_write_requests;	/*!< srv_log_write_requests */
	ulint innodb_log_writes;		/*!< srv_log_writes */
	lsn_t innodb_os_log_written;		/*!< srv_os_log_written */
	lsn_t innodb_lsn_current;
	lsn_t innodb_lsn_flushed;
	lsn_t innodb_lsn_last_checkpoint;
	ulint innodb_master_thread_active_loops;/*!< srv_main_active_loops */
	ulint innodb_master_thread_idle_loops;	/*!< srv_main_idle_loops */
	ib_int64_t innodb_max_trx_id;
	ulint innodb_mem_adaptive_hash;
	ulint innodb_mem_dictionary;
	ulint innodb_mem_total;
	ib_int64_t innodb_mutex_os_waits;
	ib_int64_t innodb_mutex_spin_rounds;
	ib_int64_t innodb_mutex_spin_waits;
	ib_int64_t innodb_oldest_view_low_limit_trx_id;
	ulint innodb_os_log_fsyncs;		/*!< fil_n_log_flushes */
	ulint innodb_os_log_pending_writes;	/*!< srv_os_log_pending_writes */
	ulint innodb_os_log_pending_fsyncs;	/*!< fil_n_pending_log_flushes */
	ulint innodb_page_size;			/*!< UNIV_PAGE_SIZE */
	ulint innodb_pages_created;		/*!< buf_pool->stat.n_pages_created */
	ulint innodb_pages_read;		/*!< buf_pool->stat.n_pages_read */
	ulint innodb_pages_written;		/*!< buf_pool->stat.n_pages_written */
	ib_int64_t innodb_purge_trx_id;
	ib_int64_t innodb_purge_undo_no;
	ulint innodb_row_lock_waits;		/*!< srv_n_lock_wait_count */
	ulint innodb_row_lock_current_waits;	/*!< srv_n_lock_wait_current_count */
	ib_int64_t innodb_row_lock_time;	/*!< srv_n_lock_wait_time
						/ 1000 */
	ulint innodb_row_lock_time_avg;		/*!< srv_n_lock_wait_time
						/ 1000
						/ srv_n_lock_wait_count */
	ulint innodb_row_lock_time_max;		/*!< srv_n_lock_max_wait_time
						/ 1000 */
	ulint innodb_current_row_locks;
	ulint innodb_rows_read;			/*!< srv_n_rows_read */
	ulint innodb_rows_inserted;		/*!< srv_n_rows_inserted */
	ulint innodb_rows_updated;		/*!< srv_n_rows_updated */
	ulint innodb_rows_deleted;		/*!< srv_n_rows_deleted */
	ulint innodb_num_open_files;		/*!< fil_n_file_opened */
	ulint innodb_truncated_status_writes;	/*!< srv_truncated_status_writes */
	ulint innodb_available_undo_logs;       /*!< srv_available_undo_logs */
	ulint innodb_read_views_memory;		/*!< srv_read_views_memory */
	ulint innodb_descriptors_memory;	/*!< srv_descriptors_memory */
	ib_int64_t innodb_s_lock_os_waits;
	ib_int64_t innodb_s_lock_spin_rounds;
	ib_int64_t innodb_s_lock_spin_waits;
	ib_int64_t innodb_x_lock_os_waits;
	ib_int64_t innodb_x_lock_spin_rounds;
	ib_int64_t innodb_x_lock_spin_waits;
#ifdef UNIV_DEBUG
	ulint innodb_purge_trx_id_age;		/*!< rw_max_trx_id - purged trx_id */
	ulint innodb_purge_view_trx_id_age;	/*!< rw_max_trx_id
						- purged view's min trx_id */
#endif /* UNIV_DEBUG */
	ulint innodb_column_compressed;		/*!< srv_column_compressed */
	ulint innodb_column_decompressed;	/*!< srv_column_decompressed */

	ulint innodb_sec_rec_cluster_reads;	/*!< srv_sec_rec_cluster_reads */
	ulint innodb_sec_rec_cluster_reads_avoided; /*!< srv_sec_rec_cluster_reads_avoided */

	ulint innodb_buffered_aio_submitted;

	fragmentation_stats_t innodb_fragmentation_stats;/*!< Fragmentation
						statistics */
};

/** Thread slot in the thread table.  */
struct srv_slot_t{
	srv_thread_type type;			/*!< thread type: user,
						utility etc. */
	ibool		in_use;			/*!< TRUE if this slot
						is in use */
	ibool		suspended;		/*!< TRUE if the thread is
						waiting for the event of this
						slot */
	ib_time_t	suspend_time;		/*!< time when the thread was
						suspended. Initialized by
						lock_wait_table_reserve_slot()
						for lock wait */
	ulong		wait_timeout;		/*!< wait time that if exceeded
						the thread will be timed out.
						Initialized by
						lock_wait_table_reserve_slot()
						for lock wait */
	os_event_t	event;			/*!< event used in suspending
						the thread when it has nothing
						to do */
	que_thr_t*	thr;			/*!< suspended query thread
						(only used for user threads) */
};

#else /* !UNIV_HOTBACKUP */
# define srv_use_adaptive_hash_indexes		FALSE
# define srv_use_native_aio			FALSE
# define srv_numa_interleave			FALSE
# define srv_force_recovery			0UL
# define srv_set_io_thread_op_info(t,info)	((void) 0)
# define srv_reset_io_thread_op_info()		((void) 0)
# define srv_is_being_started			0
# define srv_win_file_flush_method		SRV_WIN_IO_UNBUFFERED
# define srv_unix_file_flush_method		SRV_UNIX_O_DSYNC
# define srv_start_raw_disk_in_use		0
# define srv_file_per_table			1
#endif /* !UNIV_HOTBACKUP */
#ifdef WITH_WSREP
UNIV_INTERN
void
wsrep_srv_conc_cancel_wait(
/*==================*/
	trx_t*	trx);	/*!< in: transaction object associated with the
			thread */
#endif /* WITH_WSREP */

#ifndef DBUG_OFF
/** false before InnoDB monitor has been printed at least once, true
afterwards */
extern bool	srv_debug_monitor_printed;
#else
#define	srv_debug_monitor_printed	false
#endif

#endif
