/* Copyright (c) 2010, 2018, Oracle and/or its affiliates. All rights reserved.

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

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql_reload.h"
#include "sql_priv.h"
#include "mysqld.h"      // select_errors
#include "sql_class.h"   // THD
#include "sql_acl.h"     // acl_reload
#include "sql_servers.h" // servers_reload
#include "sql_connect.h" // reset_mqh
#include "sql_base.h"    // close_cached_tables
#include "sql_db.h"      // my_dbopt_cleanup
#include "hostname.h"    // hostname_cache_refresh
#include "rpl_master.h"  // reset_master
#include "rpl_slave.h"   // reset_slave
#include "rpl_rli.h"     // rotate_relay_log
#include "rpl_mi.h"
#include "debug_sync.h"
#include "des_key_file.h"

#ifdef WITH_WSREP
#include "sql_parse.h"
#endif /* WITH_WSREP */

/**
  Reload/resets privileges and the different caches.

  @param thd Thread handler (can be NULL!)
  @param options What should be reset/reloaded (tables, privileges, slave...)
  @param tables Tables to flush (if any)
  @param write_to_binlog < 0 if there was an error while interacting with the binary log inside
                         reload_acl_and_cache,
                         0 if we should not write to the binary log,
                         > 0 if we can write to the binlog.

               
  @note Depending on 'options', it may be very bad to write the
    query to the binlog (e.g. FLUSH SLAVE); this is a
    pointer where reload_acl_and_cache() will put 0 if
    it thinks we really should not write to the binlog.
    Otherwise it will put 1.

  @return Error status code
    @retval 0 Ok
    @retval !=0  Error; thd->killed is set or thd->is_error() is true
*/

