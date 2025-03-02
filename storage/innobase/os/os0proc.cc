/*****************************************************************************

Copyright (c) 1995, 2011, Oracle and/or its affiliates. All Rights Reserved.

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
@file os/os0proc.cc
The interface to the operating system
process control primitives

Created 9/30/1995 Heikki Tuuri
*******************************************************/

#include "os0proc.h"
#ifdef UNIV_NONINL
#include "os0proc.ic"
#endif

#include "ut0mem.h"
#include "ut0byte.h"

/* Linux release version */
#if defined(UNIV_LINUX) && defined(_GNU_SOURCE)
#include <string.h>		/* strverscmp() */
#include <sys/utsname.h>	/* uname() */
#endif

#if defined(WITH_WSREP) && defined(UNIV_LINUX)
#include <sys/mman.h>		/* madvise() */
#endif
#include "ha_prototypes.h"

/* FreeBSD for example has only MAP_ANON, Linux has MAP_ANONYMOUS and
MAP_ANON but MAP_ANON is marked as deprecated */
#if defined(MAP_ANONYMOUS)
#define OS_MAP_ANON	MAP_ANONYMOUS
#elif defined(MAP_ANON)
#define OS_MAP_ANON	MAP_ANON
#endif

/* Linux's MAP_POPULATE */
#if defined(MAP_POPULATE)
#define OS_MAP_POPULATE	MAP_POPULATE
#else
#define OS_MAP_POPULATE	0
#endif

UNIV_INTERN ibool os_use_large_pages;
/* Large page size. This may be a boot-time option on some platforms */
UNIV_INTERN ulint os_large_page_size;

#ifdef WITH_WSREP
extern my_bool wsrep_recovery;
#endif /* WITH_WSREP */

/****************************************************************//**
Retrieve and compare operating system release.
@return	TRUE if the OS release is equal to, or later than release. */
UNIV_INTERN
bool
os_compare_release(
/*===============*/
	const char*	release		/*!< in: OS release */
	MY_ATTRIBUTE((unused)))
{
#if defined(UNIV_LINUX) && defined(_GNU_SOURCE)
	struct utsname name;
	return uname(&name) == 0 && strverscmp(name.release, release) >= 0;
#else
	return false;
#endif
}

/****************************************************************//**
Converts the current process id to a number. It is not guaranteed that the
number is unique. In Linux returns the 'process number' of the current
thread. That number is the same as one sees in 'top', for example. In Linux
the thread id is not the same as one sees in 'top'.
@return	process id as a number */
UNIV_INTERN
ulint
os_proc_get_number(void)
/*====================*/
{
#ifdef __WIN__
	return((ulint)GetCurrentProcessId());
#else
	return((ulint) getpid());
#endif
}

/****************************************************************//**
Allocates large pages memory.
@return	allocated memory */
UNIV_INTERN
void*
os_mem_alloc_large(
/*===============*/
	ulint*	n,			/*!< in/out: number of bytes */
	bool	populate)		/*!< in: virtual page preallocation */
{
	void*	ptr;
	ulint	size;
#if defined HAVE_LARGE_PAGES && defined UNIV_LINUX
	int shmid;
	struct shmid_ds buf;

	if (!os_use_large_pages || !os_large_page_size) {
		goto skip;
	}

	/* Align block size to os_large_page_size */
	ut_ad(ut_is_2pow(os_large_page_size));
	size = ut_2pow_round(*n + (os_large_page_size - 1),
			     os_large_page_size);

	shmid = shmget(IPC_PRIVATE, (size_t) size, SHM_HUGETLB | SHM_R | SHM_W);
	if (shmid < 0) {
		fprintf(stderr, "InnoDB: HugeTLB: Warning: Failed to allocate"
			" %lu bytes. errno %d\n", size, errno);
		ptr = NULL;
	} else {
		ptr = shmat(shmid, NULL, 0);
		if (ptr == (void*)-1) {
			fprintf(stderr, "InnoDB: HugeTLB: Warning: Failed to"
				" attach shared memory segment, errno %d\n",
				errno);
			ptr = NULL;
		}

		/* Remove the shared memory segment so that it will be
		automatically freed after memory is detached or
		process exits */
		shmctl(shmid, IPC_RMID, &buf);
	}

	if (ptr) {
		*n = size;
		os_fast_mutex_lock(&ut_list_mutex);
		ut_total_allocated_memory += size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_ALLOC(ptr, size);
		return(ptr);
	}

	fprintf(stderr, "InnoDB HugeTLB: Warning: Using conventional"
		" memory pool\n");
skip:
#endif /* HAVE_LARGE_PAGES && UNIV_LINUX */

#ifdef __WIN__
	SYSTEM_INFO	system_info;
	GetSystemInfo(&system_info);

	/* Align block size to system page size */
	ut_ad(ut_is_2pow(system_info.dwPageSize));
	/* system_info.dwPageSize is only 32-bit. Casting to ulint is required
	on 64-bit Windows. */
	size = *n = ut_2pow_round(*n + (system_info.dwPageSize - 1),
				  (ulint) system_info.dwPageSize);
	ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
			   PAGE_READWRITE);
	if (!ptr) {
		fprintf(stderr, "InnoDB: VirtualAlloc(%lu bytes) failed;"
			" Windows error %lu\n",
			(ulong) size, (ulong) GetLastError());
	} else {
		os_fast_mutex_lock(&ut_list_mutex);
		ut_total_allocated_memory += size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_ALLOC(ptr, size);
	}
