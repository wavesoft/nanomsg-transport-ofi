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

//////////////////////////////////////////////////////////////////////////////////////////
// Entry Point
//////////////////////////////////////////////////////////////////////////////////////////

void help()
{
	printf("Usage:   fab server <node> [<service>]\n");
	printf("         fab client <node> [<service>]\n");
	printf("\n");
}

int main(int argc, char ** argv)
{
	int ret;

	/* Validate arguments */
	if (argc < 3) {
		printf("ERROR: Too few arguments!\n");
		help();
		return 1;
	}

	/* Create and initialize OFI structure */
	struct ofi_resources ofi = {0};
	ofi_alloc( &ofi, FI_EP_DGRAM );

	/* Active endoint */
	struct ofi_active_endpoint ep = {0};

	/* Get port if specified */
	const char * node = argv[2]; 
	const char * service = "5125";
	if (argc >= 4) service = argv[3];

	// ofi.hints->fabric_attr->name = "usnic_0";

	/* Setup OFI resources */
	if (!strcmp(argv[1], "server")) {

		printf("OFI: Listening on %s:%s\n", node, service );

		/* Initialize an RDM endpoint (bind there) */
		ret = ofi_init_connectionless( &ofi, &ep, FI_SOURCE, FI_SOCKADDR, node, service );
		if (ret)
			return ret;

		/* Receive data */
		printf("Receiving data...");
		ret = ofi_rx( &ep, MAX_MSG_SIZE );
		if (ret) {
			printf("Error sending message!\n");
			return 1;
		}
		printf("'%s'\n", ep.rx_buf );


	} else if (!strcmp(argv[1], "client")) {

		printf("OFI: Connecting to %s:%s\n", node, service );

		/* Initialize an RDM endpoint (connect there) */
		ret = ofi_init_connectionless( &ofi, &ep, 0, FI_SOCKADDR, node, service );
		if (ret)
			return ret;

		/* Add a remote address to the AV */
		ret = ofi_add_remote( &ofi, &ep, node, service );
		if (ret)
			return ret;

		/* Send data */
		printf("Sending data...");
		sprintf( ep.tx_buf, "Hello World" );
		ret = ofi_tx( &ep, 12 );
		if (ret) {
			printf("Error sending message!\n");
			return 1;
		}
		printf("OK\n");


	} else {
		printf("ERROR: Unknown action! Second argument must be 'server' or 'client'\n");
		help();
		return 1;

	}


	return 0;
}