bool reload_acl_and_cache(THD *thd, unsigned long options,
                          TABLE_LIST *tables, int *write_to_binlog)
{
  bool result=0;
  select_errors=0;				/* Write if more errors */
  int tmp_write_to_binlog= *write_to_binlog= 1;

  DBUG_ASSERT(!thd || !thd->in_sub_stmt);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (options & REFRESH_GRANT)
  {
    THD *tmp_thd= 0;
    /*
      If reload_acl_and_cache() is called from SIGHUP handler we have to
      allocate temporary THD for execution of acl_reload()/grant_reload().
    */
    if (!thd && (thd= (tmp_thd= new THD)))
    {
      thd->thread_stack= (char*) &tmp_thd;
      thd->store_globals();
    }

    if (thd)
    {
      bool reload_acl_failed= acl_reload(thd);
      bool reload_grants_failed= grant_reload(thd);
      bool reload_servers_failed= servers_reload(thd);

      if (reload_acl_failed || reload_grants_failed || reload_servers_failed)
      {
        result= 1;
        /*
          When an error is returned, my_message may have not been called and
          the client will hang waiting for a response.
        */
        my_error(ER_UNKNOWN_ERROR, MYF(0));
      }
    }

    if (tmp_thd)
    {
      delete tmp_thd;
      /* Remember that we don't have a THD */
      my_pthread_setspecific_ptr(THR_THD,  0);
      thd= 0;
    }
    reset_mqh((LEX_USER *)NULL, TRUE);
  }
#endif
  if (options & REFRESH_LOG)
  {
    /*
      Flush the normal query log, the update log, the binary log,
      the slow query log, the relay log (if it exists) and the log
      tables.
    */

    options|= REFRESH_BINARY_LOG;
    options|= REFRESH_RELAY_LOG;
    options|= REFRESH_SLOW_LOG;
    options|= REFRESH_GENERAL_LOG;
    options|= REFRESH_ENGINE_LOG;
    options|= REFRESH_ERROR_LOG;
  }

  if (options & REFRESH_ERROR_LOG)
    if (flush_error_log())
    {
      /*
        When flush_error_log() failed, my_error() has not been called.
        So, we have to do it here to keep the protocol.
      */
      my_error(ER_UNKNOWN_ERROR, MYF(0));
      result= 1;
    }

  if ((options & REFRESH_SLOW_LOG) && opt_slow_log)
    if (logger.flush_slow_log())
      result= 1;

  if ((options & REFRESH_GENERAL_LOG) && opt_log)
    if (logger.flush_general_log())
      result= 1;

  if (options & REFRESH_ENGINE_LOG)
    if (ha_flush_logs(NULL))
      result= 1;
  if ((options & REFRESH_BINARY_LOG) || (options & REFRESH_RELAY_LOG ))
  {
    /*
      If reload_acl_and_cache() is called from SIGHUP handler we have to
      allocate temporary THD for execution of binlog/relay log rotation.
     */
    THD *tmp_thd= 0;
    if (!thd && (thd= (tmp_thd= new THD)))
    {
      thd->thread_stack= (char *) (&tmp_thd);
      thd->store_globals();
    }

    if (options & REFRESH_BINARY_LOG)
    {
      /*
        Writing this command to the binlog may result in infinite loops
        when doing mysqlbinlog|mysql, and anyway it does not really make
        sense to log it automatically (would cause more trouble to users
        than it would help them)
       */
      tmp_write_to_binlog= 0;
      if (mysql_bin_log.is_open())
      {
        if (mysql_bin_log.rotate_and_purge(thd, true))
          *write_to_binlog= -1;
      }
    }
    if (options & REFRESH_RELAY_LOG)
    {
#ifdef HAVE_REPLICATION
      mysql_mutex_lock(&LOCK_active_mi);
      if (active_mi != NULL)
      {
        mysql_mutex_lock(&active_mi->data_lock);
        if (rotate_relay_log(active_mi, true/*need_log_space_lock=true*/))
          *write_to_binlog= -1;
        mysql_mutex_unlock(&active_mi->data_lock);
      }
      mysql_mutex_unlock(&LOCK_active_mi);
#endif
    }
    if (tmp_thd)
    {
      delete tmp_thd;
      /* Remember that we don't have a THD */
      my_pthread_setspecific_ptr(THR_THD,  0);
      thd= 0;
    }
  }
#ifdef HAVE_QUERY_CACHE
  if (options & REFRESH_QUERY_CACHE_FREE)
  {
    query_cache.pack();				// FLUSH QUERY CACHE
    options &= ~REFRESH_QUERY_CACHE;    // Don't flush cache, just free memory
  }
  if (options & (REFRESH_TABLES | REFRESH_QUERY_CACHE))
  {
    query_cache.flush();			// RESET QUERY CACHE
  }
#endif /*HAVE_QUERY_CACHE*/

  DBUG_ASSERT(!thd || thd->locked_tables_mode ||
              !thd->mdl_context.has_locks() ||
              thd->handler_tables_hash.records ||
              thd->ull_hash.records ||
              thd->global_read_lock.is_acquired() ||
              thd->backup_tables_lock.is_acquired() ||
              thd->backup_binlog_lock.is_acquired());

  /*
    Note that if REFRESH_READ_LOCK bit is set then REFRESH_TABLES is set too
    (see sql_yacc.yy)
  */
  if (options & (REFRESH_TABLES | REFRESH_READ_LOCK)) 
  {
    if ((options & REFRESH_READ_LOCK) && thd)
    {
      bool own_lock;
      /*
        On the first hand we need write lock on the tables to be flushed,
        on the other hand we must not try to aspire a global read lock
        if we have a write locked table as this would lead to a deadlock
        when trying to reopen (and re-lock) the table after the flush.
      */
      if (thd->locked_tables_mode)
      {
        my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
        return 1;
      }
      /*
	Writing to the binlog could cause deadlocks, as we don't log
	UNLOCK TABLES
      */
      tmp_write_to_binlog= 0;
      if (thd->global_read_lock.lock_global_read_lock(thd, &own_lock))
        return 1; // Killed
      if (close_cached_tables(thd, tables,
                              ((options & REFRESH_FAST) ?  FALSE : TRUE),
                              thd->variables.lock_wait_timeout))
      {
        /*
          NOTE: my_error() has been already called by reopen_tables() within
          close_cached_tables().
        */
        result= 1;
      }

      if (thd->global_read_lock.make_global_read_lock_block_commit(thd)) // Killed
      {
        /* Don't leave things in a half-locked state */
        if (own_lock)
        {
          thd->global_read_lock.unlock_global_read_lock(thd);
        }
        return 1;
      }
#ifdef WITH_WSREP
      /*
        We need to do it second time after wsrep appliers were blocked in
        make_global_read_lock_block_commit(thd) above since they could have
        modified the tables too.
      */
      if (WSREP(thd) && 
	  close_cached_tables(thd, tables, (options & REFRESH_FAST) ?
                              FALSE : TRUE, TRUE))
          result= 1;
#endif /* WITH_WSREP */
     }
    else
    {
      if (thd && thd->locked_tables_mode)
      {
        /*
          If we are under LOCK TABLES we should have a write
          lock on tables which we are going to flush.
        */
        if (tables)
        {
          for (TABLE_LIST *t= tables; t; t= t->next_local)
            if (!find_table_for_mdl_upgrade(thd, t->db, t->table_name, false))
              return 1;
        }
        else
        {
          /*
            It is not safe to upgrade the metadata lock without GLOBAL IX lock.
            This can happen with FLUSH TABLES <list> WITH READ LOCK as we in these
            cases don't take a GLOBAL IX lock in order to be compatible with
            global read lock.
          */
          if (thd->open_tables &&
              !thd->mdl_context.is_lock_owner(MDL_key::GLOBAL, "", "",
                                              MDL_INTENTION_EXCLUSIVE))
          {
            my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0),
                     thd->open_tables->s->table_name.str);
            return true;
          }

          for (TABLE *tab= thd->open_tables; tab; tab= tab->next)
          {
            if (! tab->mdl_ticket->is_upgradable_or_exclusive())
            {
              my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0),
                       tab->s->table_name.str);
              return 1;
            }
          }
        }
      }

