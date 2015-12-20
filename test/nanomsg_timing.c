/**
 * NanoMsg libFabric Transport - Shared Functions
 * Copyright (c) 2015 Ioannis Charalampidis
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <unistd.h>
#include <assert.h>
#include <libc.h>
#include <stdio.h>

#include <time.h>
#include <sys/time.h>

#include <nanomsg/nn.h>
#include <nanomsg/pair.h>

#define MSG_LEN 1024
#define DIRECTION_IN  0
#define DIRECTION_OUT 1

#define NODE0 "node0"
#define NODE1 "node1"

const char msg_buffer[MSG_LEN];

/* Missing definitions on OSX */
#if defined(__APPLE__) && defined(__MACH__)

/* Missing types */
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_REALTIME_COARSE 0
#define CLOCK_MONOTONIC 0

/* OSX Dues not have clock_getttime */
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
	int retval;
	struct timeval tv;

	retval = gettimeofday(&tv, NULL);

	tp->tv_sec = tv.tv_sec;
	tp->tv_nsec = tv.tv_usec * 1000;

	return retval;
}
#endif

/**
 * shamelessly stolen from fabtests shared.c
 * precision fixed to us
 */
int64_t get_elapsed(const struct timespec *b, const struct timespec *a)
{
	int64_t elapsed;

	elapsed = (a->tv_sec - b->tv_sec) * 1000 * 1000 * 1000;
	elapsed += a->tv_nsec - b->tv_nsec;
	return elapsed / 1000;  // microseconds
}

/**
 */
int run_tests( int sock, int direction )
{
	char *buf = NULL;
	struct timespec t0, t1;
	int iterations = 10000;
	int sz_n, i;

	// When sending, start counting before transmittion
	if (direction == DIRECTION_OUT)
		clock_gettime(CLOCK_MONOTONIC, &t0);

	// Exchange messages
	for (i=0; i<iterations; i++) {
		
		// Send or receive
		if (direction == DIRECTION_OUT) {

			// Send message
			sz_n = nn_send (sock, msg_buffer, MSG_LEN, 0);
			assert( sz_n == MSG_LEN );

		} else {

			// Receive message
			sz_n = nn_recv (sock, &buf, NN_MSG, 0);
			assert( sz_n == MSG_LEN );
			nn_freemsg (buf);

			// When receiving, start counting after first receive
			if (i == 0)
				clock_gettime(CLOCK_MONOTONIC, &t0);

		}

	}

	// Calculate overall lattency
	clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("TIM: Time per message: %8.2f us\n", get_elapsed(&t0, &t1)/i/2.0);
    return 0;
}

/**
 * Firtst node
 */
int node0 (const char *url)
{
	int sock = nn_socket (AF_SP, NN_PAIR);
	assert (sock >= 0);
	assert (nn_bind (sock, url) >= 0);
	printf("TIM: I will be receiving\n");
	run_tests(sock, DIRECTION_IN);
	return nn_shutdown (sock, 0);
}

/**
 * Second node
 */
int node1 (const char *url)
{
	int sock = nn_socket (AF_SP, NN_PAIR);
	assert (sock >= 0);
	assert (nn_connect (sock, url) >= 0);
	printf("TIM: I will be sending\n");
	run_tests(sock, DIRECTION_OUT);
	return nn_shutdown (sock, 0);
}

/**
 * Entry point
 */
int main (const int argc, const char **argv)
{
	if (strncmp (NODE0, argv[1], strlen (NODE0)) == 0 && argc > 1)
		return node0 (argv[2]);
	else if (strncmp (NODE1, argv[1], strlen (NODE1)) == 0 && argc > 1)
		return node1 (argv[2]);
	else
	{
		fprintf (stderr, "Usage: pair %s|%s <URL> <ARG> ...\n",
		       NODE0, NODE1);
		return 1;
	}
	return 0;
}
