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
	char *reply;
	int i;

	context = zmq_ctx_new ();

	requester = zmq_socket (context, ZMQ_REQ);
	zmq_connect (requester, "tcp://localhost:6000");

	request = "start";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);

	reply = s_recv (requester);
	printf ("Reply received: %s\n", reply);
	free (reply);

	sleep (10);

	request = "stop";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);

	reply = s_recv (requester);
	printf ("Reply received: %s\n", reply);
	free (reply);

	zmq_close (requester);
	zmq_ctx_destroy (context);
	return 0;
}
