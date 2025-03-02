/* Copyright (c) 2005, 2016, Oracle and/or its affiliates. All rights reserved.

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


#define MYSQL_SERVER 1
#include "sql_priv.h"
#include "unireg.h"
#include "probes_mysql.h"
#include "ha_blackhole.h"
#include "sql_class.h"                          // THD, SYSTEM_THREAD_SLAVE_*
#include "rpl_rli.h" // THD::rli_slave::rows_query_ev

static inline bool is_slave_applier(const THD &thd)
{
  return thd.system_thread == SYSTEM_THREAD_SLAVE_SQL ||
    thd.system_thread == SYSTEM_THREAD_SLAVE_WORKER;
}

static inline bool pretend_for_slave(const THD &thd)
{
  return is_slave_applier(thd) &&
    ((thd.rli_slave && thd.rli_slave->rows_query_ev) || thd.query() == NULL);
}

/* Static declarations for handlerton */

static handler *blackhole_create_handler(handlerton *hton,
                                         TABLE_SHARE *table,
                                         MEM_ROOT *mem_root)
{
  return new (mem_root) ha_blackhole(hton, table);
}


/* Static declarations for shared structures */

static mysql_mutex_t blackhole_mutex;
static HASH blackhole_open_tables;

static st_blackhole_share *get_share(const char *table_name);
static void free_share(st_blackhole_share *share);

/*****************************************************************************
** BLACKHOLE tables
*****************************************************************************/

ha_blackhole::ha_blackhole(handlerton *hton,
                           TABLE_SHARE *table_arg)
  :handler(hton, table_arg)
{}


static const char *ha_blackhole_exts[] = {
  NullS
};

const char **ha_blackhole::bas_ext() const
{
  return ha_blackhole_exts;
}

