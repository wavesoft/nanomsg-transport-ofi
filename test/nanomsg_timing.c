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
#include <stdio.h>

#include <nanomsg/nn.h>
#include <nanomsg/pair.h>

#include "common.h"

#define MSG_LEN 	10240
#define ITERATIONS 	10000

#define DIRECTION_IN  0
#define DIRECTION_OUT 1

#define NODE0 "node0"
#define NODE1 "node1"

/**
 */
int run_tests( int sock, int direction, size_t msg_len )
{
	struct u_bw_timing bw;
	void * msg = NULL;
	int iterations = ITERATIONS;
	int sz_n, i;

	// When sending, start counting before transmittion
	if (direction == DIRECTION_OUT)
		u_bw_init( &bw, "OUT: ");


	// Exchange messages
	for (i=0; i<iterations; i++) {
		
		// Send or receive
		if (direction == DIRECTION_OUT) {

			// Alloc message
			msg = nn_allocmsg( msg_len, 0 );

			// Send message
			printf("-- Sending %i\n", i);
			sz_n = nn_send (sock, &msg, NN_MSG, 0);
			assert( sz_n == msg_len );
            if (sz_n != msg_len) {
	            printf("!! Sent %d instead of %zu\n", sz_n, msg_len);
	        }
			printf("-- Sent %i\n", i);
			u_bw_count( &bw, sz_n );

		} else {

			// Receive message
			printf("-- Receiving %i\n", i);
			sz_n = nn_recv (sock, &msg, NN_MSG, 0);
            if (sz_n != msg_len) {
	            printf("!! Received %d instead of %zu\n", sz_n, msg_len);
	        }
			nn_freemsg (msg);
			printf("-- Received %i\n", i);

			// When receiving, start counting after first receive
			if (i == 0) {
				u_bw_init( &bw, "IN: ");
			} else {
				u_bw_count( &bw, sz_n );
			}

		}

	}

	// Calculate overall lattency
	u_bw_finalize( &bw );

	// Wait 500 ms (hack to flush queue)
	usleep(500000);

	// Display bandwidth/lattency results
	u_bw_display( &bw );

    return 0;
}

/**
 * Firtst node
 */
int node0 ( const char *url, size_t msg_len )
{
	int sock = nn_socket (AF_SP, NN_PAIR);
    int len = msg_len;
	assert (sock >= 0);
    assert (!nn_setsockopt(sock, NN_SOL_SOCKET, NN_RCVBUF, &len, sizeof(len)) );
	assert (nn_bind (sock, url) >= 0);
	printf("TIM: I will be receiving\n");
	run_tests(sock, DIRECTION_IN, msg_len);
	printf(">>> SHUTDOWN\n");
	nn_shutdown (sock, 0);
	return 0;
}

/**
 * Second node
 */
int node1 ( const char *url, size_t msg_len )
{
	int sock = nn_socket (AF_SP, NN_PAIR);
    int len = msg_len;
	assert (sock >= 0);
    assert (!nn_setsockopt(sock, NN_SOL_SOCKET, NN_SNDBUF, &len, sizeof(len)) );
	assert (nn_connect (sock, url) >= 0);
	printf("TIM: I will be sending\n");
	run_tests(sock, DIRECTION_OUT, msg_len);
	printf(">>> SHUTDOWN\n");
	nn_shutdown (sock, 0);
	return 0;
}

/**
 * Help
 */
void help() {
	fprintf (stderr, "Usage: pair %s|%s <URL> <ARG> ...\n",
	       NODE0, NODE1);
}

/**
 * Entry point
 */
int main (const int argc, const char **argv)
{
	int msg_size = MSG_LEN;
	if (argc < 3) {
		help();
		return 1;
	}
	if (argc > 3) {
		msg_size = atoi(argv[3]);
	}

	if (strncmp (NODE0, argv[1], strlen (NODE0)) == 0 && argc > 1)
		return node0 (argv[2], msg_size);
	else if (strncmp (NODE1, argv[1], strlen (NODE1)) == 0 && argc > 1)
		return node1 (argv[2], msg_size);
	else
	{
		help();
		return 1;
	}

	return 0;
}
