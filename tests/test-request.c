/* Request-reply test */

#include <zmq.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "zhelpers.h"

int
main (void)
{
	void *context;
	void *requester;
	char *request;
	int i;

	context = zmq_ctx_new ();

	requester = zmq_socket (context, ZMQ_REQ);
	zmq_connect (requester, "tcp://localhost:6000");
	printf ("requester connected\n");

	request = "start";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);
	printf ("...done.\n");

	sleep (1);

	request = "stop";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);
	printf ("...done.\n");

#if 0
	printf ("Receive reply...\n");
	reply = s_recv (requester);
	printf ("Reply received: %s\n", reply);
	free (reply);
#endif

	zmq_close (requester);
	zmq_ctx_destroy (context);
	return 0;
}
