#ifndef SET_VAR_INCLUDED
#define SET_VAR_INCLUDED
/* Copyright (c) 2002, 2013, Oracle and/or its affiliates. All rights reserved.

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

/**
  @file
  "public" interface to sys_var - server configuration variables.
*/

#include <my_getopt.h>
#include <vector>

class sys_var;
class set_var;
class sys_var_pluginvar;
class PolyLock;
class Item_func_set_user_var;

// This include needs to be here since item.h requires enum_var_type :-P
#include "item.h"                          /* Item */
#include "sql_class.h"                     /* THD  */

extern TYPELIB bool_typelib;

struct sys_var_chain
{
  sys_var *first;
  sys_var *last;
};

int mysql_add_sys_var_chain(sys_var *chain);
int mysql_del_sys_var_chain(sys_var *chain);

/**
  A class representing one system variable - that is something
  that can be accessed as @@global.variable_name or @@session.variable_name,
  visible in SHOW xxx VARIABLES and in INFORMATION_SCHEMA.xxx_VARIABLES,
  optionally it can be assigned to, optionally it can have a command-line
  counterpart with the same name.
*/
class sys_var
{
public:
  sys_var *next;
  LEX_CSTRING name;
  enum flag_enum { GLOBAL, SESSION, ONLY_SESSION, SCOPE_MASK=1023,
                   READONLY=1024, ALLOCATED=2048, INVISIBLE=4096 };
  static const int PARSE_EARLY= 1;
  static const int PARSE_NORMAL= 2;
  /**
    Enumeration type to indicate for a system variable whether
    it will be written to the binlog or not.
  */    
  enum binlog_status_enum { VARIABLE_NOT_IN_BINLOG,
                            SESSION_VARIABLE_IN_BINLOG } binlog_status;

protected:
  typedef bool (*on_check_function)(sys_var *self, THD *thd, set_var *var);
  typedef bool (*on_update_function)(sys_var *self, THD *thd, enum_var_type type);

  int flags;            ///< or'ed flag_enum values
  int m_parse_flag;     ///< either PARSE_EARLY or PARSE_NORMAL.
  const SHOW_TYPE show_val_type; ///< what value_ptr() returns for sql_show.cc
  my_option option;     ///< min, max, default values are stored here
  PolyLock *guard;      ///< *second* lock that protects the variable
  ptrdiff_t offset;     ///< offset to the value from global_system_variables
  on_check_function on_check;
  on_update_function on_update;
  const char *const deprecation_substitute;
  bool is_os_charset; ///< true if the value is in character_set_filesystem

public:
  sys_var(sys_var_chain *chain, const char *name_arg, const char *comment,
          int flag_args, ptrdiff_t off, int getopt_id,
          enum get_opt_arg_type getopt_arg_type, SHOW_TYPE show_val_type_arg,
          longlong def_val, PolyLock *lock, enum binlog_status_enum binlog_status_arg,
          on_check_function on_check_func, on_update_function on_update_func,
          const char *substitute, int parse_flag);

  virtual ~sys_var() {}

  /**
    All the cleanup procedures should be performed here
  */
  virtual void cleanup() {}
  /**
    downcast for sys_var_pluginvar. Returns this if it's an instance
    of sys_var_pluginvar, and 0 otherwise.
  */
  virtual sys_var_pluginvar *cast_pluginvar() { return 0; }

  bool check(THD *thd, set_var *var);
  uchar *value_ptr(THD *thd, enum_var_type type, LEX_STRING *base);
  virtual void update_default(longlong new_def_value)
  { option.def_value= new_def_value; }

  /**
     Update the system variable with the default value from either
     session or global scope.  The default value is stored in the
     'var' argument. Return false when successful.
  */
  bool set_default(THD *thd, set_var *var);
  bool update(THD *thd, set_var *var);
  void stmt_update(THD *thd) {
    on_update && on_update(this, thd, OPT_SESSION);
  }