int ha_blackhole::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_blackhole::open");

  if (!(share= get_share(name)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  thr_lock_data_init(&share->lock, &lock, NULL);
  DBUG_RETURN(0);
}

int ha_blackhole::close(void)
{
  DBUG_ENTER("ha_blackhole::close");
  free_share(share);
  DBUG_RETURN(0);
}

int ha_blackhole::create(const char *name, TABLE *table_arg,
                         HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_blackhole::create");
  DBUG_RETURN(0);
}

/*
  Intended to support partitioning.
  Allows a particular partition to be truncated.
*/
int ha_blackhole::truncate()
{
  DBUG_ENTER("ha_blackhole::truncate");
  DBUG_RETURN(0);
}

const char *ha_blackhole::index_type(uint key_number)
{
  DBUG_ENTER("ha_blackhole::index_type");
  DBUG_RETURN((table_share->key_info[key_number].flags & HA_FULLTEXT) ? 
              "FULLTEXT" :
              (table_share->key_info[key_number].flags & HA_SPATIAL) ?
              "SPATIAL" :
              (table_share->key_info[key_number].algorithm ==
               HA_KEY_ALG_RTREE) ? "RTREE" : "BTREE");
}

int ha_blackhole::write_row(uchar * buf)
{
  DBUG_ENTER("ha_blackhole::write_row");
  DBUG_RETURN(table->next_number_field ? update_auto_increment() : 0);
}

int ha_blackhole::update_row(const uchar *old_data, uchar *new_data)
{
  DBUG_ENTER("ha_blackhole::update_row");
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
    DBUG_RETURN(0);
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_blackhole::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_blackhole::delete_row");
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
    DBUG_RETURN(0);
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_blackhole::rnd_init(bool scan)
{
  DBUG_ENTER("ha_blackhole::rnd_init");
  DBUG_RETURN(0);
}


int ha_blackhole::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_blackhole::rnd_next");
  MYSQL_READ_ROW_START(table_share->db.str, table_share->table_name.str,
                       TRUE);
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
  {
    /*
      Unlike a normal storage engine (e.g. 'InnoDB') in which
      'rnd_next()' overload actually reads all data in BLOB fields
      from real storage into internal SE memory (like 'mem_heap' in InnoDB)
      and updates packed representation ('table->record[0]') of the BLOB
      field values with pointers to this internal SE memory, 'Blackhole'
      engine does not do this.

      In case when a database with Blackhole tables serves just as an
      intermediate binlog server in a replication chain, this may cause
      data corruption.

      In particular, when 'Update_rows_log_event' is processed on a
      Blackhole table, calling 'Update_rows_log_event::do_exec_row()' will
      first copy Before Image (BI) found in 'record[0]' into 'record[1]' and
      then unpack After Image (AI) into 'record[0]'. The problem is that this
      record copying is shallow (just 'memcpy()') and for packed BLOB fields
      it just copies pointer values. In other words, before calling
      'unpack_current_row()' we end up in a situation when packed BLOB field
      values in 'record[0]' (AI) and 'record[1]' (BI) point to exactly the
      same memory location and calling 'unpack_current_row()' for AI
      overwrites BLOB data in BI.

      To prevent this we do the same trick as for virtual generated columns
      in 5.7 - keeping old BLOB value inside 'old_value' field in
      'Field_blob' class by calling 'keep_old_value()' for all BLOB fields
      currently marked for update in 'table->write_set'..
    */

    if (table_share->blob_fields != 0)
      for (Field **field_ptr= table->field; *field_ptr != NULL; ++field_ptr)
      {
        Field *current_field= *field_ptr;
        if ((current_field->flags & BLOB_FLAG) != 0)
        {
          Field_blob* bfield= static_cast<Field_blob*>(current_field);
          if (bfield->in_write_set())
            bfield->keep_old_value();
        }
      }

    rc= 0;
  }
  else
    rc= HA_ERR_END_OF_FILE;
  MYSQL_READ_ROW_DONE(rc);
  table->status= rc ? STATUS_NOT_FOUND : 0;
  DBUG_RETURN(rc);
}


int ha_blackhole::rnd_pos(uchar * buf, uchar *pos)
{
  DBUG_ENTER("ha_blackhole::rnd_pos");
  MYSQL_READ_ROW_START(table_share->db.str, table_share->table_name.str,
                       FALSE);
  DBUG_ASSERT(0);
  MYSQL_READ_ROW_DONE(0);
  DBUG_RETURN(0);
}


void ha_blackhole::position(const uchar *record)
{
  DBUG_ENTER("ha_blackhole::position");
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}


int ha_blackhole::info(uint flag)
{
  DBUG_ENTER("ha_blackhole::info");

  memset(static_cast<void*>(&stats), 0, sizeof(stats));
  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= 1;
  DBUG_RETURN(0);
}

int ha_blackhole::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_blackhole::external_lock");
  DBUG_RETURN(0);
}


THR_LOCK_DATA **ha_blackhole::store_lock(THD *thd,
                                         THR_LOCK_DATA **to,
                                         enum thr_lock_type lock_type)
{
  DBUG_ENTER("ha_blackhole::store_lock");
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
  {
    /*
      Here is where we get into the guts of a row level lock.
      If TL_UNLOCK is set
      If we are not doing a LOCK TABLE or DISCARD/IMPORT
      TABLESPACE, then allow multiple writers
    */

    if ((lock_type >= TL_WRITE_CONCURRENT_INSERT &&
         lock_type <= TL_WRITE) && !thd_in_lock_tables(thd)
        && !thd_tablespace_op(thd))
      lock_type = TL_WRITE_ALLOW_WRITE;

    /*
      In queries of type INSERT INTO t1 SELECT ... FROM t2 ...
      MySQL would use the lock TL_READ_NO_INSERT on t2, and that
      would conflict with TL_WRITE_ALLOW_WRITE, blocking all inserts
      to t2. Convert the lock to a normal read lock to allow
      concurrent inserts to t2.
    */

    if (lock_type == TL_READ_NO_INSERT && !thd_in_lock_tables(thd))
      lock_type = TL_READ;

    lock.type= lock_type;
  }
  *to++= &lock;
  DBUG_RETURN(to);
}


