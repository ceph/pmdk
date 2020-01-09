/*
 * Copyright 2019-2020, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * map_posix.c -- pmem2_map (POSIX)
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

#include "valgrind_internal.h"

#include "libpmem2.h"
#include "alloc.h"
#include "auto_flush.h"
#include "out.h"
#include "file.h"
#include "config.h"
#include "map.h"
#include "pmem2_utils.h"

#ifndef MAP_SYNC
#define MAP_SYNC 0x80000
#endif

#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif

#define MEGABYTE ((uintptr_t)1 << 20)
#define GIGABYTE ((uintptr_t)1 << 30)

/* indicates the cases in which the error cannot occur */
#define GRAN_IMPOSSIBLE "impossible"
#ifdef __linux__
	/* requested CACHE_LINE, available PAGE */
#define REQ_CL_AVAIL_PG \
	"requested granularity not available because fd doesn't point to DAX-enabled file " \
	"or kernel doesn't support MAP_SYNC flag (Linux >= 4.15)"

/* requested BYTE, available PAGE */
#define REQ_BY_AVAIL_PG REQ_CL_AVAIL_PG

/* requested BYTE, available CACHE_LINE */
#define REQ_BY_AVAIL_CL \
	"requested granularity not available because the platform doesn't support eADR"

static const char *granularity_err_msg[3][3] = {
/*		requested granularity / available granularity		*/
/* -------------------------------------------------------------------- */
/*		BYTE		CACHE_LINE		PAGE		*/
/* -------------------------------------------------------------------- */
/* BYTE */ {GRAN_IMPOSSIBLE,	REQ_BY_AVAIL_CL,	REQ_BY_AVAIL_PG},
/* CL	*/ {GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE,	REQ_CL_AVAIL_PG},
/* PAGE */ {GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE}};
#else
/* requested CACHE_LINE, available PAGE */
#define REQ_CL_AVAIL_PG \
	"the operating system doesn't provide a method of detecting granularity"

/* requested BYTE, available PAGE */
#define REQ_BY_AVAIL_PG \
	"the operating system doesn't provide a method of detecting whether the platform supports eADR"

static const char *granularity_err_msg[3][3] = {
/*		requested granularity / available granularity		*/
/* -------------------------------------------------------------------- */
/*		BYTE		CACHE_LINE		PAGE		*/
/* -------------------------------------------------------------------- */
/* BYTE */ {GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE,	REQ_BY_AVAIL_PG},
/* CL	*/ {GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE,	REQ_CL_AVAIL_PG},
/* PAGE */ {GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE,	GRAN_IMPOSSIBLE}};
#endif

/*
 * get_map_alignment -- (internal) choose the desired mapping alignment
 *
 * The smallest supported alignment is 2 megabytes because of the object
 * alignment requirements. Changing this value to 4 kilobytes constitutes a
 * layout change.
 *
 * Use 1GB page alignment only if the mapping length is at least
 * twice as big as the page size.
 */
static inline size_t
get_map_alignment(size_t len, size_t req_align)
{
	size_t align = 2 * MEGABYTE;
	if (req_align)
		align = req_align;
	else if (len >= 2 * GIGABYTE)
		align = GIGABYTE;

	return align;
}

/*
 * map_reserve -- (internal) reserve an address for mmap()
 *
 * ALSR in 64-bit Linux kernel uses 28-bit of randomness for mmap
 * (bit positions 12-39), which means the base mapping address is randomized
 * within [0..1024GB] range, with 4KB granularity.  Assuming additional
 * 1GB alignment, it results in 1024 possible locations.
 */
