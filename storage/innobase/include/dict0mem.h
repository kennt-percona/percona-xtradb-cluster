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

/**************************************************//**
@file include/dict0mem.h
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0mem_h
#define dict0mem_h

#include "univ.i"

#ifndef UNIV_INNOCHECKSUM

#include "dict0types.h"
#include "data0type.h"
#include "mem0mem.h"
#include "row0types.h"
#include "rem0types.h"
#include "btr0types.h"
#ifndef UNIV_HOTBACKUP
# include "lock0types.h"
# include "que0types.h"
# include "sync0rw.h"
#endif /* !UNIV_HOTBACKUP */
#include "ut0mem.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "ut0byte.h"
#include "hash0hash.h"
#include "trx0types.h"
#include "fts0fts.h"
#include "os0once.h"
#include <set>
#include <algorithm>
#include <iterator>

/* Forward declaration. */
struct ib_rbt_t;

/** Type flags of an index: OR'ing of the flags is allowed to define a
combination of types */
/* @{ */
#define DICT_CLUSTERED	1	/*!< clustered index */
#define DICT_UNIQUE	2	/*!< unique index */
#define	DICT_UNIVERSAL	4	/*!< index which can contain records from any
				other index */
#define	DICT_IBUF	8	/*!< insert buffer tree */
#define	DICT_CORRUPT	16	/*!< bit to store the corrupted flag
				in SYS_INDEXES.TYPE */
#define	DICT_FTS	32	/* FTS index; can't be combined with the
				other flags */

#define	DICT_IT_BITS	6	/*!< number of bits used for
				SYS_INDEXES.TYPE */
/* @} */

#if 0 /* not implemented, retained for history */
/** Types for a table object */
#define DICT_TABLE_ORDINARY		1 /*!< ordinary table */
#define	DICT_TABLE_CLUSTER_MEMBER	2
#define	DICT_TABLE_CLUSTER		3 /* this means that the table is
					  really a cluster definition */
#endif

/* Table and tablespace flags are generally not used for the Antelope file
format except for the low order bit, which is used differently depending on
where the flags are stored.

==================== Low order flags bit =========================
                    | REDUNDANT | COMPACT | COMPRESSED and DYNAMIC
SYS_TABLES.TYPE     |     1     |    1    |     1
dict_table_t::flags |     0     |    1    |     1
FSP_SPACE_FLAGS     |     0     |    0    |     1
fil_space_t::flags  |     0     |    0    |     1

Before the 5.1 plugin, SYS_TABLES.TYPE was always DICT_TABLE_ORDINARY (1)
and the tablespace flags field was always 0. In the 5.1 plugin, these fields
were repurposed to identify compressed and dynamic row formats.

The following types and constants describe the flags found in dict_table_t
and SYS_TABLES.TYPE.  Similar flags found in fil_space_t and FSP_SPACE_FLAGS
are described in fsp0fsp.h. */

/* @{ */
/** dict_table_t::flags bit 0 is equal to 0 if the row format = Redundant */
#define DICT_TF_REDUNDANT		0	/*!< Redundant row format. */
/** dict_table_t::flags bit 0 is equal to 1 if the row format = Compact */
#define DICT_TF_COMPACT			1	/*!< Compact row format. */

/** This bitmask is used in SYS_TABLES.N_COLS to set and test whether
the Compact page format is used, i.e ROW_FORMAT != REDUNDANT */
#define DICT_N_COLS_COMPACT	0x80000000UL

#endif /* !UNIV_INNOCHECKSUM */

/** Width of the COMPACT flag */
#define DICT_TF_WIDTH_COMPACT		1
/** Width of the ZIP_SSIZE flag */
#define DICT_TF_WIDTH_ZIP_SSIZE		4
/** Width of the ATOMIC_BLOBS flag.  The Antelope file formats broke up
BLOB and TEXT fields, storing the first 768 bytes in the clustered index.
Brracuda row formats store the whole blob or text field off-page atomically.
Secondary indexes are created from this external data using row_ext_t
to cache the BLOB prefixes. */
#define DICT_TF_WIDTH_ATOMIC_BLOBS	1
/** If a table is created with the MYSQL option DATA DIRECTORY and
innodb-file-per-table, an older engine will not be able to find that table.
This flag prevents older engines from attempting to open the table and
allows InnoDB to update_create_info() accordingly. */
#define DICT_TF_WIDTH_DATA_DIR		1

/** Width of all the currently known table flags */
#define DICT_TF_BITS	(DICT_TF_WIDTH_COMPACT		\
			+ DICT_TF_WIDTH_ZIP_SSIZE	\
			+ DICT_TF_WIDTH_ATOMIC_BLOBS	\
			+ DICT_TF_WIDTH_DATA_DIR)

/** A mask of all the known/used bits in table flags */
#define DICT_TF_BIT_MASK	(~(~0 << DICT_TF_BITS))

/** Zero relative shift position of the COMPACT field */
#define DICT_TF_POS_COMPACT		0
/** Zero relative shift position of the ZIP_SSIZE field */
#define DICT_TF_POS_ZIP_SSIZE		(DICT_TF_POS_COMPACT		\
					+ DICT_TF_WIDTH_COMPACT)
/** Zero relative shift position of the ATOMIC_BLOBS field */
#define DICT_TF_POS_ATOMIC_BLOBS	(DICT_TF_POS_ZIP_SSIZE		\
					+ DICT_TF_WIDTH_ZIP_SSIZE)
/** Zero relative shift position of the DATA_DIR field */
#define DICT_TF_POS_DATA_DIR		(DICT_TF_POS_ATOMIC_BLOBS	\
					+ DICT_TF_WIDTH_ATOMIC_BLOBS)
/** Zero relative shift position of the start of the UNUSED bits */
#define DICT_TF_POS_UNUSED		(DICT_TF_POS_DATA_DIR		\
					+ DICT_TF_WIDTH_DATA_DIR)

/** Bit mask of the COMPACT field */
#define DICT_TF_MASK_COMPACT				\
		((~(~0U << DICT_TF_WIDTH_COMPACT))	\
		<< DICT_TF_POS_COMPACT)
