/* Copyright 2008-2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "wsrep_var.h"

#include <mysqld.h>
#include <sql_class.h>
#include <sql_plugin.h>
#include <set_var.h>
#include <sql_acl.h>
#include "wsrep_priv.h"
#include "wsrep_thd.h"
#include "wsrep_xid.h"
#include <my_dir.h>
#include <cstdio>
#include <cstdlib>

const  char* wsrep_provider         = 0;
const  char* wsrep_provider_options = 0;
const  char* wsrep_cluster_address  = 0;
const  char* wsrep_cluster_name     = 0;
const  char* wsrep_node_name        = 0;
const  char* wsrep_node_address     = 0;
const  char* wsrep_node_incoming_address = 0;
const  char* wsrep_start_position   = 0;
ulong   wsrep_reject_queries;

int wsrep_init_vars()
{
  wsrep_provider        = my_strdup(WSREP_NONE, MYF(MY_WME));
  wsrep_provider_options= my_strdup("", MYF(MY_WME));
  wsrep_cluster_address = my_strdup("", MYF(MY_WME));
  wsrep_cluster_name    = my_strdup(WSREP_CLUSTER_NAME, MYF(MY_WME));
  wsrep_node_name       = my_strdup("", MYF(MY_WME));
  wsrep_node_address    = my_strdup("", MYF(MY_WME));
  wsrep_node_incoming_address= my_strdup(WSREP_NODE_INCOMING_AUTO, MYF(MY_WME));
  wsrep_start_position  = my_strdup(WSREP_START_POSITION_ZERO, MYF(MY_WME));

  global_system_variables.binlog_format=BINLOG_FORMAT_ROW;
  return 0;
}

/* Function checks if the new value for wsrep_on is valid.
Toggling of the value inside function or transaction is not allowed
@return false if no error encountered with check else return true. */
bool wsrep_on_check (sys_var *self, THD* thd, set_var* var)
{
  if (var->type == OPT_GLOBAL)
    return true;

  /* If in a stored function/trigger, it's too late to change wsrep_on. */
  if (thd->in_sub_stmt)
  {
    WSREP_WARN("Cannot modify @@session.wsrep_on inside a stored function "
               " or trigger");
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             var->save_result.ulonglong_value ? "ON" : "OFF");
    return true;
  }
  /* Make the session variable 'wsrep_on' read-only inside a transaction. */
  if (thd->in_active_multi_stmt_transaction())
  {
    WSREP_WARN("Cannot modify @@session.wsrep_on inside a transaction");
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             var->save_result.ulonglong_value ? "ON" : "OFF");
    return true;
  }

  return false;
}

bool wsrep_on_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  return false;
}

bool wsrep_causal_reads_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  // global setting should not affect session setting.
  // if (var_type == OPT_GLOBAL) {
  //   thd->variables.wsrep_causal_reads = global_system_variables.wsrep_causal_reads;
  // }
  if (thd->variables.wsrep_causal_reads) {
    thd->variables.wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
  } else {
    thd->variables.wsrep_sync_wait &= ~WSREP_SYNC_WAIT_BEFORE_READ;
  }

  // update global settings too.
  if (global_system_variables.wsrep_causal_reads) {
      global_system_variables.wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
  } else {
      global_system_variables.wsrep_sync_wait &= ~WSREP_SYNC_WAIT_BEFORE_READ;
  }
  return false;
}

bool wsrep_sync_wait_update (sys_var* self, THD* thd, enum_var_type var_type)
{
  // global setting should not affect session setting.
  // if (var_type == OPT_GLOBAL) {
  //   thd->variables.wsrep_sync_wait = global_system_variables.wsrep_sync_wait;
  // }
  thd->variables.wsrep_causal_reads = thd->variables.wsrep_sync_wait &
          WSREP_SYNC_WAIT_BEFORE_READ;

  // update global settings too
  global_system_variables.wsrep_causal_reads = global_system_variables.wsrep_sync_wait &
          WSREP_SYNC_WAIT_BEFORE_READ;
  return false;
}

static int wsrep_start_position_verify (const char* start_str)
{
  size_t        start_len;
  wsrep_uuid_t  uuid;
  ssize_t       uuid_len;

  start_len = strlen (start_str);
  if (start_len < 34)
    return 1;

  uuid_len = wsrep_uuid_scan (start_str, start_len, &uuid);
  if (uuid_len < 0 || (start_len - uuid_len) < 2)
    return 1;

  if (start_str[uuid_len] != ':') // separator should follow UUID
    return 1;

  char* endptr;
  wsrep_seqno_t const seqno __attribute__((unused)) // to avoid GCC warnings
    (strtoll(&start_str[uuid_len + 1], &endptr, 10));

  if (*endptr == '\0') return 0; // remaining string was seqno

  return 1;
}