  SHOW_TYPE show_type() { return show_val_type; }
  int scope() const { return flags & SCOPE_MASK; }
  const CHARSET_INFO *charset(THD *thd);
  bool is_readonly() const 
  {
    const my_bool *readonly= getopt_constraint_get_readonly_value(option.name,
                                                                  0, FALSE);
    if (readonly && *readonly)
      return TRUE;  
    
    return flags & READONLY;
  }
  bool not_visible() const { return flags & INVISIBLE; }
  /**
    the following is only true for keycache variables,
    that support the syntax @@keycache_name.variable_name
  */
  bool is_struct() { return option.var_type & GET_ASK_ADDR; }
  bool is_written_to_binlog(enum_var_type type)
  { return type != OPT_GLOBAL && binlog_status == SESSION_VARIABLE_IN_BINLOG; }
  virtual bool check_update_type(Item_result type) = 0;
  bool check_type(enum_var_type type)
  {
    switch (scope())
    {
    case GLOBAL:       return type != OPT_GLOBAL;
    case SESSION:      return false; // always ok
    case ONLY_SESSION: return type == OPT_GLOBAL;
    }
    return true; // keep gcc happy
  }
  bool register_option(std::vector<my_option> *array, int parse_flags)
  {
    return (option.id != -1) && (m_parse_flag & parse_flags) &&
      (array->push_back(option), false);
  }
  void do_deprecated_warning(THD *thd);

private:
  virtual bool do_check(THD *thd, set_var *var) = 0;
  /**
    save the session default value of the variable in var
  */
  virtual void session_save_default(THD *thd, set_var *var) = 0;
  /**
    save the global default value of the variable in var
  */
  virtual void global_save_default(THD *thd, set_var *var) = 0;
  virtual bool session_update(THD *thd, set_var *var) = 0;
  virtual bool global_update(THD *thd, set_var *var) = 0;
protected:
  /**
    A pointer to a value of the variable for SHOW.
    It must be of show_val_type type (bool for SHOW_BOOL, int for SHOW_INT,
    longlong for SHOW_LONGLONG, etc).
  */
  virtual uchar *session_value_ptr(THD *thd, LEX_STRING *base);
  virtual uchar *global_value_ptr(THD *thd, LEX_STRING *base);

  /**
    A pointer to a storage area of the variable, to the raw data.
    Typically it's the same as session_value_ptr(), but it's different,
    for example, for ENUM, that is printed as a string, but stored as a number.
  */
  uchar *session_var_ptr(THD *thd)
  { return ((uchar*)&(thd->variables)) + offset; }

  uchar *global_var_ptr()
  { return ((uchar*)&global_system_variables) + offset; }
};

#include "binlog.h"                           /* binlog_format_typelib */
#include "sql_plugin.h"                    /* SHOW_HA_ROWS, SHOW_MY_BOOL */

/****************************************************************************
  Classes for parsing of the SET command
****************************************************************************/

/**
  A base class for everything that can be set with SET command.
  It's similar to Items, an instance of this is created by the parser
  for every assigmnent in SET (or elsewhere, e.g. in SELECT).
*/
class set_var_base :public Sql_alloc
{
public:
  set_var_base() {}
  virtual ~set_var_base() {}
  virtual int check(THD *thd)=0;           /* To check privileges etc. */
  virtual int update(THD *thd)=0;                  /* To set the value */
  virtual int light_check(THD *thd) { return check(thd); }   /* for PS */
  virtual void print(THD *thd, String *str)=0;	/* To self-print */
  /// @returns whether this variable is @@@@optimizer_trace.
  virtual bool is_var_optimizer_trace() const { return false; }
};


/**
  set_var_base descendant for assignments to the system variables.
*/
class set_var :public set_var_base
{
public:
  sys_var *var; ///< system variable to be updated
  Item *value;  ///< the expression that provides the new value of the variable
  enum_var_type type;
  union ///< temp storage to hold a value between sys_var::check and ::update
  {
    ulonglong ulonglong_value;          ///< for all integer, set, enum sysvars
    double double_value;                ///< for Sys_var_double
    plugin_ref plugin;                  ///< for Sys_var_plugin
    Time_zone *time_zone;               ///< for Sys_var_tz
    LEX_STRING string_value;            ///< for Sys_var_charptr and others
    const void *ptr;                    ///< for Sys_var_struct
  } save_result;
  LEX_STRING base; /**< for structured variables, like keycache_name.variable_name */

  set_var(enum_var_type type_arg, sys_var *var_arg,
          const LEX_STRING *base_name_arg, Item *value_arg)
    :var(var_arg), type(type_arg), base(*base_name_arg)
  {
    /*
      If the set value is a field, change it to a string to allow things like
      SET table_type=MYISAM;
    */
    if (value_arg && value_arg->type() == Item::FIELD_ITEM)
    {
      Item_field *item= (Item_field*) value_arg;
      if (item->field_name)
      {
        if (!(value= new Item_string(item->field_name,
                                     (uint) strlen(item->field_name),
                                     system_charset_info))) // names are utf8
	  value= value_arg;			/* Give error message later */
      }
      else
      {
        /* Both Item_field and Item_insert_value will return the type as
        Item::FIELD_ITEM. If the item->field_name is NULL, we assume the
        object to be Item_insert_value. */
        value= value_arg;
      }
    }
    else
      value= value_arg;
  }
  int check(THD *thd);
  int update(THD *thd);
  int light_check(THD *thd);
#ifdef WITH_WSREP
  int wsrep_store_variable(THD *thd);
#endif
  void print(THD *thd, String *str);	/* To self-print */
#ifdef OPTIMIZER_TRACE
  virtual bool is_var_optimizer_trace() const
  {
    extern sys_var *Sys_optimizer_trace_ptr;
    return var == Sys_optimizer_trace_ptr;
  }
#endif
};


