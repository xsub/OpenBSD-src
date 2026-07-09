/*	$OpenBSD$	*/

/*
 * ZLFS zone allocator.  Skeleton.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/zlfs.h>

/*
 * In-memory state for tracking a zone.
 */
struct zlfs_zone_state {
	u_int64_t	start_lba;
	u_int64_t	wp_lba;		/* current write pointer */
	u_int64_t	capacity;
	u_int32_t	state;		/* EMPTY, IMPLICIT_OPEN, FULL, etc. */
	u_int32_t	flags;
	u_int64_t	live_bytes;	/* for garbage collection heuristics */
};

/*
 * Initialize the zone allocator.
 */
void
zlfs_alloc_init(void)
{
	/* Initialization logic for memory zone management */
}
