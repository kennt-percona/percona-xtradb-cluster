/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.

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

/******************************************************************//**
@file dict/dict0mem.cc
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include "dict0mem.h"

#ifdef UNIV_NONINL
#include "dict0mem.ic"
#endif

#include "rem0rec.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0dict.h"
#include "fts0priv.h"
#include "ut0crc32.h"
#ifndef UNIV_HOTBACKUP
# include "ha_prototypes.h"	/* innobase_casedn_str(),
				innobase_get_lower_case_table_names */
# include "mysql_com.h"		/* NAME_LEN */
# include "lock0lock.h"
#endif /* !UNIV_HOTBACKUP */
#ifdef UNIV_BLOB_DEBUG
# include "ut0rbt.h"
#endif /* UNIV_BLOB_DEBUG */
#include <iostream>

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */

#ifdef UNIV_PFS_MUTEX
/* Key to register autoinc_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	autoinc_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/** An interger randomly initialized at startup used to make a temporary
table name as unique as possible. */
static ib_uint32_t	dict_temp_file_num;

/**********************************************************************//**
Creates a table memory object.
@return	own: table object */
UNIV_INTERN
dict_table_t*
dict_mem_table_create(
/*==================*/
	const char*	name,	/*!< in: table name */
	ulint		space,	/*!< in: space where the clustered index of
				the table is placed */
	ulint		n_cols,	/*!< in: number of columns */
	ulint		flags,	/*!< in: table flags */
	ulint		flags2)	/*!< in: table flags2 */
{
	dict_table_t*	table;
	mem_heap_t*	heap;

	ut_ad(name);
	ut_a(dict_tf_is_valid(flags));
	ut_a(!(flags2 & ~DICT_TF2_BIT_MASK));

	heap = mem_heap_create(DICT_HEAP_SIZE);

	table = static_cast<dict_table_t*>(
		mem_heap_zalloc(heap, sizeof(dict_table_t)));

	table->heap = heap;

	table->flags = (unsigned int) flags;
	table->flags2 = (unsigned int) flags2;
	table->name = static_cast<char*>(ut_malloc(strlen(name) + 1));
	memcpy(table->name, name, strlen(name) + 1);
	table->space = (unsigned int) space;
	table->n_cols = (unsigned int) (n_cols + DATA_N_SYS_COLS);

	table->cols = static_cast<dict_col_t*>(
		mem_heap_alloc(heap,
			       (n_cols + DATA_N_SYS_COLS)
			       * sizeof(dict_col_t)));

	ut_d(table->magic_n = DICT_TABLE_MAGIC_N);

	/* true means that the stats latch will be enabled -
	dict_table_stats_lock() will not be noop. */
	dict_table_stats_latch_create(table, true);

#ifndef UNIV_HOTBACKUP
	table->autoinc_lock = static_cast<ib_lock_t*>(
		mem_heap_alloc(heap, lock_get_size()));

	dict_table_autoinc_create_lazy(table);

	table->autoinc = 0;

	/* The number of transactions that are either waiting on the
	AUTOINC lock or have been granted the lock. */
	table->n_waiting_or_granted_auto_inc_locks = 0;

	/* If the table has an FTS index or we are in the process
	of building one, create the table->fts */
	if (dict_table_has_fts_index(table)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		table->fts = fts_create(table);
		table->fts->cache = fts_cache_create(table);
	} else {
		table->fts = NULL;
	}

	table->is_corrupt = FALSE;

#endif /* !UNIV_HOTBACKUP */

	new(&table->foreign_set) dict_foreign_set();
	new(&table->referenced_set) dict_foreign_set();

	return(table);
}

/****************************************************************//**
Free a table memory object. */
UNIV_INTERN
void
dict_mem_table_free(
/*================*/
	dict_table_t*	table)		/*!< in: table */
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_d(table->cached = FALSE);

        if (dict_table_has_fts_index(table)
            || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
            || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		if (table->fts) {
			if (table->cached) {
				fts_optimize_remove_table(table);
			}

			fts_free(table);
		}
	}
#ifndef UNIV_HOTBACKUP
	dict_table_autoinc_destroy(table);
#endif /* UNIV_HOTBACKUP */

	dict_table_stats_latch_destroy(table);

	table->foreign_set.~dict_foreign_set();
	table->referenced_set.~dict_foreign_set();

	ut_free(table->name);
	mem_heap_free(table->heap);
}

