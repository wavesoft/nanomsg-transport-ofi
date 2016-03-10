
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

/**
 * shamelessly stolen from fabtests shared.c
 * precision fixed to us
 */
static int64_t get_elapsed(const struct timespec *b, const struct timespec *a)
{
	int64_t elapsed;

	elapsed = (a->tv_sec - b->tv_sec) * 1000 * 1000 * 1000;
	elapsed += a->tv_nsec - b->tv_nsec;
	return elapsed / 1000;  // microseconds
}

/**
 * Update averages
 */
static void u_bw_update_average( struct u_bw_timing * self )
{
	double sum;
	int vmax, i;

	/* Calculate average lattency */
	sum = 0;
	vmax = U_BW_TIMING_RING_SIZE;
	if (self->lattency_index < vmax)
		vmax = self->lattency_index;
	for (i=0; i<vmax; i++) {
		sum += self->lattency_ring[i];
	}
	self->lattency_average = sum / vmax;

	/* Calculate average bandwidth */
	sum = 0;
	vmax = U_BW_TIMING_RING_SIZE;
	if (self->bandwidth_index < vmax)
		vmax = self->bandwidth_index;
	for (i=0; i<vmax; i++) {
		sum += self->bandwidth_ring[i];
	}
	self->bandwidth_average = sum / vmax;

}

/**
 * Update bandwidth counters
 */
static void u_bw_update_bandwidth( struct u_bw_timing * self, int64_t t_delta )
{
	/**
	 * Calculate:
	 * 
	 *  MBytes     10^6 Bytes
	 * -------- = ---------------
	 *    Sec      10^6 microSec
	 *
	 */
	int64_t bandwidth = (int64_t)((double)(self->bandwidth_counter / t_delta) * 1.048576);
	printf("%sBandwidth %lld MB/s (%.2lf GBit/s)\n", self->prefix, bandwidth, (double)(bandwidth * 8.0 / 1000.0));

	/* Update bandwidth */
	if (self->bandwidth_index == 0) {

		/* Initial values of lattency measurements */
		self->bandwidth_ring[0] = bandwidth;
		self->bandwidth_min = bandwidth;
		self->bandwidth_max = bandwidth;

	} else {

		/* Update min/max */
		if (bandwidth < self->bandwidth_min)
			self->bandwidth_min = bandwidth;
		if (bandwidth > self->bandwidth_max)
			self->bandwidth_max = bandwidth;

		/* Update average through ring buffer */
		self->bandwidth_ring[ self->bandwidth_index % U_BW_TIMING_RING_SIZE ] = bandwidth;

	}

	/* Increment index */
	self->bandwidth_index++;

}

/**
 * Initialize bandwidth timing structure
 */
void u_bw_init( struct u_bw_timing * self, const char * prefix )
{
	struct timespec time;
	int i;

	/* Initialize time */
	clock_gettime(CLOCK_MONOTONIC, &time);
	self->last = time;
	self->counter = time;

	/* Copy message prefix */
	self->prefix = malloc(strlen(prefix)+1);
	memcpy( self->prefix, prefix, strlen(prefix)+1 );

	/* Reset lattency properties */
	for (i=0; i<U_BW_TIMING_RING_SIZE; i++)
		self->lattency_ring[i] = 0;
	self->lattency_index = 0;
	self->lattency_min = 0;
	self->lattency_max = 0;
	self->lattency_average = 0;

	/* Reset bandwidth properties */
	for (i=0; i<U_BW_TIMING_RING_SIZE; i++)
		self->bandwidth_ring[i] = 0;
	self->bandwidth_index = 0;
	self->bandwidth_min = 0;
	self->bandwidth_max = 0;
	self->bandwidth_average = 0;
	self->bandwidth_counter = 0;

	/* Display counters every second */
	self->count_interval = 1000000;

}

/**
 * Called every time a packet is sent/received
 */
void u_bw_count( struct u_bw_timing * self, size_t len )
{
	struct timespec time;
	int64_t t_lat, t_delta;

	/* Get current time */
	clock_gettime(CLOCK_MONOTONIC, &time);

	/* Get lattency and delta */
	t_lat = get_elapsed( &self->last, &time );
	t_delta = get_elapsed( &self->counter, &time );

	/* Update lattency */
	if (self->lattency_index == 0) {

		/* Initial values of lattency measurements */
		self->lattency_ring[0] = t_lat;
		self->lattency_min = t_lat;
		self->lattency_max = t_lat;

	} else {

		/* Update min/max */
		if (t_lat < self->lattency_min)
			self->lattency_min = t_lat;
		if (t_lat > self->lattency_max)
			self->lattency_max = t_lat;

		/* Update average through ring buffer */
		self->lattency_ring[ self->lattency_index % U_BW_TIMING_RING_SIZE ] = t_lat;

	}

	/* Reset lattency timers */
	self->last = time;
	self->lattency_index++;

	/* Update bandwidth */
	self->bandwidth_counter += len;

	/* Check if we have passed interval time */
	if (t_delta > self->count_interval) {

		/* Update counter */
		u_bw_update_bandwidth( self, t_delta );

		/* Update bandwidth timers */
		self->bandwidth_counter = 0;
		self->counter = time;

		/* Update averages */
		u_bw_update_average( self );

	}

}

/**
 * Finalize counters
 */
void u_bw_finalize( struct u_bw_timing * self )
{
	struct timespec time;
	int t_delta;

	/* Update final bandwidth chunk */
	clock_gettime(CLOCK_MONOTONIC, &time);
	t_delta = get_elapsed( &self->counter, &time );
	u_bw_update_bandwidth( self, t_delta );

	/* Update averages */
	u_bw_update_average( self );

	/* Dump */
	printf("----------------------\n");
	printf("%sLattency Avg  : %lf uSec\n", self->prefix, self->lattency_average );
	printf("%sLattency Min  : %lld uSec\n", self->prefix, self->lattency_min );
	printf("%sLattency Max  : %lld uSec\n", self->prefix, self->lattency_max );
	printf("%sBandwidth Avg : %.2lf MB/s (%lf GBit/s)\n", self->prefix, 
		self->bandwidth_average, (double)(self->bandwidth_average * 8.0 / 1000.0) );
	printf("%sBandwidth Min : %lld MB/s\n", self->prefix, self->bandwidth_min );
	printf("%sBandwidth Max : %lld MB/s\n", self->prefix, self->bandwidth_max );
	printf("----------------------\n");

}