/* Function checks if the new value for start_position is valid.
@return false if no error encountered with check else return true. */
bool wsrep_start_position_check (sys_var *self, THD* thd, set_var* var)
{
  char start_pos_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(start_pos_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  start_pos_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_start_position_verify(start_pos_buf)) return false;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return true;
}

static
void wsrep_set_local_position(const char* const value, bool const sst)
{
  size_t const value_len = strlen(value);
  wsrep_uuid_t uuid;
  size_t const uuid_len = wsrep_uuid_scan(value, value_len, &uuid);
  wsrep_seqno_t const seqno = strtoll(value + uuid_len + 1, NULL, 10);

  if (sst) {
    wsrep_sst_received (wsrep, uuid, seqno, NULL, 0);
  } else {
    // initialization
    local_uuid = uuid;
    local_seqno = seqno;
  }
}

bool wsrep_start_position_update (sys_var *self, THD* thd, enum_var_type type)
{
  WSREP_INFO ("wsrep_start_position var submitted: '%s'",
              wsrep_start_position);
  // since this value passed wsrep_start_position_check, don't check anything
  // here
  wsrep_set_local_position (wsrep_start_position, true);
  return 0;
}

void wsrep_start_position_init (const char* val)
{
  if (NULL == val || wsrep_start_position_verify (val))
  {
    WSREP_ERROR("Bad initial value for wsrep_start_position: %s", 
                (val ? val : ""));
    return;
  }

  wsrep_set_local_position (val, false);
}

static int get_provider_option_value(const char* opts,
                                     const char* opt_name,
                                     ulong* opt_value)
{
  int ret= 1;
  ulong opt_value_tmp;
  char *opt_value_str, *s, *opts_copy= my_strdup(opts, MYF(MY_WME));

  if ((opt_value_str= strstr(opts_copy, opt_name)) == NULL)
    goto end;
  opt_value_str= strtok_r(opt_value_str, "=", &s);
  if (opt_value_str == NULL) goto end;
  opt_value_str= strtok_r(NULL, ";", &s);
  if (opt_value_str == NULL) goto end;

  opt_value_tmp= strtoul(opt_value_str, NULL, 10);
  if (errno == ERANGE) goto end;

  *opt_value= opt_value_tmp;
  ret= 0;

end:
  my_free(opts_copy);
  return ret;
}

static bool refresh_provider_options()
{
  WSREP_DEBUG("refresh_provider_options: %s", 
              (wsrep_provider_options) ? wsrep_provider_options : "null");
  char* opts= wsrep->options_get(wsrep);
  if (opts)
  {
    wsrep_provider_options_init(opts);
    get_provider_option_value(wsrep_provider_options,
                              (char*)"repl.max_ws_size",
                              &wsrep_max_ws_size);
    free(opts);
  }
  else
  {
    WSREP_ERROR("Failed to get provider options");
    return true;
  }
  return false;
}

static int wsrep_provider_verify (const char* provider_str)
{
  MY_STAT   f_stat;
  char path[FN_REFLEN];

  if (!provider_str || strlen(provider_str)== 0)
    return 1;

  if (!strcmp(provider_str, WSREP_NONE))
    return 0;

  if (!unpack_filename(path, provider_str))
    return 1;

  /* check that provider file exists */
  memset(&f_stat, 0, sizeof(MY_STAT));
  if (!my_stat(path, &f_stat, MYF(0)))
  {
    return 1;
  }
  return 0;
}

/* Function checks if the new value for provider is valid.
@return false if no error encountered with check else return true. */
bool wsrep_provider_check (sys_var *self, THD* thd, set_var* var)
{
  char wsrep_provider_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  /* Changing wsrep_provider in middle of function/trigger is not allowed. */
  if (thd->in_sub_stmt)
  {
    WSREP_WARN("Cannot modify wsrep_provider inside a stored function "
              " or trigger");
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             var->save_result.string_value.str);
    return true;
   }

  /* Changing wsrep_provider in middle of transaction is not allowed. */
  if (thd->in_active_multi_stmt_transaction())
  {
    WSREP_WARN("Cannot modify wsrep_provider inside a transaction");
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             var->save_result.string_value.str);
    return true;
  }

  memcpy(wsrep_provider_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  wsrep_provider_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_provider_verify(wsrep_provider_buf)) return false;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return true;
}