/****************************************************************//**
Append 'name' to 'col_names'.  @see dict_table_t::col_names
@return	new column names array */
static
const char*
dict_add_col_name(
/*==============*/
	const char*	col_names,	/*!< in: existing column names, or
					NULL */
	ulint		cols,		/*!< in: number of existing columns */
	const char*	name,		/*!< in: new column name */
	mem_heap_t*	heap)		/*!< in: heap */
{
	ulint	old_len;
	ulint	new_len;
	ulint	total_len;
	char*	res;

	ut_ad(!cols == !col_names);

	/* Find out length of existing array. */
	if (col_names) {
		const char*	s = col_names;
		ulint		i;

		for (i = 0; i < cols; i++) {
			s += strlen(s) + 1;
		}

		old_len = s - col_names;
	} else {
		old_len = 0;
	}

	new_len = strlen(name) + 1;
	total_len = old_len + new_len;

	res = static_cast<char*>(mem_heap_alloc(heap, total_len));

	if (old_len > 0) {
		memcpy(res, col_names, old_len);
	}

	memcpy(res + old_len, name, new_len);

	return(res);
}

/**********************************************************************//**
Adds a column definition to a table. */
UNIV_INTERN
void
dict_mem_table_add_col(
/*===================*/
	dict_table_t*	table,	/*!< in: table */
	mem_heap_t*	heap,	/*!< in: temporary memory heap, or NULL */
	const char*	name,	/*!< in: column name, or NULL */
	ulint		mtype,	/*!< in: main datatype */
	ulint		prtype,	/*!< in: precise type */
	ulint		len)	/*!< in: precision */
{
	dict_col_t*	col;
	ulint		i;

	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!heap == !name);

	i = table->n_def++;

	if (name) {
		if (UNIV_UNLIKELY(table->n_def == table->n_cols)) {
			heap = table->heap;
		}
		if (UNIV_LIKELY(i) && UNIV_UNLIKELY(!table->col_names)) {
			/* All preceding column names are empty. */
			char* s = static_cast<char*>(
				mem_heap_zalloc(heap, table->n_def));

			table->col_names = s;
		}

		table->col_names = dict_add_col_name(table->col_names,
						     i, name, heap);
	}

	col = dict_table_get_nth_col(table, i);

	dict_mem_fill_column_struct(col, i, mtype, prtype, len);
}