/** Bit mask of the ZIP_SSIZE field */
#define DICT_TF_MASK_ZIP_SSIZE				\
		((~(~0U << DICT_TF_WIDTH_ZIP_SSIZE))	\
		<< DICT_TF_POS_ZIP_SSIZE)
/** Bit mask of the ATOMIC_BLOBS field */
#define DICT_TF_MASK_ATOMIC_BLOBS			\
		((~(~0U << DICT_TF_WIDTH_ATOMIC_BLOBS))	\
		<< DICT_TF_POS_ATOMIC_BLOBS)
/** Bit mask of the DATA_DIR field */
#define DICT_TF_MASK_DATA_DIR				\
		((~(~0U << DICT_TF_WIDTH_DATA_DIR))	\
		<< DICT_TF_POS_DATA_DIR)

/** Return the value of the COMPACT field */
#define DICT_TF_GET_COMPACT(flags)			\
		((flags & DICT_TF_MASK_COMPACT)		\
		>> DICT_TF_POS_COMPACT)
/** Return the value of the ZIP_SSIZE field */
#define DICT_TF_GET_ZIP_SSIZE(flags)			\
		((flags & DICT_TF_MASK_ZIP_SSIZE)	\
		>> DICT_TF_POS_ZIP_SSIZE)
/** Return the value of the ATOMIC_BLOBS field */
#define DICT_TF_HAS_ATOMIC_BLOBS(flags)			\
		((flags & DICT_TF_MASK_ATOMIC_BLOBS)	\
		>> DICT_TF_POS_ATOMIC_BLOBS)
/** Return the value of the DATA_DIR field */
#define DICT_TF_HAS_DATA_DIR(flags)			\
		((flags & DICT_TF_MASK_DATA_DIR)	\
		>> DICT_TF_POS_DATA_DIR)
/** Return the contents of the UNUSED bits */
#define DICT_TF_GET_UNUSED(flags)			\
		(flags >> DICT_TF_POS_UNUSED)
/* @} */

#ifndef UNIV_INNOCHECKSUM

/** @brief Table Flags set number 2.

These flags will be stored in SYS_TABLES.MIX_LEN.  All unused flags
will be written as 0.  The column may contain garbage for tables
created with old versions of InnoDB that only implemented
ROW_FORMAT=REDUNDANT.  InnoDB engines do not check these flags
for unknown bits in order to protect backward incompatibility. */
/* @{ */
/** Total number of bits in table->flags2. */
#define DICT_TF2_BITS			7
#define DICT_TF2_BIT_MASK		~(~0U << DICT_TF2_BITS)

/** TEMPORARY; TRUE for tables from CREATE TEMPORARY TABLE. */
#define DICT_TF2_TEMPORARY		1
/** The table has an internal defined DOC ID column */
#define DICT_TF2_FTS_HAS_DOC_ID		2
/** The table has an FTS index */
#define DICT_TF2_FTS			4
/** Need to add Doc ID column for FTS index build.
This is a transient bit for index build */
#define DICT_TF2_FTS_ADD_DOC_ID		8
/** This bit is used during table creation to indicate that it will
use its own tablespace instead of the system tablespace. */
#define DICT_TF2_USE_TABLESPACE		16

/** Set when we discard/detach the tablespace */
#define DICT_TF2_DISCARDED		32

/** This bit is set if all aux table names (both common tables and
index tables) of a FTS table are in HEX format. */
#define DICT_TF2_FTS_AUX_HEX_NAME	64
/* @} */

#define DICT_TF2_FLAG_SET(table, flag)				\
	(table->flags2 |= (flag))

#define DICT_TF2_FLAG_IS_SET(table, flag)			\
	(table->flags2 & (flag))

#define DICT_TF2_FLAG_UNSET(table, flag)			\
	(table->flags2 &= ~(flag))

/** Tables could be chained together with Foreign key constraint. When
first load the parent table, we would load all of its descedents.
This could result in rescursive calls and out of stack error eventually.
DICT_FK_MAX_RECURSIVE_LOAD defines the maximum number of recursive loads,
when exceeded, the child table will not be loaded. It will be loaded when
the foreign constraint check needs to be run. */
#define DICT_FK_MAX_RECURSIVE_LOAD	20

/** Similarly, when tables are chained together with foreign key constraints
with on cascading delete/update clause, delete from parent table could
result in recursive cascading calls. This defines the maximum number of
such cascading deletes/updates allowed. When exceeded, the delete from
parent table will fail, and user has to drop excessive foreign constraint
before proceeds. */
#define FK_MAX_CASCADE_DEL		255

/**********************************************************************//**
Creates a table memory object.
@return	own: table object */
UNIV_INTERN
dict_table_t*
dict_mem_table_create(
/*==================*/
	const char*	name,		/*!< in: table name */
	ulint		space,		/*!< in: space where the clustered index
					of the table is placed */
	ulint		n_cols,		/*!< in: number of columns */
	ulint		flags,		/*!< in: table flags */
	ulint		flags2);	/*!< in: table flags2 */
/****************************************************************//**
Free a table memory object. */
UNIV_INTERN
void
dict_mem_table_free(
/*================*/
	dict_table_t*	table);		/*!< in: table */
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
	ulint		len);	/*!< in: precision */
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
	MY_ATTRIBUTE((nonnull));
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
	ulint		col_len);	/*!< in: column length */
/**********************************************************************//**
This function poplulates a dict_index_t index memory structure with
supplied information. */
UNIV_INLINE
void
dict_mem_fill_index_struct(
/*=======================*/
	dict_index_t*	index,		/*!< out: index to be filled */
	mem_heap_t*	heap,		/*!< in: memory heap */
	const char*	table_name,	/*!< in: table name */
	const char*	index_name,	/*!< in: index name */
	ulint		space,		/*!< in: space where the index tree is
					placed, ignored if the index is of
					the clustered type */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields);	/*!< in: number of fields */
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
	ulint		n_fields);	/*!< in: number of fields */
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
	ulint		prefix_len);	/*!< in: 0 or the column prefix length
					in a MySQL index like
					INDEX (textcol(25)) */