bool wsrep_provider_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool rcode= false;

  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  /* Ensure we free the stats that are allocated by galera-library.
  wsrep part doesn't know how to free them shouldn't be attempting it
  as it not allocated by wsrep part.
  Before unloading cleanup any stale reference and stats is one of it.
  Given that thd->variables.wsrep.on is turned-off it is safe to assume
  that no new stats queries will be allowed during this transition time. */
  wsrep_free_status(thd);

  WSREP_DEBUG("wsrep_provider_update: %s", wsrep_provider);

  /* stop replication is heavy operation, and includes closing all client 
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.

     Note: releasing LOCK_global_system_variables may cause race condition, if 
     there can be several concurrent clients changing wsrep_provider
  */
  mysql_mutex_unlock(&LOCK_global_system_variables);
  wsrep_stop_replication(thd);

  /*
    Unlock and lock LOCK_wsrep_slave_threads to maintain lock order & avoid
    any potential deadlock.
  */
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  mysql_mutex_lock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);

#if 0
  /* provider status variables are allocated in provider library
     and need to freed here, otherwise a dangling reference to
     wsrep_status_vars would remain in THD
  */
  wsrep_free_status(thd);
#endif

  wsrep_deinit();

  char* tmp= strdup(wsrep_provider); // wsrep_init() rewrites provider 
                                     //when fails
  if (wsrep_init())
  {
    my_error(ER_CANT_OPEN_LIBRARY, MYF(0), tmp);
    rcode = true;
  }
  free(tmp);

  // we sure don't want to use old address with new provider
  wsrep_cluster_address_init(NULL);
  wsrep_provider_options_init(NULL);

  thd->variables.wsrep_on= wsrep_on_saved;

  refresh_provider_options();

  return rcode;
}

void wsrep_provider_init (const char* value)
{
  WSREP_DEBUG("wsrep_provider_init: %s -> %s", 
              (wsrep_provider) ? wsrep_provider : "null", 
              (value) ? value : "null");
  if (NULL == value || wsrep_provider_verify (value))
  {
    WSREP_ERROR("Bad initial value for wsrep_provider: %s",
                (value ? value : ""));
    return;
  }

  if (wsrep_provider) my_free((void *)wsrep_provider);
  wsrep_provider = my_strdup(value, MYF(0));
}

bool wsrep_provider_options_check (sys_var *self, THD* thd, set_var* var)
{
  return 0;
}

bool wsrep_provider_options_update(sys_var *self, THD* thd, enum_var_type type)
{
  if (wsrep == NULL)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }

  wsrep_status_t ret= wsrep->options_set(wsrep, wsrep_provider_options);
  if (ret != WSREP_OK)
  {
    WSREP_ERROR("Set options returned %d", ret);
    refresh_provider_options();
    return true;
  }
  return refresh_provider_options();
}

void wsrep_provider_options_init(const char* value)
{
  if (wsrep_provider_options && wsrep_provider_options != value)
    my_free((void *)wsrep_provider_options);
  wsrep_provider_options = (value) ? my_strdup(value, MYF(0)) : NULL;
}

bool wsrep_reject_queries_update(sys_var *self, THD* thd, enum_var_type type)
{
    switch (wsrep_reject_queries) {
        case WSREP_REJECT_NONE:
            WSREP_INFO("Allowing client queries due to manual setting");
            break;
        case WSREP_REJECT_ALL:
            WSREP_INFO("Rejecting client queries due to manual setting");
            break;
        case WSREP_REJECT_ALL_KILL:
            wsrep_close_client_connections(FALSE);
            WSREP_INFO("Rejecting client queries and killing connections due to manual setting");
            break;
        default:
          WSREP_INFO("Unknown value for wsrep_reject_queries: %lu",
                     wsrep_reject_queries);
            return true;
    }
    return false;
}

static int wsrep_cluster_address_verify (const char* cluster_address_str)
{
  /* There is no predefined address format, it depends on provider. */
  return 0;

}

/* Function checks if the new value for cluster_address is valid.
@return false if no error encountered with check else return true. */
bool wsrep_cluster_address_check (sys_var *self, THD* thd, set_var* var)
{
  char addr_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(addr_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  addr_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_cluster_address_verify(addr_buf)) return false;

 err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return true;
}

