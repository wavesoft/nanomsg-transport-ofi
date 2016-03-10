

#include <time.h>
#include <sys/time.h>

/* Platform-specific customisations */
#ifdef __APPLE__
#include <libc.h>
#include "../src/transports/ofi/platf/osx.h"
/* (Implementation in libfabric) */
#else
#include <inttypes.h>
#endif

#define U_BW_TIMING_RING_SIZE	500

/**
 * Timing functions
 */
struct u_bw_timing {

	/* Message prefix */
	char * 				prefix;
	int 				count_interval;
	uint8_t 			status;

	/* Time of last call and time since last counter */
	struct timespec		last;
	struct timespec		counter;

	/* Lattency counters */
	int64_t 			lattency_ring[U_BW_TIMING_RING_SIZE];
	int 				lattency_index;
	int64_t				lattency_min;
	int64_t				lattency_max;
	double				lattency_average;

	/* Bandwidth counters */
	int64_t 			bandwidth_counter;
	int64_t 			bandwidth_ring[U_BW_TIMING_RING_SIZE];
	int 				bandwidth_index;
	int64_t				bandwidth_min;
	int64_t				bandwidth_max;
	double				bandwidth_average;

};

void u_bw_init( struct u_bw_timing * self, const char * prefix );
void u_bw_count( struct u_bw_timing * self, size_t len );
void u_bw_finalize( struct u_bw_timing * self );