/**********************************************************************//**
Frees an index memory object. */
UNIV_INTERN
void
dict_mem_index_free(
/*================*/
	dict_index_t*	index);	/*!< in: index */
/**********************************************************************//**
Creates and initializes a foreign constraint memory object.
@return	own: foreign constraint struct */
UNIV_INTERN
dict_foreign_t*
dict_mem_foreign_create(void);
/*=========================*/

/**********************************************************************//**
Sets the foreign_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, foreign_table_name_lookup
will point to foreign_table_name.  If 2, then another string is
allocated from the heap and set to lower case. */
UNIV_INTERN
void
dict_mem_foreign_table_name_lookup_set(
/*===================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc);	/*!< in: is an alloc needed */

/**********************************************************************//**
Sets the referenced_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, referenced_table_name_lookup
will point to referenced_table_name.  If 2, then another string is
allocated from the heap and set to lower case. */
UNIV_INTERN
void
dict_mem_referenced_table_name_lookup_set(
/*======================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc);	/*!< in: is an alloc needed */

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
	table_id_t	id);

/** Initialize dict memory variables */

void
dict_mem_init(void);

/** Data structure for a column in a table */
struct dict_col_t{
	/*----------------------*/
	/** The following are copied from dtype_t,
	so that all bit-fields can be packed tightly. */
	/* @{ */
	unsigned	prtype:32;	/*!< precise type; MySQL data
					type, charset code, flags to
					indicate nullability,
					signedness, whether this is a
					binary string, whether this is
					a true VARCHAR where MySQL
					uses 2 bytes to store the length */
	unsigned	mtype:8;	/*!< main data type */

	/* the remaining fields do not affect alphabetical ordering: */

	unsigned	len:16;		/*!< length; for MySQL data this
					is field->pack_length(),
					except that for a >= 5.0.3
					type true VARCHAR this is the
					maximum byte length of the
					string data (in addition to
					the string, MySQL uses 1 or 2
					bytes to store the string length) */

	unsigned	mbminmaxlen:5;	/*!< minimum and maximum length of a
					character, in bytes;
					DATA_MBMINMAXLEN(mbminlen,mbmaxlen);
					mbminlen=DATA_MBMINLEN(mbminmaxlen);
					mbmaxlen=DATA_MBMINLEN(mbminmaxlen) */
	/*----------------------*/
	/* End of definitions copied from dtype_t */
	/* @} */

	unsigned	ind:10;		/*!< table column position
					(starting from 0) */
	unsigned	ord_part:1;	/*!< nonzero if this column
					appears in the ordering fields
					of an index */
	unsigned	max_prefix:12;	/*!< maximum index prefix length on
					this column. Our current max limit is
					3072 for Barracuda table */
};

/** @brief DICT_ANTELOPE_MAX_INDEX_COL_LEN is measured in bytes and
is the maximum indexed column length (or indexed prefix length) in
ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT. Also, in any format,
any fixed-length field that is longer than this will be encoded as
a variable-length field.

It is set to 3*256, so that one can create a column prefix index on
256 characters of a TEXT or VARCHAR column also in the UTF-8
charset. In that charset, a character may take at most 3 bytes.  This
constant MUST NOT BE CHANGED, or the compatibility of InnoDB data
files would be at risk! */
#define DICT_ANTELOPE_MAX_INDEX_COL_LEN	REC_ANTELOPE_MAX_INDEX_COL_LEN

/** Find out maximum indexed column length by its table format.
For ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT, the maximum
field length is REC_ANTELOPE_MAX_INDEX_COL_LEN - 1 (767). For
Barracuda row formats COMPRESSED and DYNAMIC, the length could
be REC_VERSION_56_MAX_INDEX_COL_LEN (3072) bytes */
#define DICT_MAX_FIELD_LEN_BY_FORMAT(table)				\
		((dict_table_get_format(table) < UNIV_FORMAT_B)		\
			? (REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)		\
			: REC_VERSION_56_MAX_INDEX_COL_LEN)

#define DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags)			\
		((DICT_TF_HAS_ATOMIC_BLOBS(flags) < UNIV_FORMAT_B)	\
			? (REC_ANTELOPE_MAX_INDEX_COL_LEN - 1)		\
			: REC_VERSION_56_MAX_INDEX_COL_LEN)

/** Defines the maximum fixed length column size */
#define DICT_MAX_FIXED_COL_LEN		DICT_ANTELOPE_MAX_INDEX_COL_LEN
#ifdef WITH_WSREP
#define WSREP_MAX_SUPPORTED_KEY_LENGTH 3500
#endif /* WITH_WSREP */

/** Data structure for a field in an index */
struct dict_field_t{
	dict_col_t*	col;		/*!< pointer to the table column */
	const char*	name;		/*!< name of the column */
	unsigned	prefix_len:12;	/*!< 0 or the length of the column
					prefix in bytes in a MySQL index of
					type, e.g., INDEX (textcol(25));
					must be smaller than
					DICT_MAX_FIELD_LEN_BY_FORMAT;
					NOTE that in the UTF-8 charset, MySQL
					sets this to (mbmaxlen * the prefix len)
					in UTF-8 chars */
	unsigned	fixed_len:10;	/*!< 0 or the fixed length of the
					column if smaller than
					DICT_ANTELOPE_MAX_INDEX_COL_LEN */
};

/**********************************************************************//**
PADDING HEURISTIC BASED ON LINEAR INCREASE OF PADDING TO AVOID
COMPRESSION FAILURES
(Note: this is relevant only for compressed indexes)
GOAL: Avoid compression failures by maintaining information about the
compressibility of data. If data is not very compressible then leave
some extra space 'padding' in the uncompressed page making it more
likely that compression of less than fully packed uncompressed page will
succeed.

This padding heuristic works by increasing the pad linearly until the
desired failure rate is reached. A "round" is a fixed number of
compression operations.
After each round, the compression failure rate for that round is
computed. If the failure rate is too high, then padding is incremented
by a fixed value, otherwise it's left intact.
If the compression failure is lower than the desired rate for a fixed
number of consecutive rounds, then the padding is decreased by a fixed
value. This is done to prevent overshooting the padding value,
and to accommodate the possible change in data compressibility. */

/** Number of zip ops in one round. */
#define ZIP_PAD_ROUND_LEN			(128)

/** Number of successful rounds after which the padding is decreased */
#define ZIP_PAD_SUCCESSFUL_ROUND_LIMIT		(5)

/** Amount by which padding is increased. */
#define ZIP_PAD_INCR				(128)

/** Percentage of compression failures that are allowed in a single
round */
extern ulong	zip_failure_threshold_pct;

/** Maximum percentage of a page that can be allowed as a pad to avoid
compression failures */
extern ulong	zip_pad_max;

/** Data structure to hold information about how much space in
an uncompressed page should be left as padding to avoid compression
failures. This estimate is based on a self-adapting heuristic. */
struct zip_pad_info_t {
	os_fast_mutex_t*
			mutex;	/*!< mutex protecting the info */
	ulint		pad;	/*!< number of bytes used as pad */
	ulint		success;/*!< successful compression ops during
				current round */
	ulint		failure;/*!< failed compression ops during
				current round */
	ulint		n_rounds;/*!< number of currently successful
				rounds */
	volatile os_once::state_t
			mutex_created;
				/*!< Creation state of mutex member */
};

/** Data structure for an index.  Most fields will be
initialized to 0, NULL or FALSE in dict_mem_index_create(). */
struct dict_index_t{
	index_id_t	id;	/*!< id of the index */
	prio_rw_lock_t*	search_latch; /*!< latch protecting the AHI partition
				      corresponding to this index */
	hash_table_t*	search_table; /*!< hash table protected by
				      search_latch */
	mem_heap_t*	heap;	/*!< memory heap */
	const char*	name;	/*!< index name */
	const char*	table_name;/*!< table name */
	dict_table_t*	table;	/*!< back pointer to table */
#ifndef UNIV_HOTBACKUP
	unsigned	space:32;
				/*!< space where the index tree is placed */
	unsigned	page:32;/*!< index tree root page number */
#endif /* !UNIV_HOTBACKUP */
	unsigned	type:DICT_IT_BITS;
				/*!< index type (DICT_CLUSTERED, DICT_UNIQUE,
				DICT_UNIVERSAL, DICT_IBUF, DICT_CORRUPT) */
#define MAX_KEY_LENGTH_BITS 12
	unsigned	trx_id_offset:MAX_KEY_LENGTH_BITS;
				/*!< position of the trx id column
				in a clustered index record, if the fields
				before it are known to be of a fixed size,
				0 otherwise */
#if (1<<MAX_KEY_LENGTH_BITS) < MAX_KEY_LENGTH
# error (1<<MAX_KEY_LENGTH_BITS) < MAX_KEY_LENGTH
#endif
	unsigned	n_user_defined_cols:10;
				/*!< number of columns the user defined to
				be in the index: in the internal
				representation we add more columns */
	unsigned	n_uniq:10;/*!< number of fields from the beginning
				which are enough to determine an index
				entry uniquely */
	unsigned	n_def:10;/*!< number of fields defined so far */
	unsigned	n_fields:10;/*!< number of fields in the index */
	unsigned	n_nullable:10;/*!< number of nullable fields */
	unsigned	cached:1;/*!< TRUE if the index object is in the
				dictionary cache */
	unsigned	to_be_dropped:1;
				/*!< TRUE if the index is to be dropped;
				protected by dict_operation_lock */
	unsigned	online_status:2;
				/*!< enum online_index_status.
				Transitions from ONLINE_INDEX_COMPLETE (to
				ONLINE_INDEX_CREATION) are protected
				by dict_operation_lock and
				dict_sys->mutex. Other changes are
				protected by index->lock. */
	dict_field_t*	fields;	/*!< array of field descriptions */
	bool            index_fts_syncing;/*!< Whether the fts index is
					still syncing in the background */
#ifndef UNIV_HOTBACKUP
	UT_LIST_NODE_T(dict_index_t)
			indexes;/*!< list of indexes of the table */
	btr_search_t*	search_info;
				/*!< info used in optimistic searches */
	row_log_t*	online_log;
				/*!< the log of modifications
				during online index creation;
				valid when online_status is
				ONLINE_INDEX_CREATION */
	/*----------------------*/
	/** Statistics for query optimization */
	/* @{ */
	ib_uint64_t*	stat_n_diff_key_vals;
				/*!< approximate number of different
				key values for this index, for each
				n-column prefix where 1 <= n <=
				dict_get_n_unique(index) (the array is
				indexed from 0 to n_uniq-1); we
				periodically calculate new
				estimates */
	ib_uint64_t*	stat_n_sample_sizes;
				/*!< number of pages that were sampled
				to calculate each of stat_n_diff_key_vals[],
				e.g. stat_n_sample_sizes[3] pages were sampled
				to get the number stat_n_diff_key_vals[3]. */
	ib_uint64_t*	stat_n_non_null_key_vals;
				/* approximate number of non-null key values
				for this index, for each column where
				1 <= n <= dict_get_n_unique(index) (the array
				is indexed from 0 to n_uniq-1); This
				is used when innodb_stats_method is
				"nulls_ignored". */
	ulint		stat_index_size;
				/*!< approximate index size in
				database pages */
	ulint		stat_n_leaf_pages;
				/*!< approximate number of leaf pages in the
				index tree */
	/* @} */
	prio_rw_lock_t	lock;	/*!< read-write lock protecting the
				upper levels of the index tree */
	trx_id_t	trx_id; /*!< id of the transaction that created this
				index, or 0 if the index existed
				when InnoDB was started up */
	zip_pad_info_t	zip_pad;/*!< Information about state of
				compression failures and successes */
#endif /* !UNIV_HOTBACKUP */
#ifdef UNIV_BLOB_DEBUG
	ib_mutex_t		blobs_mutex;
				/*!< mutex protecting blobs */
	ib_rbt_t*	blobs;	/*!< map of (page_no,heap_no,field_no)
				to first_blob_page_no; protected by
				blobs_mutex; @see btr_blob_dbg_t */
#endif /* UNIV_BLOB_DEBUG */
#ifdef UNIV_DEBUG
	ulint		magic_n;/*!< magic number */
/** Value of dict_index_t::magic_n */
# define DICT_INDEX_MAGIC_N	76789786
#endif
};

/** The status of online index creation */
enum online_index_status {
	/** the index is complete and ready for access */
	ONLINE_INDEX_COMPLETE = 0,
	/** the index is being created, online
	(allowing concurrent modifications) */
	ONLINE_INDEX_CREATION,
	/** secondary index creation was aborted and the index
	should be dropped as soon as index->table->n_ref_count reaches 0,
	or online table rebuild was aborted and the clustered index
	of the original table should soon be restored to
	ONLINE_INDEX_COMPLETE */
	ONLINE_INDEX_ABORTED,
	/** the online index creation was aborted, the index was
	dropped from the data dictionary and the tablespace, and it
	should be dropped from the data dictionary cache as soon as
	index->table->n_ref_count reaches 0. */
	ONLINE_INDEX_ABORTED_DROPPED
};

/** Data structure for a foreign key constraint; an example:
FOREIGN KEY (A, B) REFERENCES TABLE2 (C, D).  Most fields will be
initialized to 0, NULL or FALSE in dict_mem_foreign_create(). */
struct dict_foreign_t{
	mem_heap_t*	heap;		/*!< this object is allocated from
					this memory heap */
	char*		id;		/*!< id of the constraint as a
					null-terminated string */
	unsigned	n_fields:10;	/*!< number of indexes' first fields
					for which the foreign key
					constraint is defined: we allow the
					indexes to contain more fields than
					mentioned in the constraint, as long
					as the first fields are as mentioned */
	unsigned	type:6;		/*!< 0 or DICT_FOREIGN_ON_DELETE_CASCADE
					or DICT_FOREIGN_ON_DELETE_SET_NULL */
	char*		foreign_table_name;/*!< foreign table name */
	char*		foreign_table_name_lookup;
				/*!< foreign table name used for dict lookup */
	dict_table_t*	foreign_table;	/*!< table where the foreign key is */
	const char**	foreign_col_names;/*!< names of the columns in the
					foreign key */
	char*		referenced_table_name;/*!< referenced table name */
	char*		referenced_table_name_lookup;
				/*!< referenced table name for dict lookup*/
	dict_table_t*	referenced_table;/*!< table where the referenced key
					is */
	const char**	referenced_col_names;/*!< names of the referenced
					columns in the referenced table */
	dict_index_t*	foreign_index;	/*!< foreign index; we require that
					both tables contain explicitly defined
					indexes for the constraint: InnoDB
					does not generate new indexes
					implicitly */
	dict_index_t*	referenced_index;/*!< referenced index */
};

std::ostream&
operator<< (std::ostream& out, const dict_foreign_t& foreign);

struct dict_foreign_print {

	dict_foreign_print(std::ostream& out)
		: m_out(out)
	{}

	void operator()(const dict_foreign_t* foreign) {
		m_out << *foreign;
	}
private:
	std::ostream&	m_out;
};

/** Compare two dict_foreign_t objects using their ids. Used in the ordering
of dict_table_t::foreign_set and dict_table_t::referenced_set.  It returns
true if the first argument is considered to go before the second in the
strict weak ordering it defines, and false otherwise. */
struct dict_foreign_compare {

	bool operator()(
		const dict_foreign_t*	lhs,
		const dict_foreign_t*	rhs) const
	{
		return(ut_strcmp(lhs->id, rhs->id) < 0);
	}
};

/** A function object to find a foreign key with the given index as the
referenced index. Return the foreign key with matching criteria or NULL */
struct dict_foreign_with_index {

	dict_foreign_with_index(const dict_index_t*	index)
	: m_index(index)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->referenced_index == m_index);
	}

	const dict_index_t*	m_index;
};