/* User variables like @my_own_variable */
class set_var_user: public set_var_base
{
  Item_func_set_user_var *user_var_item;
public:
  set_var_user(Item_func_set_user_var *item)
    :user_var_item(item)
  {}
  int check(THD *thd);
  int update(THD *thd);
  int light_check(THD *thd);
  void print(THD *thd, String *str);	/* To self-print */
};

/* For SET PASSWORD */

class set_var_password: public set_var_base
{
  LEX_USER *user;
  char *password;
public:
  set_var_password(LEX_USER *user_arg,char *password_arg)
    :user(user_arg), password(password_arg)
  {}
  int check(THD *thd);
  int update(THD *thd);
  void print(THD *thd, String *str);	/* To self-print */
};


/* For SET NAMES and SET CHARACTER SET */

class set_var_collation_client: public set_var_base
{
  int   set_cs_flags;
  const CHARSET_INFO *character_set_client;
  const CHARSET_INFO *character_set_results;
  const CHARSET_INFO *collation_connection;
public:
  enum  set_cs_flags_enum { SET_CS_NAMES=1, SET_CS_DEFAULT=2, SET_CS_COLLATE=4 };
  set_var_collation_client(int set_cs_flags_arg,
                           const CHARSET_INFO *client_coll_arg,
                           const CHARSET_INFO *connection_coll_arg,
                           const CHARSET_INFO *result_coll_arg)
    :set_cs_flags(set_cs_flags_arg),
     character_set_client(client_coll_arg),
     character_set_results(result_coll_arg),
     collation_connection(connection_coll_arg)
  {}
  int check(THD *thd);
  int update(THD *thd);
  void print(THD *thd, String *str);	/* To self-print */
};


/* optional things, have_* variables */
extern SHOW_COMP_OPTION have_csv;
extern SHOW_COMP_OPTION have_ndbcluster, have_partitioning;
extern SHOW_COMP_OPTION have_profiling;

extern SHOW_COMP_OPTION have_ssl, have_symlink, have_dlopen;
extern SHOW_COMP_OPTION have_query_cache;
extern SHOW_COMP_OPTION have_geometry, have_rtree_keys;
extern SHOW_COMP_OPTION have_crypt;
extern SHOW_COMP_OPTION have_compress;
extern SHOW_COMP_OPTION have_statement_timeout;
extern SHOW_COMP_OPTION have_backup_locks;
extern SHOW_COMP_OPTION have_backup_safe_binlog_info;
extern SHOW_COMP_OPTION have_snapshot_cloning;
extern SHOW_COMP_OPTION have_tlsv1_2;
extern SHOW_COMP_OPTION have_elliptic_curve_crypto;

/*
  Prototypes for helper functions
*/

SHOW_VAR* enumerate_sys_vars(THD *thd, bool sorted, enum enum_var_type type);

sys_var *find_sys_var(THD *thd, const char *str, uint length=0);

MY_ATTRIBUTE((warn_unused_result))
int sql_set_variables(THD *thd, List<set_var_base> *var_list,
                      bool free_joins = true);

bool fix_delay_key_write(sys_var *self, THD *thd, enum_var_type type);

sql_mode_t expand_sql_mode(sql_mode_t sql_mode);
bool sql_mode_string_representation(THD *thd, sql_mode_t sql_mode, LEX_STRING *ls);

extern sys_var *Sys_autocommit_ptr;
extern sys_var *Sys_gtid_next_ptr;
extern sys_var *Sys_gtid_next_list_ptr;
extern sys_var *Sys_gtid_purged_ptr;

const CHARSET_INFO *get_old_charset_by_name(const char *old_name);

#ifdef WITH_WSREP
int sql_set_wsrep_variables(THD *thd, List<set_var_base> *var_list);
#endif
int sys_var_init();
int sys_var_add_options(std::vector<my_option> *long_options, int parse_flags);
void sys_var_end(void);

extern void init_log_slow_verbosity();
extern void init_slow_query_log_use_global_control();
extern void init_log_slow_sp_statements();

#endif