/**********************************************************************//**
Renames a column of a table in the data dictionary cache. */
static MY_ATTRIBUTE((nonnull))
void
dict_mem_table_col_rename_low(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	unsigned	i,	/*!< in: column offset corresponding to s */
	const char*	to,	/*!< in: new column name */
	const char*	s)	/*!< in: pointer to table->col_names */
{
	size_t from_len = strlen(s), to_len = strlen(to);

	ut_ad(i < table->n_def);
	ut_ad(from_len <= NAME_LEN);
	ut_ad(to_len <= NAME_LEN);

	char from[NAME_LEN];
	strncpy(from, s, NAME_LEN);

	if (from_len == to_len) {
		/* The easy case: simply replace the column name in
		table->col_names. */
		strcpy(const_cast<char*>(s), to);
	} else {
		/* We need to adjust all affected index->field
		pointers, as in dict_index_add_col(). First, copy
		table->col_names. */
		ulint	prefix_len	= s - table->col_names;

		for (; i < table->n_def; i++) {
			s += strlen(s) + 1;
		}

		ulint	full_len	= s - table->col_names;
		char*	col_names;

		if (to_len > from_len) {
			ulint table_size_before_rename_col
                                = mem_heap_get_size(table->heap);
			col_names = static_cast<char*>(
				mem_heap_alloc(
					table->heap,
					full_len + to_len - from_len));
			ulint table_size_after_rename_col
                                = mem_heap_get_size(table->heap);
			if (table_size_before_rename_col
                                != table_size_after_rename_col) {
                                dict_sys->size +=
                                        table_size_after_rename_col
                                                - table_size_before_rename_col;
                        }
			memcpy(col_names, table->col_names, prefix_len);
		} else {
			col_names = const_cast<char*>(table->col_names);
		}

		memcpy(col_names + prefix_len, to, to_len);
		memmove(col_names + prefix_len + to_len,
			table->col_names + (prefix_len + from_len),
			full_len - (prefix_len + from_len));

		/* Replace the field names in every index. */
		for (dict_index_t* index = dict_table_get_first_index(table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			ulint	n_fields = dict_index_get_n_fields(index);

			for (ulint i = 0; i < n_fields; i++) {
				dict_field_t*	field
					= dict_index_get_nth_field(
						index, i);
				ulint		name_ofs
					= field->name - table->col_names;
				if (name_ofs <= prefix_len) {
					field->name = col_names + name_ofs;
				} else {
					ut_a(name_ofs < full_len);
					field->name = col_names
						+ name_ofs + to_len - from_len;
				}
			}
		}

		table->col_names = col_names;
	}

	dict_foreign_t*	foreign;

	/* Replace the field names in every foreign key constraint. */
	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (foreign->foreign_index == NULL) {
			/* We may go here when we set foreign_key_checks to 0,
			and then try to rename a column and modify the
			corresponding foreign key constraint. The index
			would have been dropped, we have to find an equivalent
			one */
			for (unsigned f = 0; f < foreign->n_fields; f++) {
				if (strcmp(foreign->foreign_col_names[f], from)
				    == 0) {

					char** rc = const_cast<char**>(
						foreign->foreign_col_names
						+ f);

					if (to_len <= strlen(*rc)) {
						memcpy(*rc, to, to_len + 1);
					} else {
						*rc = static_cast<char*>(
							mem_heap_dup(
								foreign->heap,
								to,
								to_len + 1));
					}
				}
			}

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->foreign_table, NULL,
				foreign->foreign_col_names,
				foreign->n_fields, NULL, true, false);
			/* There must be an equivalent index in this case. */
			ut_ad(new_index != NULL);

			foreign->foreign_index = new_index;

		} else {

		for (unsigned f = 0; f < foreign->n_fields; f++) {
			/* These can point straight to
			table->col_names, because the foreign key
			constraints will be freed at the same time
			when the table object is freed. */
			foreign->foreign_col_names[f]
				= dict_index_get_nth_field(
						foreign->foreign_index,
						f)->name;
			}
		}
	}

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		ut_ad(foreign->referenced_index != NULL);

		for (unsigned f = 0; f < foreign->n_fields; f++) {
			/* foreign->referenced_col_names[] need to be
			copies, because the constraint may become
			orphan when foreign_key_checks=0 and the
			parent table is dropped. */

			const char* col_name = dict_index_get_nth_field(
				foreign->referenced_index, f)->name;

			if (strcmp(foreign->referenced_col_names[f],
				   col_name)) {
				char**	rc = const_cast<char**>(
					foreign->referenced_col_names + f);
				size_t	col_name_len_1 = strlen(col_name) + 1;

				if (col_name_len_1 <= strlen(*rc) + 1) {
					memcpy(*rc, col_name, col_name_len_1);
				} else {
					*rc = static_cast<char*>(
						mem_heap_dup(
							foreign->heap,
							col_name,
							col_name_len_1));
				}
			}
		}
	}
}

/**********************************************************************//**
Renames a column of a table in the data dictionary cache. */
UNIV_INTERN
void
dict_mem_table_col_rename(
/*======================*/
	dict_table_t*	table,	/*!< in/out: table */
	unsigned	nth_col,/*!< in: column index */
	const char*	from,	/*!< in: old column name */
	const char*	to)	/*!< in: new column name */
{
	const char*	s = table->col_names;

	ut_ad(nth_col < table->n_def);

	for (unsigned i = 0; i < nth_col; i++) {
		size_t	len = strlen(s);
		ut_ad(len > 0);
		s += len + 1;
	}

	/* This could fail if the data dictionaries are out of sync.
	Proceed with the renaming anyway. */
	ut_ad(!strcmp(from, s));

	dict_mem_table_col_rename_low(table, nth_col, to, s);
}

