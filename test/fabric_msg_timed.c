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

#include <stdio.h>
#include <string.h>

#include <hlapi.h>
#include <time.h>

//////////////////////////////////////////////////////////////////////////////////////////
// Entry Point
//////////////////////////////////////////////////////////////////////////////////////////

struct timespec t0, t1;
int iterations = 10000;

void help()
{
	printf("Usage:   fab server <node> [<service>]\n");
	printf("         fab client <node> [<service>]\n");
	printf("\n");
}

// shamelessly stolen from fabtests shared.c
// precision fixed to us
int64_t get_elapsed_us(const struct timespec *b, const struct timespec *a)
{
  int64_t elapsed;

  elapsed = (a->tv_sec - b->tv_sec) * 1000 * 1000 * 1000;
  elapsed += a->tv_nsec - b->tv_nsec;
  return elapsed / 1000;  // microseconds
}


#define MSG_LEN 1024

int main(int argc, char ** argv)
{
	int ret;
	int i;

	/* Validate arguments */
	if (argc < 3) {
		printf("ERROR: Too few arguments!\n");
		help();
		return 1;
	}

	/* Create and initialize OFI structure */
	struct ofi_resources ofi = {0};
	ofi_alloc( &ofi, FI_EP_MSG );

	/* Get port if specified */
	const char * node = argv[2]; 
	const char * service = "5125";
	if (argc >= 4) service = argv[3];

	/* Setup OFI resources */
	if (!strcmp(argv[1], "server")) {

		/* Prepare endpoints */
		struct ofi_passive_endpoint pep = {0};
		struct ofi_active_endpoint ep = {0};

		printf("OFI: Listening on %s:%s\n", node, service );

		/* Create a server */
		ret = ofi_init_server( &ofi, &pep, FI_SOCKADDR, node, service );
		if (ret)
			return ret;

		/* Listen for connections */
		ret = ofi_server_accept( &ofi, &pep, &ep );
		if (ret)
			return ret;

		/* Setup memory region */
		ret = ofi_active_ep_init_mr( &ofi, &ep, 1024, 1024 );
		if (ret)
			return ret;

		sprintf( ep.tx_buf, "Hello World" );

		/* Receive data */
		//printf("Receiving data...");
		for (i=0; i<iterations; i++) {

		  ret = ofi_rx( &ep, MAX_MSG_SIZE );
		  if (ret) {
			printf("Error receiving message!\n");
			return 1;
		  }

		  // start time measurement on first receive
		  if (i==0)
		    clock_gettime(CLOCK_MONOTONIC, &t0);


		//printf("'%s'\n", ep.rx_buf );

		/* Send data */
		//printf("Sending data...");
		  sprintf( ep.tx_buf, "Hello World" );
		  ret = ofi_tx( &ep, MSG_LEN );
		  if (ret) {
			printf("Error sending message!\n");
			return 1;
		  }
		}
		printf("OK %d\n", i);
		clock_gettime(CLOCK_MONOTONIC, &t1);
                printf("time per message: %8.2f us\n", get_elapsed_us(&t0, &t1)/i/2.0);
		

	} else if (!strcmp(argv[1], "client")) {

		/* Prepare endpoints */
		struct ofi_active_endpoint ep = {0};

		printf("OFI: Connecting to %s:%s\n", node, service );

		/* Create a client & connect */
		ret = ofi_init_client( &ofi, &ep, FI_SOCKADDR, node, service );
		if (ret)
			return ret;

		/* Setup memory region */
		ret = ofi_active_ep_init_mr( &ofi, &ep, 1024, 1024 );
		if (ret)
			return ret;

		sprintf( ep.tx_buf, "Hello World" );

		// start time measurement
		clock_gettime(CLOCK_MONOTONIC, &t0);

		/* Send data */
		//printf("Sending data...");
		for (i=0; i<iterations; i++) {

		  ret = ofi_tx( &ep, MSG_LEN );
		  if (ret) {
			printf("Error sending message!\n");
			return 1;
		  }
		
		  //printf("OK\n");

		/* Receive data */
		//printf("Receiving data...");
		  ret = ofi_rx( &ep, MAX_MSG_SIZE );
		  if (ret) {
			printf("Error receiving message!\n");
			return 1;
		  }
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("'%s' %d\n", ep.rx_buf, i );
		printf("time per message: %8.2f us\n", get_elapsed_us(&t0, &t1)/i/2.0);


	} else {
		printf("ERROR: Unknown action! Second argument must be 'server' or 'client'\n");
		help();
		return 1;

	}

	return 0;
}