#ifdef WITH_WSREP
      if (WSREP(thd) && !thd->lex->no_write_to_binlog
                     && (options & REFRESH_TABLES)
                     && !(options & (REFRESH_FOR_EXPORT|REFRESH_READ_LOCK)))
      {
        /*
          This is done here because LOCK TABLES is not replicated in galera,
          the upgrade of which is checked above.  Hence, done after/if we
          are able to upgrade locks.

          Also, note that, in error log with debug you may see
          'thread holds MDL locks at TI' but since this is a flush
          tables and is required for LOCK TABLE WRITE
          it can be ignored there.
        */
        if (tables)
        {
            if (wsrep_to_isolation_begin(thd, NULL, NULL, tables))
            {
                result= 1;
                goto cleanup;
            }
        }
        else if (wsrep_to_isolation_begin(thd, WSREP_MYSQL_DB, NULL, NULL))
        {
                result= 1;
                goto cleanup;
        }
      }

      if (thd && (thd->wsrep_applier || thd->slave_thread))
      {
        /*
          In case of wsrep-applier/mysql-slave thread, do not wait for table
          share(s) to be removed from table definition cache.

          This is important otherwise it can lead to a deadlock in following
          scenario:
          * A parallel DML workload is being carried out on node-1
          * FLUSH TABLE is executed on node-2 which is then replicated.
            Applier applies this action on node-1. For doing this it register
            it with galera eco-system and get a commit order seqno assigned.
          * FLUSH TABLE will wait for TABLE SHARE count to drop to 0.
          * This table share count can't drop to 0 till all DML are done.
          * Take a case where-in a DML action entered galera post FLUSH TABLE
            got its seqno assigned and then immediately went ahead and opened
            the table. This thread will release TABLE SHARE only on commit.
            But commit can't proceed as its seqno is > seqno of FLUSH TABLE.
          * FLUSH TABLE continue to wait for TABLE SHARE count to drop to 0.
          This all leads to DEADLOCK.
          With REFRESH_FAST, FLUSH_TABLE thread will not wait for TABLE SHARE
          count to drop to 0 and avoiding DEADLOCK.
        */
        options|= REFRESH_FAST;
      }
