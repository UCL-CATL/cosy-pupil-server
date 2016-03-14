#include "zhelpers.h"
#include <stdio.h>

int
main (void)
{
	void *context;
	void *subscriber;
	char *filter;
	int ok;

	context = zmq_ctx_new ();
	subscriber = zmq_socket (context, ZMQ_SUB);
	printf ("connecting...\n");
	ok = zmq_connect (subscriber, "tcp://localhost:5000");
	assert (ok == 0);
	printf ("connected.\n");

	printf ("set filter...\n");
	filter = "pupil_positions";
	ok = zmq_setsockopt (subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	assert (ok == 0);
	printf ("filter set.\n");

	printf ("receiving messages...\n");
	while (1)
	{
		char *msg = s_recv (subscriber);
		printf ("Message:\n%s\n", msg);
		free (msg);
	}

	zmq_close (subscriber);
	zmq_ctx_destroy (context);
	return 0;
}