bool wsrep_cluster_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  /* stop replication is heavy operation, and includes closing all client 
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.

     Note: releasing LOCK_global_system_variables may cause race condition, if 
     there can be several concurrent clients changing wsrep_provider
  */
  mysql_mutex_unlock(&LOCK_global_system_variables);
  wsrep_stop_replication(thd);
  /*
    Unlock and lock LOCK_wsrep_slave_threads to maintain lock order & avoid
    any potential deadlock.
  */
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  mysql_mutex_lock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);

  if (wsrep_start_replication())
  {
    wsrep_create_rollbacker();
    wsrep_create_appliers(wsrep_slave_threads);
  }

  thd->variables.wsrep_on= wsrep_on_saved;

  return false;
}

void wsrep_cluster_address_init (const char* value)
{
  WSREP_DEBUG("wsrep_cluster_address_init: %s -> %s", 
              (wsrep_cluster_address) ? wsrep_cluster_address : "null", 
              (value) ? value : "null");

  if (wsrep_cluster_address) my_free ((void*)wsrep_cluster_address);
  wsrep_cluster_address = (value) ? my_strdup(value, MYF(0)) : NULL;
}

/* Function checks if the new value for cluster_name is valid.
@return false if no error encountered with check else return true. */
bool wsrep_cluster_name_check (sys_var *self, THD* thd, set_var* var)
{
  if (!var->save_result.string_value.str ||
      (var->save_result.string_value.length == 0) ||
      (var->save_result.string_value.length > WSREP_CLUSTER_NAME_MAX_LEN))
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             (var->save_result.string_value.str ?
              var->save_result.string_value.str : "NULL"));
    return true;
  }
  return false;
}

bool wsrep_cluster_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

/* Function checks if the new value for node_name is valid.
@return false if no error encountered with check else return true. */
bool wsrep_node_name_check (sys_var *self, THD* thd, set_var* var)
{
  // TODO: for now 'allow' 0-length string to be valid (default)
  if (!var->save_result.string_value.str)
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             (var->save_result.string_value.str ?
              var->save_result.string_value.str : "NULL"));
    return true;
  }
  return false;
}

bool wsrep_node_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

/* Function checks if the new value for node_address is valid.
TODO: do something more elaborate, like checking connectivity
@return false if no error encountered with check else return true. */
bool wsrep_node_address_check (sys_var *self, THD* thd, set_var* var)
{
  char addr_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(addr_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  addr_buf[var->save_result.string_value.length]= 0;

  // TODO: for now 'allow' 0-length string to be valid (default)
  return false;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return true;
}

bool wsrep_node_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

void wsrep_node_address_init (const char* value)
{
  if (wsrep_node_address && strcmp(wsrep_node_address, value))
    my_free ((void*)wsrep_node_address);

  wsrep_node_address = (value) ? my_strdup(value, MYF(0)) : NULL;
}

static void wsrep_slave_count_change_update ()
{
  // wsrep_running_threads = appliers threads + 1 rollbacker thread
  wsrep_slave_count_change = (wsrep_slave_threads - wsrep_running_threads + 1);
}

bool wsrep_slave_threads_update (sys_var *self, THD* thd, enum_var_type type)
{
  wsrep_slave_count_change_update();
  if (wsrep_slave_count_change > 0)
  {
    WSREP_DEBUG("Creating %d applier threads, total %ld", wsrep_slave_count_change, wsrep_slave_threads);
    wsrep_create_appliers(wsrep_slave_count_change);
    wsrep_slave_count_change = 0;
  }
  return false;
}

bool wsrep_desync_check (sys_var *self, THD* thd, set_var* var)
{
  bool new_wsrep_desync = var->save_result.ulonglong_value;
  if (wsrep_desync == new_wsrep_desync) {
    if (new_wsrep_desync) {
      push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "'wsrep_desync' is already ON.");
    } else {
      push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "'wsrep_desync' is already OFF.");
    }
    return false;
  }

  /* Setting desync to on/off signals participation of node in flow control.
  It doesn't turn-off recieval of write-sets.
  If the node is already in paused state due to some previous action like FTWRL
  then avoid desync as superset action of pausing the node is already active. */

  if (!thd->global_read_lock.provider_resumed())
  {
    char message[1024];
    sprintf(message,
            "Explictly desync/resync of already desynced/paused node"
            " is prohibited");
    my_message(ER_UNKNOWN_ERROR, message, MYF(0));
    return true;
  }

  wsrep_status_t ret(WSREP_WARNING);
  if (new_wsrep_desync) {
    ret = wsrep->desync (wsrep);
    if (ret != WSREP_OK) {
      WSREP_WARN ("SET desync failed %d for schema: %s, query: %s", ret,
                  (thd->db ? thd->db : "(null)"), WSREP_QUERY(thd));
      my_error (ER_CANNOT_USER, MYF(0), "'desync'", thd->query());
      return true;
    }
  } else {
    ret = wsrep->resync (wsrep);
    if (ret != WSREP_OK) {
      WSREP_WARN ("SET resync failed %d for schema: %s, query: %s", ret,
                  (thd->db ? thd->db : "(null)"), WSREP_QUERY(thd));
      my_error (ER_CANNOT_USER, MYF(0), "'resync'", thd->query());
      return true;
    }
  }
  return false;
}