/**********************************************************************//**
This function populates a dict_col_t memory structure with
supplied information. */
UNIV_INTERN
void
dict_mem_fill_column_struct(
/*========================*/
	dict_col_t*	column,		/*!< out: column struct to be
					filled */
	ulint		col_pos,	/*!< in: column position */
	ulint		mtype,		/*!< in: main data type */
	ulint		prtype,		/*!< in: precise type */
	ulint		col_len)	/*!< in: column length */
{
#ifndef UNIV_HOTBACKUP
	ulint	mbminlen;
	ulint	mbmaxlen;
#endif /* !UNIV_HOTBACKUP */

	column->ind = (unsigned int) col_pos;
	column->ord_part = 0;
	column->max_prefix = 0;
	column->mtype = (unsigned int) mtype;
	column->prtype = (unsigned int) prtype;
	column->len = (unsigned int) col_len;
#ifndef UNIV_HOTBACKUP
        dtype_get_mblen(mtype, prtype, &mbminlen, &mbmaxlen);
	dict_col_set_mbminmaxlen(column, mbminlen, mbmaxlen);
#endif /* !UNIV_HOTBACKUP */
}

/**********************************************************************//**
Creates an index memory object.
@return	own: index object */
UNIV_INTERN
dict_index_t*
dict_mem_index_create(
/*==================*/
	const char*	table_name,	/*!< in: table name */
	const char*	index_name,	/*!< in: index name */
	ulint		space,		/*!< in: space where the index tree is
					placed, ignored if the index is of
					the clustered type */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields)	/*!< in: number of fields */
{
	dict_index_t*	index;
	mem_heap_t*	heap;

	ut_ad(table_name && index_name);

	heap = mem_heap_create(DICT_HEAP_SIZE);

	index = static_cast<dict_index_t*>(
		mem_heap_zalloc(heap, sizeof(*index)));

	dict_mem_fill_index_struct(index, heap, table_name, index_name,
				   space, type, n_fields);

	dict_index_zip_pad_mutex_create_lazy(index);
	return(index);
}

#ifndef UNIV_HOTBACKUP
/**********************************************************************//**
Creates and initializes a foreign constraint memory object.
@return	own: foreign constraint struct */
UNIV_INTERN
dict_foreign_t*
dict_mem_foreign_create(void)
/*=========================*/
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;

	heap = mem_heap_create(100);

	foreign = static_cast<dict_foreign_t*>(
		mem_heap_zalloc(heap, sizeof(dict_foreign_t)));

	foreign->heap = heap;

	return(foreign);
}

/**********************************************************************//**
Sets the foreign_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, foreign_table_name_lookup
will point to foreign_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
UNIV_INTERN
void
dict_mem_foreign_table_name_lookup_set(
/*===================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			ulint	len;

			len = strlen(foreign->foreign_table_name) + 1;

			foreign->foreign_table_name_lookup =
				static_cast<char*>(
					mem_heap_alloc(foreign->heap, len));
		}
		strcpy(foreign->foreign_table_name_lookup,
		       foreign->foreign_table_name);
		innobase_casedn_str(foreign->foreign_table_name_lookup);
	} else {
		foreign->foreign_table_name_lookup
			= foreign->foreign_table_name;
	}
}

/**********************************************************************//**
Sets the referenced_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, referenced_table_name_lookup
will point to referenced_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
UNIV_INTERN
void
dict_mem_referenced_table_name_lookup_set(
/*======================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			ulint	len;

			len = strlen(foreign->referenced_table_name) + 1;

			foreign->referenced_table_name_lookup =
				static_cast<char*>(
					mem_heap_alloc(foreign->heap, len));
		}
		strcpy(foreign->referenced_table_name_lookup,
		       foreign->referenced_table_name);
		innobase_casedn_str(foreign->referenced_table_name_lookup);
	} else {
		foreign->referenced_table_name_lookup
			= foreign->referenced_table_name;
	}
}
#endif /* !UNIV_HOTBACKUP */

/**********************************************************************//**
Adds a field definition to an index. NOTE: does not take a copy
of the column name if the field is a column. The memory occupied
by the column name may be released only after publishing the index. */
UNIV_INTERN
void
dict_mem_index_add_field(
/*=====================*/
	dict_index_t*	index,		/*!< in: index */
	const char*	name,		/*!< in: column name */
	ulint		prefix_len)	/*!< in: 0 or the column prefix length
					in a MySQL index like
					INDEX (textcol(25)) */
{
	dict_field_t*	field;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	index->n_def++;

	field = dict_index_get_nth_field(index, index->n_def - 1);

	field->name = name;
	field->prefix_len = (unsigned int) prefix_len;
}