static int
map_reserve(size_t len, size_t alignment, void **reserv, size_t *reslen)
{
	ASSERTne(reserv, NULL);

	size_t dlength = len + alignment; /* dummy length */

	/*
	 * Create dummy mapping to find an unused region of given size.
	 * Request for increased size for later address alignment.
	 * Use MAP_PRIVATE with read-only access to simulate
	 * zero cost for overcommit accounting.  Note: MAP_NORESERVE
	 * flag is ignored if overcommit is disabled (mode 2).
	 */
	char *daddr = mmap(NULL, dlength, PROT_READ,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (daddr == MAP_FAILED) {
		ERR("!mmap MAP_ANONYMOUS");
		return PMEM2_E_ERRNO;
	}

	LOG(4, "system choice %p", daddr);
	*reserv = (void *)roundup((uintptr_t)daddr, alignment);
	/*
	 * since the last part of the reservation from (reserv + reslen == end)
	 * will be unmapped, the 'end' address has to be page-aligned.
	 * 'reserv' is already page-aligned (or even aligned to multiple of page
	 * size) so it is enough to page-align the 'reslen' value.
	 */
	*reslen = roundup(len, Pagesize);
	LOG(4, "hint %p", *reserv);

	/*
	 * The placeholder mapping is divided into few parts:
	 *
	 * 1      2         3   4                 5
	 * |......|uuuuuuuuu|rrr|.................|
	 *
	 * Addresses:
	 * 1 == daddr
	 * 2 == reserv
	 * 3 == reserv + len
	 * 4 == reserv + reslen == end (has to be page-aligned)
	 * 5 == daddr + dlength
	 *
	 * Key:
	 * - '.' is an unused part of the placeholder
	 * - 'u' is where the actual mapping lies
	 * - 'r' is what reserved as padding
	 */

	/* unmap the placeholder before the actual mapping */
	const size_t before = (uintptr_t)(*reserv) - (uintptr_t)daddr;
	if (before) {
		if (munmap(daddr, before)) {
			ERR("!munmap");
			return PMEM2_E_ERRNO;
		}
	}

	/* unmap the placeholder after the actual mapping */
	const size_t after = dlength - *reslen - before;
	void *end = (void *)((uintptr_t)(*reserv) + (uintptr_t)*reslen);
	if (after)
		if (munmap(end, after)) {
			ERR("!munmap");
			return PMEM2_E_ERRNO;
		}

	return 0;
}

/*
 * file_map -- (internal) memory map given file into memory
 * If (flags & MAP_PRIVATE) it uses just mmap. Otherwise, it tries to mmap with
 * (flags | MAP_SHARED_VALIDATE | MAP_SYNC) which allows flushing from the
 * user-space. If MAP_SYNC fails and the user did not specify it by himself it
 * falls back to the mmap with user-provided flags.
 */
static int
file_map(void *reserv, size_t len, int proto, int flags,
		int fd, off_t offset, bool *map_sync, void **base)
{
	LOG(15, "reserve %p len %zu proto %x flags %x fd %d offset %ld "
			"map_sync %p", reserv, len, proto, flags, fd, offset,
			map_sync);

	ASSERTne(map_sync, NULL);
	ASSERTne(base, NULL);

	/* try to mmap with MAP_SYNC flag */
	const int sync_flags = MAP_SHARED_VALIDATE | MAP_SYNC;
	*base = mmap(reserv, len, proto, flags | sync_flags, fd, offset);
	if (*base != MAP_FAILED) {
		LOG(4, "mmap with MAP_SYNC succeeded");
		*map_sync = true;
		return 0;
	}

	/* try to mmap with MAP_SHARED flag (without MAP_SYNC) */
	if (errno == EINVAL || errno == ENOTSUP) {
		LOG(4, "mmap with MAP_SYNC not supported");
		*base = mmap(reserv, len, proto, flags | MAP_SHARED, fd,
				offset);
		if (*base != MAP_FAILED) {
			*map_sync = false;
			return 0;
		}
	}

	ERR("!mmap");
	return PMEM2_E_ERRNO;
}

/*
 * unmap -- (internal) unmap a memory range
 */
static int
unmap(void *addr, size_t len)
{
/*
 * XXX Workaround for https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=169608
 */
#ifdef __FreeBSD__
	if (!IS_PAGE_ALIGNED((uintptr_t)addr)) {
		ERR("!munmap");
		return PMEM2_E_INVALID_ARG;
	}
#endif
	int retval = munmap(addr, len);
	if (retval < 0) {
		ERR("!munmap");
		return PMEM2_E_ERRNO;
	}

	return 0;
}

/*
 * pmem2_map -- map memory according to provided config
 */
int
pmem2_map(const struct pmem2_config *cfg, struct pmem2_map **map_ptr)
{
	LOG(3, "cfg %p map_ptr %p", cfg, map_ptr);

	int ret = 0;
	struct pmem2_map *map;
	size_t file_len;
	*map_ptr = NULL;

	if (cfg->fd == INVALID_FD) {
		ERR("the provided file descriptor is invalid");
		return PMEM2_E_FILE_HANDLE_NOT_SET;
	}

	if (cfg->requested_max_granularity == PMEM2_GRANULARITY_INVALID) {
		ERR(
			"please define the max granularity requested for the mapping");

		return PMEM2_E_GRANULARITY_NOT_SET;
	}

	/* get file size */
	ret = pmem2_config_get_file_size(cfg, &file_len);
	if (ret)
		return ret;

	/* get file type */
	enum pmem2_file_type file_type;
	os_stat_t st;
	if (os_fstat(cfg->fd, &st)) {
		ERR("!fstat");
		return PMEM2_E_ERRNO;
	}
	ret = pmem2_get_type_from_stat(&st, &file_type);
	if (ret)
		return ret;

	/* get offset */
	size_t offset;
	ret = pmem2_validate_offset(cfg, &offset);
	if (ret)
		return ret;
	os_off_t off = (os_off_t)offset;

	ASSERTeq((size_t)off, cfg->offset);

	/* map input and output variables */
	bool map_sync;
	/*
	 * MAP_SHARED - is required to mmap directly the underlying hardware
	 * MAP_FIXED - is required to mmap at exact address pointed by hint
	 */
	int flags = MAP_FIXED;
	int proto = PROT_READ | PROT_WRITE;
	void *addr;

	if (file_type == PMEM2_FTYPE_DIR) {
		ERR("the directory is not a supported file type");
		return PMEM2_E_INVALID_FILE_TYPE;
	}

	ASSERT(file_type == PMEM2_FTYPE_REG || file_type == PMEM2_FTYPE_DEVDAX);

	size_t content_length, reserved_length = 0;
	ret = pmem2_config_validate_length(cfg, file_len);
	if (ret)
		return ret;

	/* without user-provided length, map to the end of the file */
	if (cfg->length)
		content_length = cfg->length;
	else
		content_length = file_len - cfg->offset;

	const size_t alignment = get_map_alignment(content_length,
			cfg->alignment);

	/* find a hint for the mapping */
	void *reserv = NULL;
	ret = map_reserve(content_length, alignment, &reserv, &reserved_length);
	if (ret != 0) {
		LOG(1, "cannot find a contiguous region of given size");
		return ret;
	}
	ASSERTne(reserv, NULL);

	ret = file_map(reserv, content_length, proto, flags, cfg->fd, off,
			&map_sync, &addr);
	if (ret == -EACCES) {
		proto = PROT_READ;
		ret = file_map(reserv, content_length, proto, flags, cfg->fd,
				off, &map_sync, &addr);
	}

	if (ret) {
		/* unmap the reservation mapping */
		munmap(reserv, reserved_length);
		return ret;
	}

	LOG(3, "mapped at %p", addr);

	bool eADR = (pmem2_auto_flush() == 1);
	enum pmem2_granularity available_min_granularity =
		get_min_granularity(eADR, map_sync);

	if (available_min_granularity > cfg->requested_max_granularity) {
		const char *err = granularity_err_msg
			[cfg->requested_max_granularity]
			[available_min_granularity];
		if (strcmp(err, GRAN_IMPOSSIBLE) == 0)
			FATAL(
				"unhandled granularity error: available_min_granularity: %d" \
				"requested_max_granularity: %d",
				available_min_granularity,
				cfg->requested_max_granularity);
		ERR("%s", err);
		ret = PMEM2_E_GRANULARITY_NOT_SUPPORTED;
		goto err;
	}

	/* prepare pmem2_map structure */
	map = (struct pmem2_map *)pmem2_malloc(sizeof(*map), &ret);
	if (!map)
		goto err;

	map->addr = addr;
	map->reserved_length = reserved_length;
	map->content_length = content_length;
	map->effective_granularity = available_min_granularity;

	*map_ptr = map;

	VALGRIND_REGISTER_PMEM_MAPPING(map->addr, map->content_length);
	VALGRIND_REGISTER_PMEM_FILE(cfg->fd, map->addr, map->content_length, 0);

	return ret;

err:
	unmap(addr, reserved_length);
	return ret;

}

/*
 * pmem2_unmap -- unmap the specified mapping
 */
int
pmem2_unmap(struct pmem2_map **map_ptr)
{
	LOG(3, "map_ptr %p", map_ptr);

	int ret = 0;
	struct pmem2_map *map = *map_ptr;

	ret = unmap(map->addr, map->reserved_length);
	if (ret)
		return ret;

	VALGRIND_REMOVE_PMEM_MAPPING(map->addr, map->content_length);

	Free(map);
	*map_ptr = NULL;

	return ret;
}