#ifdef WITH_WSREP
/** A function object to find a foreign key with the given index as the
foreign index. Return the foreign key with matching criteria or NULL */
struct dict_foreign_with_foreign_index {

	dict_foreign_with_foreign_index(const dict_index_t*	index)
	: m_index(index)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->foreign_index == m_index);
	}

	const dict_index_t*	m_index;
};
#endif
/* A function object to check if the foreign constraint is between different
tables.  Returns true if foreign key constraint is between different tables,
false otherwise. */
struct dict_foreign_different_tables {

	bool operator()(const dict_foreign_t*	foreign) const
	{
		return(foreign->foreign_table != foreign->referenced_table);
	}
};

/** A function object to check if the foreign key constraint has the same
name as given.  If the full name of the foreign key constraint doesn't match,
then, check if removing the database name from the foreign key constraint
matches. Return true if it matches, false otherwise. */
struct dict_foreign_matches_id {

	dict_foreign_matches_id(const char* id)
		: m_id(id)
	{}

	bool operator()(const dict_foreign_t*	foreign) const
	{
		if (0 == innobase_strcasecmp(foreign->id, m_id)) {
			return(true);
		}
		if (const char* pos = strchr(foreign->id, '/')) {
			if (0 == innobase_strcasecmp(m_id, pos + 1)) {
				return(true);
			}
		}
		return(false);
	}