bool wsrep_desync_update (sys_var *self, THD* thd, enum_var_type type)
{
  return false;
}

bool wsrep_max_ws_size_update (sys_var *self, THD *thd, enum_var_type)
{
  char max_ws_size_opt[128];
  my_snprintf(max_ws_size_opt, sizeof(max_ws_size_opt),
              "repl.max_ws_size=%d", wsrep_max_ws_size);
  wsrep_status_t ret= wsrep->options_set(wsrep, max_ws_size_opt);
  if (ret != WSREP_OK)
  {
    WSREP_ERROR("Set options returned %d", ret);
    refresh_provider_options();
    return true;
  }
  return refresh_provider_options();
}

/*
 * Status variables stuff below
 */
static inline void
wsrep_assign_to_mysql (SHOW_VAR* mysql, wsrep_stats_var* wsrep)
{
  mysql->name = wsrep->name;
  switch (wsrep->type) {
  case WSREP_VAR_INT64:
    mysql->value = (char*) &wsrep->value._int64;
    mysql->type  = SHOW_LONGLONG;
    break;
  case WSREP_VAR_STRING:
    mysql->value = (char*) &wsrep->value._string;
    mysql->type  = SHOW_CHAR_PTR;
    break;
  case WSREP_VAR_DOUBLE:
    mysql->value = (char*) &wsrep->value._double;
    mysql->type  = SHOW_DOUBLE;
    break;
  }
}

#if DYNAMIC
// somehow this mysql status thing works only with statically allocated arrays.
static SHOW_VAR*          mysql_status_vars = NULL;
static int                mysql_status_len  = -1;
#else
static SHOW_VAR           mysql_status_vars[512 + 1];
static const int          mysql_status_len  = 512;
#endif

static void export_wsrep_status_to_mysql(THD* thd)
{
  int wsrep_status_len, i;

  /* Avoid freeing the stats immediately on completion of the call
  instead free them on next invocation as the reference to status
  is feeded in MySQL show status array. Freeing them on completion
  will make these references invalid. */
  wsrep_free_status(thd);

  thd->wsrep_status_vars = wsrep->stats_get(wsrep);

  if (!thd->wsrep_status_vars) {
    return;
  }

  for (wsrep_status_len = 0;
       thd->wsrep_status_vars[wsrep_status_len].name != NULL;
       wsrep_status_len++) {
      /* */
  }

#if DYNAMIC
  if (wsrep_status_len != mysql_status_len) {
    void* tmp = realloc (mysql_status_vars,
                         (wsrep_status_len + 1) * sizeof(SHOW_VAR));
    if (!tmp) {

      sql_print_error ("Out of memory for wsrep status variables."
                       "Number of variables: %d", wsrep_status_len);
      return;
    }

    mysql_status_len  = wsrep_status_len;
    mysql_status_vars = (SHOW_VAR*)tmp;
  }
  /* @TODO: fix this: */
#else
  if (mysql_status_len < wsrep_status_len) wsrep_status_len= mysql_status_len;
#endif

  for (i = 0; i < wsrep_status_len; i++)
    wsrep_assign_to_mysql (mysql_status_vars + i, thd->wsrep_status_vars + i);

  mysql_status_vars[wsrep_status_len].name  = NullS;
  mysql_status_vars[wsrep_status_len].value = NullS;
  mysql_status_vars[wsrep_status_len].type  = SHOW_LONG;
}

int wsrep_show_status (THD *thd, SHOW_VAR *var, char *buff)
{
  export_wsrep_status_to_mysql(thd);
  var->type= SHOW_ARRAY;
  var->value= (char *) &mysql_status_vars;
  return 0;
}

void wsrep_free_status (THD* thd)
{
  if (thd->wsrep_status_vars)
  {
    wsrep->stats_free (wsrep, thd->wsrep_status_vars);
    thd->wsrep_status_vars = 0;
  }
}