#elif !defined OS_MAP_ANON
	size = *n;
	ptr = ut_malloc_low(size, TRUE, FALSE);
#else
# ifdef HAVE_GETPAGESIZE
	size = getpagesize();
# else
	size = UNIV_PAGE_SIZE;
# endif
	/* Align block size to system page size */
	ut_ad(ut_is_2pow(size));
	size = *n = ut_2pow_round(*n + (size - 1), size);
	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | OS_MAP_ANON |
		   (populate ? OS_MAP_POPULATE : 0), -1, 0);
	if (UNIV_UNLIKELY(ptr == MAP_FAILED)) {
		fprintf(stderr, "InnoDB: mmap(%lu bytes) failed;"
			" errno %lu\n",
			(ulong) size, (ulong) errno);
		return NULL;
	} else {
		os_fast_mutex_lock(&ut_list_mutex);
		ut_total_allocated_memory += size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_ALLOC(ptr, size);
	}
#endif

#if defined(WITH_WSREP) && defined(UNIV_LINUX)
	/* Do not make the pages from this block available to the child after a
	fork(). This is required to speed up process spawning for Galera SST. */

	if (madvise(ptr, size, MADV_DONTFORK)) {
		fprintf(stderr, "InnoDB: Warning: madvise(MADV_DONTFORK) is "
			"not supported by the kernel. Spawning SST processes "
			"can be slow.\n");
	}
#endif

#if OS_MAP_ANON && OS_MAP_POPULATE
	/* MAP_POPULATE is only supported for private mappings
	since Linux 2.6.23. */
	populate = populate && !os_compare_release("2.6.23");

	if (populate) {
		ib_logf(IB_LOG_LEVEL_WARN, "InnoDB: Warning: mmap(MAP_POPULATE) "
			"is not supported for private mappings. "
			"Forcing preallocation by faulting in pages.\n");
	}
#endif

	/* Initialize the entire buffer to force the allocation
	of physical memory page frames. */
	if (populate) {
		memset(ptr, '\0', size);
	}

	return(ptr);
}

/****************************************************************//**
Frees large pages memory. */
UNIV_INTERN
void
os_mem_free_large(
/*==============*/
	void	*ptr,			/*!< in: pointer returned by
					os_mem_alloc_large() */
	ulint	size)			/*!< in: size returned by
					os_mem_alloc_large() */
{
	os_fast_mutex_lock(&ut_list_mutex);
	ut_a(ut_total_allocated_memory >= size);
	os_fast_mutex_unlock(&ut_list_mutex);

#if defined HAVE_LARGE_PAGES && defined UNIV_LINUX
	if (os_use_large_pages && os_large_page_size && !shmdt(ptr)) {
		os_fast_mutex_lock(&ut_list_mutex);
		ut_a(ut_total_allocated_memory >= size);
		ut_total_allocated_memory -= size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_FREE(ptr, size);
		return;
	}
#endif /* HAVE_LARGE_PAGES && UNIV_LINUX */
#ifdef __WIN__
	/* When RELEASE memory, the size parameter must be 0.
	Do not use MEM_RELEASE with MEM_DECOMMIT. */
	if (!VirtualFree(ptr, 0, MEM_RELEASE)) {
		fprintf(stderr, "InnoDB: VirtualFree(%p, %lu) failed;"
			" Windows error %lu\n",
			ptr, (ulong) size, (ulong) GetLastError());
	} else {
		os_fast_mutex_lock(&ut_list_mutex);
		ut_a(ut_total_allocated_memory >= size);
		ut_total_allocated_memory -= size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_FREE(ptr, size);
	}
#elif !defined OS_MAP_ANON
	ut_free(ptr);
#else
# if defined(UNIV_SOLARIS)
	if (munmap(static_cast<caddr_t>(ptr), size)) {
# else
	if (munmap(ptr, size)) {
# endif /* UNIV_SOLARIS */
		fprintf(stderr, "InnoDB: munmap(%p, %lu) failed;"
			" errno %lu\n",
			ptr, (ulong) size, (ulong) errno);
	} else {
		os_fast_mutex_lock(&ut_list_mutex);
		ut_a(ut_total_allocated_memory >= size);
		ut_total_allocated_memory -= size;
		os_fast_mutex_unlock(&ut_list_mutex);
		UNIV_MEM_FREE(ptr, size);
	}
#endif
}