#endif /* WITH_WSREP */

      if (close_cached_tables(thd, tables,
                              ((options & REFRESH_FAST) ?  FALSE : TRUE),
                              (thd ? thd->variables.lock_wait_timeout :
                               LONG_TIMEOUT)))
      {
        /*
          NOTE: my_error() has been already called by reopen_tables() within
          close_cached_tables().
        */
        result= 1;
      }
    }

#ifdef WITH_WSREP
cleanup:
#endif /* WITH_WSREP */

    my_dbopt_cleanup();
  }
  if (options & REFRESH_HOSTS)
    hostname_cache_refresh();
  if (thd && (options & REFRESH_STATUS))
    refresh_status(thd);
  if (options & REFRESH_THREADS)
    kill_blocked_pthreads();
#ifdef HAVE_REPLICATION
  if (options & REFRESH_MASTER)
  {
    DBUG_ASSERT(thd);
    tmp_write_to_binlog= 0;
    if (reset_master(thd))
    {
      /* NOTE: my_error() has been already called by reset_master(). */
      result= 1;
    }
  }
#endif
#ifdef HAVE_OPENSSL
   if (options & REFRESH_DES_KEY_FILE)
   {
     if (des_key_file && load_des_key_file(des_key_file))
     {
       /* NOTE: my_error() has been already called by load_des_key_file(). */
       result= 1;
     }
   }
#endif
#ifdef HAVE_REPLICATION
 if (options & REFRESH_SLAVE)
 {
   tmp_write_to_binlog= 0;
   mysql_mutex_lock(&LOCK_active_mi);
   if (active_mi != NULL && reset_slave(thd, active_mi))
   {
     /* NOTE: my_error() has been already called by reset_slave(). */
     result= 1;
   }
   else if (active_mi == NULL)
   {
     result= 1;
     my_error(ER_SLAVE_CONFIGURATION, MYF(0));
   }
   mysql_mutex_unlock(&LOCK_active_mi);
 }
#endif
  if (options & REFRESH_USER_RESOURCES)
    reset_mqh((LEX_USER *) NULL, 0);             /* purecov: inspected */
#ifndef EMBEDDED_LIBRARY
  if (options & REFRESH_TABLE_STATS)
  {
    mysql_mutex_lock(&LOCK_global_table_stats);
    free_global_table_stats();
    init_global_table_stats();
    mysql_mutex_unlock(&LOCK_global_table_stats);
  }
  if (options & REFRESH_INDEX_STATS)
  {
    mysql_mutex_lock(&LOCK_global_index_stats);
    free_global_index_stats();
    init_global_index_stats();
    mysql_mutex_unlock(&LOCK_global_index_stats);
  }
  if (options & (REFRESH_USER_STATS | REFRESH_CLIENT_STATS | REFRESH_THREAD_STATS))
  {
    mysql_mutex_lock(&LOCK_global_user_client_stats);
    if (options & REFRESH_USER_STATS)
    {
      free_global_user_stats();
      init_global_user_stats();
    }
    if (options & REFRESH_CLIENT_STATS)
    {
      free_global_client_stats();
      init_global_client_stats();
    }
    if (options & REFRESH_THREAD_STATS)
    {
      free_global_thread_stats();
      init_global_thread_stats();
    }
    mysql_mutex_unlock(&LOCK_global_user_client_stats);
  }