int ha_blackhole::index_read_map(uchar * buf, const uchar * key,
                                 key_part_map keypart_map,
                             enum ha_rkey_function find_flag)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_read");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
    rc= 0;
  else
    rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= rc ? STATUS_NOT_FOUND : 0;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_read_idx_map(uchar * buf, uint idx, const uchar * key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_read_idx");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
    rc= 0;
  else
    rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= rc ? STATUS_NOT_FOUND : 0;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_read_last_map(uchar * buf, const uchar * key,
                                      key_part_map keypart_map)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_read_last");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  THD *thd= ha_thd();
  if (pretend_for_slave(*thd))
    rc= 0;
  else
    rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= rc ? STATUS_NOT_FOUND : 0;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_next(uchar * buf)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_next");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_prev(uchar * buf)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_prev");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_first(uchar * buf)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_first");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
}


int ha_blackhole::index_last(uchar * buf)
{
  int rc;
  DBUG_ENTER("ha_blackhole::index_last");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_END_OF_FILE;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  table->status= STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
}


static st_blackhole_share *get_share(const char *table_name)
{
  st_blackhole_share *share;
  uint length;

  length= (uint) strlen(table_name);
  mysql_mutex_lock(&blackhole_mutex);
    
  if (!(share= (st_blackhole_share*)
        my_hash_search(&blackhole_open_tables,
                       (uchar*) table_name, length)))
  {
    if (!(share= (st_blackhole_share*) my_malloc(sizeof(st_blackhole_share) +
                                                 length,
                                                 MYF(MY_WME | MY_ZEROFILL))))
      goto error;

    share->table_name_length= length;
    strmov(share->table_name, table_name);
    
    if (my_hash_insert(&blackhole_open_tables, (uchar*) share))
    {
      my_free(share);
      share= NULL;
      goto error;
    }
    
    thr_lock_init(&share->lock);
  }
  share->use_count++;
  
error:
  mysql_mutex_unlock(&blackhole_mutex);
  return share;
}

static void free_share(st_blackhole_share *share)
{
  mysql_mutex_lock(&blackhole_mutex);
  if (!--share->use_count)
    my_hash_delete(&blackhole_open_tables, (uchar*) share);
  mysql_mutex_unlock(&blackhole_mutex);
}

static void blackhole_free_key(st_blackhole_share *share)
{
  thr_lock_delete(&share->lock);
  my_free(share);
}

static uchar* blackhole_get_key(st_blackhole_share *share, size_t *length,
                                my_bool not_used MY_ATTRIBUTE((unused)))
{
  *length= share->table_name_length;
  return (uchar*) share->table_name;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key bh_key_mutex_blackhole;

static PSI_mutex_info all_blackhole_mutexes[]=
{
  { &bh_key_mutex_blackhole, "blackhole", PSI_FLAG_GLOBAL}
};

void init_blackhole_psi_keys()
{
  const char* category= "blackhole";
  int count;

  count= array_elements(all_blackhole_mutexes);
  mysql_mutex_register(category, all_blackhole_mutexes, count);
}
#endif

static int blackhole_init(void *p)
{
  handlerton *blackhole_hton;

#ifdef HAVE_PSI_INTERFACE
  init_blackhole_psi_keys();
#endif

  blackhole_hton= (handlerton *)p;
  blackhole_hton->state= SHOW_OPTION_YES;
  blackhole_hton->db_type= DB_TYPE_BLACKHOLE_DB;
  blackhole_hton->create= blackhole_create_handler;
  blackhole_hton->flags= HTON_CAN_RECREATE | HTON_SUPPORTS_ONLINE_BACKUPS;

  mysql_mutex_init(bh_key_mutex_blackhole,
                   &blackhole_mutex, MY_MUTEX_INIT_FAST);
  (void) my_hash_init(&blackhole_open_tables, system_charset_info,32,0,0,
                      (my_hash_get_key) blackhole_get_key,
                      (my_hash_free_key) blackhole_free_key, 0);

  return 0;
}

static int blackhole_fini(void *p)
{
  my_hash_free(&blackhole_open_tables);
  mysql_mutex_destroy(&blackhole_mutex);

  return 0;
}

struct st_mysql_storage_engine blackhole_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

mysql_declare_plugin(blackhole)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &blackhole_storage_engine,
  "BLACKHOLE",
  "MySQL AB",
  "/dev/null storage engine (anything you write to it disappears)",
  PLUGIN_LICENSE_GPL,
  blackhole_init, /* Plugin Init */
  blackhole_fini, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL,                       /* config options                  */
  0,                          /* flags                           */
}
mysql_declare_plugin_end;