	const char*	m_id;
};

typedef std::set<dict_foreign_t*, dict_foreign_compare> dict_foreign_set;

std::ostream&
operator<< (std::ostream& out, const dict_foreign_set& fk_set);

/** Function object to check if a foreign key object is there
in the given foreign key set or not.  It returns true if the
foreign key is not found, false otherwise */
struct dict_foreign_not_exists {
	dict_foreign_not_exists(const dict_foreign_set& obj_)
		: m_foreigns(obj_)
	{}

	/* Return true if the given foreign key is not found */
	bool operator()(dict_foreign_t* const & foreign) const {
		return(m_foreigns.find(foreign) == m_foreigns.end());
	}
private:
	const dict_foreign_set&	m_foreigns;
};

/** Validate the search order in the foreign key set.
@param[in]	fk_set	the foreign key set to be validated
@return true if search order is fine in the set, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_foreign_set&	fk_set);

/** Validate the search order in the foreign key sets of the table
(foreign_set and referenced_set).
@param[in]	table	table whose foreign key sets are to be validated
@return true if foreign key sets are fine, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_table_t&	table);

/*********************************************************************//**
Frees a foreign key struct. */
inline
void
dict_foreign_free(
/*==============*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign key struct */
{
	mem_heap_free(foreign->heap);
}

/** The destructor will free all the foreign key constraints in the set
by calling dict_foreign_free() on each of the foreign key constraints.
This is used to free the allocated memory when a local set goes out
of scope. */
struct dict_foreign_set_free {

	dict_foreign_set_free(const dict_foreign_set&	foreign_set)
		: m_foreign_set(foreign_set)
	{}

	~dict_foreign_set_free()
	{
		std::for_each(m_foreign_set.begin(),
			      m_foreign_set.end(),
			      dict_foreign_free);
	}

	const dict_foreign_set&	m_foreign_set;
};

/** The flags for ON_UPDATE and ON_DELETE can be ORed; the default is that
a foreign key constraint is enforced, therefore RESTRICT just means no flag */
/* @{ */
#define DICT_FOREIGN_ON_DELETE_CASCADE	1	/*!< ON DELETE CASCADE */
#define DICT_FOREIGN_ON_DELETE_SET_NULL	2	/*!< ON UPDATE SET NULL */
#define DICT_FOREIGN_ON_UPDATE_CASCADE	4	/*!< ON DELETE CASCADE */
#define DICT_FOREIGN_ON_UPDATE_SET_NULL	8	/*!< ON UPDATE SET NULL */
#define DICT_FOREIGN_ON_DELETE_NO_ACTION 16	/*!< ON DELETE NO ACTION */
#define DICT_FOREIGN_ON_UPDATE_NO_ACTION 32	/*!< ON UPDATE NO ACTION */
/* @} */

/* This flag is for sync SQL DDL and memcached DML.
if table->memcached_sync_count == DICT_TABLE_IN_DDL means there's DDL running on
the table, DML from memcached will be blocked. */
#define DICT_TABLE_IN_DDL -1

/** Data structure for a database table.  Most fields will be
initialized to 0, NULL or FALSE in dict_mem_table_create(). */
struct dict_table_t{


	table_id_t	id;	/*!< id of the table */
	mem_heap_t*	heap;	/*!< memory heap */
	char*		name;	/*!< table name */
	const char*	dir_path_of_temp_table;/*!< NULL or the directory path
				where a TEMPORARY table that was explicitly
				created by a user should be placed if
				innodb_file_per_table is defined in my.cnf;
				in Unix this is usually /tmp/..., in Windows
				temp\... */
	char*		data_dir_path; /*!< NULL or the directory path
				specified by DATA DIRECTORY */
	unsigned	space:32;
				/*!< space where the clustered index of the
				table is placed */
	unsigned	flags:DICT_TF_BITS;	/*!< DICT_TF_... */
	unsigned	flags2:DICT_TF2_BITS;	/*!< DICT_TF2_... */
	unsigned	ibd_file_missing:1;
				/*!< TRUE if this is in a single-table
				tablespace and the .ibd file is missing; then
				we must return in ha_innodb.cc an error if the
				user tries to query such an orphaned table */
	unsigned	cached:1;/*!< TRUE if the table object has been added
				to the dictionary cache */
	unsigned	to_be_dropped:1;
				/*!< TRUE if the table is to be dropped, but
				not yet actually dropped (could in the bk
				drop list); It is turned on at the beginning
				of row_drop_table_for_mysql() and turned off
				just before we start to update system tables
				for the drop. It is protected by
				dict_operation_lock */
	unsigned	n_def:10;/*!< number of columns defined so far */
	unsigned	n_cols:10;/*!< number of columns */
	unsigned	can_be_evicted:1;
				/*!< TRUE if it's not an InnoDB system table
				or a table that has no FK relationships */
	unsigned	corrupted:1;
				/*!< TRUE if table is corrupted */
	unsigned	drop_aborted:1;
				/*!< TRUE if some indexes should be dropped
				after ONLINE_INDEX_ABORTED
				or ONLINE_INDEX_ABORTED_DROPPED */
	dict_col_t*	cols;	/*!< array of column descriptions */
	const char*	col_names;
				/*!< Column names packed in a character string
				"name1\0name2\0...nameN\0".  Until
				the string contains n_cols, it will be
				allocated from a temporary heap.  The final
				string will be allocated from table->heap. */
#ifndef UNIV_HOTBACKUP
	hash_node_t	name_hash; /*!< hash chain node */
	hash_node_t	id_hash; /*!< hash chain node */
	UT_LIST_BASE_NODE_T(dict_index_t)
			indexes; /*!< list of indexes of the table */

	dict_foreign_set	foreign_set;
				/*!< set of foreign key constraints
				in the table; these refer to columns
				in other tables */

	dict_foreign_set	referenced_set;
				/*!< list of foreign key constraints
				which refer to this table */

	UT_LIST_NODE_T(dict_table_t)
			table_LRU; /*!< node of the LRU list of tables */
	unsigned	fk_max_recusive_level:8;
				/*!< maximum recursive level we support when
				loading tables chained together with FK
				constraints. If exceeds this level, we will
				stop loading child table into memory along with
				its parent table */
	ulint		n_foreign_key_checks_running;
				/*!< count of how many foreign key check
				operations are currently being performed
				on the table: we cannot drop the table while
				there are foreign key checks running on
				it! */
	trx_id_t	def_trx_id;
				/*!< transaction id that last touched
				the table definition, either when
				loading the definition or CREATE
				TABLE, or ALTER TABLE (prepare,
				commit, and rollback phases) */
	trx_id_t	query_cache_inv_trx_id;
				/*!< transactions whose trx id is
				smaller than this number are not
				allowed to store to the MySQL query
				cache or retrieve from it; when a trx
				with undo logs commits, it sets this
				to the value of the trx id counter for
				the tables it had an IX lock on */
#ifdef UNIV_DEBUG
	/*----------------------*/
	ibool		does_not_fit_in_memory;
				/*!< this field is used to specify in
				simulations tables which are so big
				that disk should be accessed: disk
				access is simulated by putting the
				thread to sleep for a while; NOTE that
				this flag is not stored to the data
				dictionary on disk, and the database
				will forget about value TRUE if it has
				to reload the table definition from
				disk */
#endif /* UNIV_DEBUG */
	/*----------------------*/
	unsigned	big_rows:1;
				/*!< flag: TRUE if the maximum length of
				a single row exceeds BIG_ROW_SIZE;
				initialized in dict_table_add_to_cache() */
				/** Statistics for query optimization */
				/* @{ */

	volatile os_once::state_t	stats_latch_created;
				/*!< Creation state of 'stats_latch'. */

	rw_lock_t*	stats_latch; /*!< this latch protects:
				dict_table_t::stat_initialized
				dict_table_t::stat_n_rows (*)
				dict_table_t::stat_clustered_index_size
				dict_table_t::stat_sum_of_other_index_sizes
				dict_table_t::stat_modified_counter (*)
				dict_table_t::indexes*::stat_n_diff_key_vals[]
				dict_table_t::indexes*::stat_index_size
				dict_table_t::indexes*::stat_n_leaf_pages
				(*) those are not always protected for
				performance reasons. */
	unsigned	stat_initialized:1; /*!< TRUE if statistics have
				been calculated the first time
				after database startup or table creation */
#define DICT_TABLE_IN_USED      -1
	lint		memcached_sync_count;
				/*!< count of how many handles are opened
				to this table from memcached; DDL on the
				table is NOT allowed until this count
				goes to zero. If it's -1, means there's DDL
		                on the table, DML from memcached will be
				blocked. */
	ib_time_t	stats_last_recalc;
				/*!< Timestamp of last recalc of the stats */
	ib_uint32_t	stat_persistent;
				/*!< The two bits below are set in the
				::stat_persistent member and have the following
				meaning:
				1. _ON=0, _OFF=0, no explicit persistent stats
				setting for this table, the value of the global
				srv_stats_persistent is used to determine
				whether the table has persistent stats enabled
				or not
				2. _ON=0, _OFF=1, persistent stats are
				explicitly disabled for this table, regardless
				of the value of the global srv_stats_persistent
				3. _ON=1, _OFF=0, persistent stats are
				explicitly enabled for this table, regardless
				of the value of the global srv_stats_persistent
				4. _ON=1, _OFF=1, not allowed, we assert if
				this ever happens. */
#define DICT_STATS_PERSISTENT_ON	(1 << 1)
#define DICT_STATS_PERSISTENT_OFF	(1 << 2)
	ib_uint32_t	stats_auto_recalc;
				/*!< The two bits below are set in the
				::stats_auto_recalc member and have
				the following meaning:
				1. _ON=0, _OFF=0, no explicit auto recalc
				setting for this table, the value of the global
				srv_stats_persistent_auto_recalc is used to
				determine whether the table has auto recalc
				enabled or not
				2. _ON=0, _OFF=1, auto recalc is explicitly
				disabled for this table, regardless of the
				value of the global
				srv_stats_persistent_auto_recalc
				3. _ON=1, _OFF=0, auto recalc is explicitly
				enabled for this table, regardless of the
				value of the global
				srv_stats_persistent_auto_recalc
				4. _ON=1, _OFF=1, not allowed, we assert if
				this ever happens. */
#define DICT_STATS_AUTO_RECALC_ON	(1 << 1)
#define DICT_STATS_AUTO_RECALC_OFF	(1 << 2)
	ulint		stats_sample_pages;
				/*!< the number of pages to sample for this
				table during persistent stats estimation;
				if this is 0, then the value of the global
				srv_stats_persistent_sample_pages will be
				used instead. */
	ib_uint64_t	stat_n_rows;
				/*!< approximate number of rows in the table;
				we periodically calculate new estimates */
	ulint		stat_clustered_index_size;
				/*!< approximate clustered index size in
				database pages */
	ulint		stat_sum_of_other_index_sizes;
				/*!< other indexes in database pages */
	ib_uint64_t	stat_modified_counter;
				/*!< when a row is inserted, updated,
				or deleted,
				we add 1 to this number; we calculate new
				estimates for the stat_... values for the
				table and the indexes when about 1 / 16 of
				table has been modified;
				also when the estimate operation is
				called for MySQL SHOW TABLE STATUS; the
				counter is reset to zero at statistics
				calculation; this counter is not protected by
				any latch, because this is only used for
				heuristics */
#define BG_STAT_NONE		0
#define BG_STAT_IN_PROGRESS	(1 << 0)
				/*!< BG_STAT_IN_PROGRESS is set in
				stats_bg_flag when the background
				stats code is working on this table. The DROP
				TABLE code waits for this to be cleared
				before proceeding. */
#define BG_STAT_SHOULD_QUIT	(1 << 1)
				/*!< BG_STAT_SHOULD_QUIT is set in
				stats_bg_flag when DROP TABLE starts
				waiting on BG_STAT_IN_PROGRESS to be cleared,
				the background stats thread will detect this
				and will eventually quit sooner */
	byte		stats_bg_flag;
				/*!< see BG_STAT_* above.
				Writes are covered by dict_sys->mutex.
				Dirty reads are possible. */
				/* @} */
	/*----------------------*/
				/**!< The following fields are used by the
				AUTOINC code.  The actual collection of
				tables locked during AUTOINC read/write is
				kept in trx_t. In order to quickly determine
				whether a transaction has locked the AUTOINC
				lock we keep a pointer to the transaction
				here in the autoinc_trx variable. This is to
				avoid acquiring the lock_sys_t::mutex and
				scanning the vector in trx_t.

				When an AUTOINC lock has to wait, the
				corresponding lock instance is created on
				the trx lock heap rather than use the
				pre-allocated instance in autoinc_lock below.*/
				/* @{ */
	lock_t*		autoinc_lock;
				/*!< a buffer for an AUTOINC lock
				for this table: we allocate the memory here
				so that individual transactions can get it
				and release it without a need to allocate
				space from the lock heap of the trx:
				otherwise the lock heap would grow rapidly
				if we do a large insert from a select */
	ib_mutex_t*	autoinc_mutex;
				/*!< mutex protecting the autoincrement
				counter */

	/** Creation state of autoinc_mutex member */
	volatile os_once::state_t
			autoinc_mutex_created;

	ib_uint64_t	autoinc;/*!< autoinc counter value to give to the
				next inserted row */
	ulong		n_waiting_or_granted_auto_inc_locks;
				/*!< This counter is used to track the number
				of granted and pending autoinc locks on this
				table. This value is set after acquiring the
				lock_sys_t::mutex but we peek the contents to
				determine whether other transactions have
				acquired the AUTOINC lock or not. Of course
				only one transaction can be granted the
				lock but there can be multiple waiters. */
	const trx_t*	autoinc_trx;
				/*!< The transaction that currently holds the
				the AUTOINC lock on this table.
				Protected by lock_sys->mutex. */
	fts_t*		fts;	/* FTS specific state variables */
				/* @} */
	/*----------------------*/

	ib_quiesce_t	 quiesce;/*!< Quiescing states, protected by the
				dict_index_t::lock. ie. we can only change
				the state if we acquire all the latches
				(dict_index_t::lock) in X mode of this table's
				indexes. */

	/*----------------------*/
	ulint		n_rec_locks;
				/*!< Count of the number of record locks on
				this table. We use this to determine whether
				we can evict the table from the dictionary
				cache. It is protected by lock_sys->mutex. */
	ulint		n_ref_count;
				/*!< count of how many handles are opened
				to this table; dropping of the table is
				NOT allowed until this count gets to zero;
				MySQL does NOT itself check the number of
				open handles at drop */
	UT_LIST_BASE_NODE_T(lock_t)
			locks;	/*!< list of locks on the table; protected
				by lock_sys->mutex */
	ibool		is_corrupt;
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_DEBUG
	ulint		magic_n;/*!< magic number */
/** Value of dict_table_t::magic_n */
# define DICT_TABLE_MAGIC_N	76333786
#endif /* UNIV_DEBUG */
};

/** A function object to add the foreign key constraint to the referenced set
of the referenced table, if it exists in the dictionary cache. */
struct dict_foreign_add_to_referenced_table {
	void operator()(dict_foreign_t*	foreign) const
	{
		if (dict_table_t* table = foreign->referenced_table) {
			std::pair<dict_foreign_set::iterator, bool>	ret
				= table->referenced_set.insert(foreign);
			ut_a(ret.second);
		}
	}
};

/** Destroy the autoinc latch of the given table.
This function is only called from either single threaded environment
or from a thread that has not shared the table object with other threads.
@param[in,out]	table	table whose stats latch to destroy */
inline
void
dict_table_autoinc_destroy(
	dict_table_t*	table)
{
	if (table->autoinc_mutex_created == os_once::DONE
	    && table->autoinc_mutex != NULL) {
		mutex_free(table->autoinc_mutex);
		delete table->autoinc_mutex;
	}
}

/** Allocate and init the autoinc latch of a given table.
This function must not be called concurrently on the same table object.
@param[in,out]	table_void	table whose autoinc latch to create */
void
dict_table_autoinc_alloc(
	void*	table_void);

/** Allocate and init the zip_pad_mutex of a given index.
This function must not be called concurrently on the same index object.
@param[in,out]	index_void	index whose zip_pad_mutex to create */
void
dict_index_zip_pad_alloc(
	void*	index_void);

/** Request for lazy creation of the autoinc latch of a given table.
This function is only called from either single threaded environment
or from a thread that has not shared the table object with other threads.
@param[in,out]	table	table whose autoinc latch is to be created. */
inline
void
dict_table_autoinc_create_lazy(
	dict_table_t*	table)
{
#ifdef HAVE_ATOMIC_BUILTINS
	table->autoinc_mutex = NULL;
	table->autoinc_mutex_created = os_once::NEVER_DONE;
#else /* HAVE_ATOMIC_BUILTINS */
	dict_table_autoinc_alloc(table);
	table->autoinc_mutex_created = os_once::DONE;
#endif /* HAVE_ATOMIC_BUILTINS */
}

/** Request a lazy creation of dict_index_t::zip_pad::mutex.
This function is only called from either single threaded environment
or from a thread that has not shared the table object with other threads.
@param[in,out]	index	index whose zip_pad mutex is to be created */
inline
void
dict_index_zip_pad_mutex_create_lazy(
	dict_index_t*	index)
{
#ifdef HAVE_ATOMIC_BUILTINS
	index->zip_pad.mutex = NULL;
	index->zip_pad.mutex_created = os_once::NEVER_DONE;
#else /* HAVE_ATOMIC_BUILTINS */
	dict_index_zip_pad_alloc(index);
	index->zip_pad.mutex_created = os_once::DONE;
#endif /* HAVE_ATOMIC_BUILTINS */
}

/** Destroy the zip_pad_mutex of the given index.
This function is only called from either single threaded environment
or from a thread that has not shared the table object with other threads.
@param[in,out]	table	table whose stats latch to destroy */
inline
void
dict_index_zip_pad_mutex_destroy(
	dict_index_t*	index)
{
	if (index->zip_pad.mutex_created == os_once::DONE
	    && index->zip_pad.mutex != NULL) {
		os_fast_mutex_free(index->zip_pad.mutex);
		delete index->zip_pad.mutex;
	}
}

/** Release the zip_pad_mutex of a given index.
@param[in,out]	index	index whose zip_pad_mutex is to be released */
inline
void
dict_index_zip_pad_unlock(
	dict_index_t*	index)
{
	os_fast_mutex_unlock(index->zip_pad.mutex);
}

#ifdef UNIV_DEBUG
/** Check if the current thread owns the autoinc_mutex of a given table.
@param[in]	table	the autoinc_mutex belongs to this table
@return true, if the current thread owns the autoinc_mutex, false otherwise.*/
inline
bool
dict_table_autoinc_own(
	const dict_table_t*	table)
{
	return(mutex_own(table->autoinc_mutex));
}
#endif /* UNIV_DEBUG */

#ifndef UNIV_NONINL
#include "dict0mem.ic"
#endif

#endif /* !UNIV_INNOCHECKSUM */

#endif