#endif
  if (options & REFRESH_FLUSH_PAGE_BITMAPS)
  {
    result= ha_flush_changed_page_bitmaps();
    if (result)
    {
      my_error(ER_UNKNOWN_ERROR, MYF(0), "FLUSH CHANGED_PAGE_BITMAPS");
    }
  }
  if (options & REFRESH_RESET_PAGE_BITMAPS)
  {
    result= ha_purge_changed_page_bitmaps(0);
    if (result)
    {
      my_error(ER_UNKNOWN_ERROR, MYF(0), "RESET CHANGED_PAGE_BITMAPS");
    }
  }
 if (*write_to_binlog != -1)
 {
   if (thd == NULL || thd->security_ctx == NULL)
   {
     *write_to_binlog=
       opt_binlog_skip_flush_commands ? 0 : tmp_write_to_binlog;
   }
   else if ((thd->security_ctx->master_access & SUPER_ACL) != 0)
   {
     /*
       For users with 'SUPER' privilege 'FLUSH XXX' statements must not be
       binlogged if 'super_read_only' is set to 'ON'.
     */
     if (opt_super_readonly)
       *write_to_binlog= 0;
     else
       *write_to_binlog=
         opt_binlog_skip_flush_commands ? 0 : tmp_write_to_binlog;
   }
   else
   {
     /*
       For users without 'SUPER' privilege 'FLUSH XXX' statements must not be
       binlogged if 'read_only' or 'super_read_only' is set to 'ON'.
       Checking only 'opt_readonly' here as in 'super_read_only' mode this
       variable is implicitly set to 'true'.
     */
     if (opt_readonly)
       *write_to_binlog= 0;
     else
       *write_to_binlog=
         opt_binlog_skip_flush_commands ? 0 : tmp_write_to_binlog;
   }
 }
 /*
   If the query was killed then this function must fail.
 */
 return result || (thd ? thd->killed : 0);
}


/**
  Implementation of FLUSH TABLES <table_list> WITH READ LOCK.

  In brief: take exclusive locks, expel tables from the table
  cache, reopen the tables, enter the 'LOCKED TABLES' mode,
  downgrade the locks.
  Note: the function is written to be called from
  mysql_execute_command(), it is not reusable in arbitrary
  execution context.

  Required privileges
  -------------------
  Since the statement implicitly enters LOCK TABLES mode,
  it requires LOCK TABLES privilege on every table.
  But since the rest of FLUSH commands require
  the global RELOAD_ACL, it also requires RELOAD_ACL.

  Compatibility with the global read lock
  ---------------------------------------
  We don't wait for the GRL, since neither the
  5.1 combination that this new statement is intended to
  replace (LOCK TABLE <list> WRITE; FLUSH TABLES;),
  nor FLUSH TABLES WITH READ LOCK do.
  @todo: this is not implemented, Dmitry disagrees.
  Currently we wait for GRL in another connection,
  but are compatible with a GRL in our own connection.

  Behaviour under LOCK TABLES
  ---------------------------
  Bail out: i.e. don't perform an implicit UNLOCK TABLES.
  This is not consistent with LOCK TABLES statement, but is
  in line with behaviour of FLUSH TABLES WITH READ LOCK, and we
  try to not introduce any new statements with implicit
  semantics.

  Compatibility with parallel updates
  -----------------------------------
  As a result, we will wait for all open transactions
  against the tables to complete. After the lock downgrade,
  new transactions will be able to read the tables, but not
  write to them.

  Differences from FLUSH TABLES <list>
  -------------------------------------
  - you can't flush WITH READ LOCK a non-existent table
  - you can't flush WITH READ LOCK under LOCK TABLES

  Effect on views and temporary tables.
  ------------------------------------
  You can only apply this command to existing base tables.
  If a view with such name exists, ER_WRONG_OBJECT is returned.
  If a temporary table with such name exists, it's ignored:
  if there is a base table, it's used, otherwise ER_NO_SUCH_TABLE
  is returned.

  Handling of MERGE tables
  ------------------------
  For MERGE table this statement will open and lock child tables
  for read (it is impossible to lock parent table without it).
  Child tables won't be flushed unless they are explicitly present
  in the statement's table list.

  Implicit commit
  ---------------
  This statement causes an implicit commit before and
  after it.

  HANDLER SQL
  -----------
  If this connection has HANDLERs open against
  some of the tables being FLUSHed, these handlers
  are implicitly flushed (lose their position).
*/