/**********************************************************************//**
Frees an index memory object. */
UNIV_INTERN
void
dict_mem_index_free(
/*================*/
	dict_index_t*	index)	/*!< in: index */
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
#ifdef UNIV_BLOB_DEBUG
	if (index->blobs) {
		mutex_free(&index->blobs_mutex);
		rbt_free(index->blobs);
	}
#endif /* UNIV_BLOB_DEBUG */

	dict_index_zip_pad_mutex_destroy(index);

	mem_heap_free(index->heap);
}

/** Create a temporary tablename like "#sql-ibtid-inc where
  tid = the Table ID
  inc = a randomly initialized number that is incremented for each file
The table ID is a 64 bit integer, can use up to 20 digits, and is
initialized at bootstrap. The second number is 32 bits, can use up to 10
digits, and is initialized at startup to a randomly distributed number.
It is hoped that the combination of these two numbers will provide a
reasonably unique temporary file name.
@param[in]	heap	A memory heap
@param[in]	dbtab	Table name in the form database/table name
@param[in]	id	Table id
@return A unique temporary tablename suitable for InnoDB use */
UNIV_INTERN
char*
dict_mem_create_temporary_tablename(
	mem_heap_t*	heap,
	const char*	dbtab,
	table_id_t	id)
{
	size_t		size;
	char*		name;
	const char*	dbend = strchr(dbtab, '/');
	ut_ad(dbend);
	size_t		dblen = dbend - dbtab + 1;

#ifdef HAVE_ATOMIC_BUILTINS
	/* Increment a randomly initialized number for each temp file. */
	os_atomic_increment_uint32(&dict_temp_file_num, 1);
#else /* HAVE_ATOMIC_BUILTINS */
	dict_temp_file_num++;
#endif /* HAVE_ATOMIC_BUILTINS */

	size = tmp_file_prefix_length + 3 + 20 + 1 + 10 + dblen;
	name = static_cast<char*>(mem_heap_alloc(heap, size));
	memcpy(name, dbtab, dblen);
	ut_snprintf(name + dblen, size - dblen,
		    TEMP_FILE_PREFIX_INNODB UINT64PF "-" UINT32PF,
		    id, dict_temp_file_num);

	return(name);
}

/** Initialize dict memory variables */

void
dict_mem_init(void)
{
	/* Initialize a randomly distributed temporary file number */
	ib_uint32_t now = static_cast<ib_uint32_t>(ut_time());

	const byte* buf = reinterpret_cast<const byte*>(&now);
	ut_ad(ut_crc32 != NULL);

	dict_temp_file_num = ut_crc32(buf, sizeof(now));

	DBUG_PRINT("dict_mem_init",
		   ("Starting Temporary file number is " UINT32PF,
		   dict_temp_file_num));
}

/** Validate the search order in the foreign key set.
@param[in]	fk_set	the foreign key set to be validated
@return true if search order is fine in the set, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_foreign_set&	fk_set)
{
	dict_foreign_not_exists	not_exists(fk_set);

	dict_foreign_set::iterator it = std::find_if(
		fk_set.begin(), fk_set.end(), not_exists);

	if (it == fk_set.end()) {
		return(true);
	}

	dict_foreign_t*	foreign = *it;
	std::cerr << "Foreign key lookup failed: " << *foreign;
	std::cerr << fk_set;
	ut_ad(0);
	return(false);
}

/** Validate the search order in the foreign key sets of the table
(foreign_set and referenced_set).
@param[in]	table	table whose foreign key sets are to be validated
@return true if foreign key sets are fine, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_table_t&	table)
{
	return(dict_foreign_set_validate(table.foreign_set)
	       && dict_foreign_set_validate(table.referenced_set));
}

std::ostream&
operator<< (std::ostream& out, const dict_foreign_t& foreign)
{
	out << "[dict_foreign_t: id='" << foreign.id << "'";

	if (foreign.foreign_table_name != NULL) {
		out << ",for: '" << foreign.foreign_table_name << "'";
	}

	out << "]";
	return(out);
}

std::ostream&
operator<< (std::ostream& out, const dict_foreign_set& fk_set)
{
	out << "[dict_foreign_set:";
	std::for_each(fk_set.begin(), fk_set.end(), dict_foreign_print(out));
	out << "]" << std::endl;
	return(out);
}