bool flush_tables_with_read_lock(THD *thd, TABLE_LIST *all_tables)
{
  Lock_tables_prelocking_strategy lock_tables_prelocking_strategy;
  TABLE_LIST *table_list;

  /*
    This is called from SQLCOM_FLUSH, the transaction has
    been committed implicitly.
  */

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    goto error;
  }

  /*
    Acquire SNW locks on tables to be flushed. Don't acquire global
    IX and database-scope IX locks on the tables as this will make
    this statement incompatible with FLUSH TABLES WITH READ LOCK.
  */
  if (lock_table_names(thd, all_tables, NULL,
                       thd->variables.lock_wait_timeout,
                       MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK))
    goto error;

  DEBUG_SYNC(thd,"flush_tables_with_read_lock_after_acquire_locks");

  for (table_list= all_tables; table_list;
       table_list= table_list->next_global)
  {
    /* Request removal of table from cache. */
    tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED,
                     table_list->db,
                     table_list->table_name, FALSE);
    /* Reset ticket to satisfy asserts in open_tables(). */
    table_list->mdl_request.ticket= NULL;
  }

  /*
    Before opening and locking tables the below call also waits
    for old shares to go away, so the fact that we don't pass
    MYSQL_OPEN_IGNORE_FLUSH flag to it is important.
    Also we don't pass MYSQL_OPEN_HAS_MDL_LOCK flag as we want
    to open underlying tables if merge table is flushed.
    For underlying tables of the merge the below call has to
    acquire SNW locks to ensure that they can be locked for
    read without further waiting.
  */
  if (open_and_lock_tables(thd, all_tables, FALSE,
                           MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK,
                           &lock_tables_prelocking_strategy) ||
      thd->locked_tables_list.init_locked_tables(thd))
  {
    goto error;
  }
  thd->variables.option_bits|= OPTION_TABLE_LOCK;

  /*
    We don't downgrade MDL_SHARED_NO_WRITE here as the intended
    post effect of this call is identical to LOCK TABLES <...> READ,
    and we didn't use thd->in_lock_talbes and
    thd->sql_command= SQLCOM_LOCK_TABLES hacks to enter the LTM.
  */

  return FALSE;

error:
  return TRUE;
}


/**
  Prepare tables for export (transportable tablespaces) by
  a) waiting until write transactions/DDL operations using these
     tables have completed.
  b) block new write operations/DDL operations on these tables.

  Once this is done, notify the storage engines using handler::extra().

  Finally, enter LOCK TABLES mode, so that locks are held
  until UNLOCK TABLES is executed.

  @param thd         Thread handler
  @param all_tables  TABLE_LIST for tables to be exported

  @retval false  Ok
  @retval true   Error
*/

bool flush_tables_for_export(THD *thd, TABLE_LIST *all_tables)
{
  Lock_tables_prelocking_strategy lock_tables_prelocking_strategy;

  /*
    This is called from SQLCOM_FLUSH, the transaction has
    been committed implicitly.
  */

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    return true;
  }

  /*
    Acquire SNW locks on tables to be exported. Don't acquire
    global IX as this will make this statement incompatible
    with FLUSH TABLES WITH READ LOCK.
  */
  if (open_and_lock_tables(thd, all_tables, false,
                           MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK,
                           &lock_tables_prelocking_strategy))
  {
    return true;
  }

  // Check if all storage engines support FOR EXPORT.
  for (TABLE_LIST *table_list= all_tables; table_list;
       table_list= table_list->next_global)
  {
    if (!(table_list->table->file->ha_table_flags() & HA_CAN_EXPORT))
    {
      my_error(ER_ILLEGAL_HA, MYF(0), table_list->table_name);
      return true;
    }
  }

  // Notify the storage engines that the tables should be made ready for export.
  for (TABLE_LIST *table_list= all_tables; table_list;
       table_list= table_list->next_global)
  {
    handler *handler_file= table_list->table->file;
    int error= handler_file->extra(HA_EXTRA_EXPORT);
    if (error)
    {
      handler_file->print_error(error, MYF(0));
      return true;
    }
  }

  // Enter LOCKED TABLES mode.
  if (thd->locked_tables_list.init_locked_tables(thd))
    return true;
  thd->variables.option_bits|= OPTION_TABLE_LOCK;

  return false;
}
