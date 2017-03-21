/* Request-reply test */

#include <glib.h>
#include <zmq.h>
#include <string.h>
#include <unistd.h>

/* Receives the next zmq message part as a string.
 * Free the return value with g_free() when no longer needed.
 */
static char *
receive_next_message (void *socket)
{
	zmq_msg_t msg;
	int n_bytes;
	char *str = NULL;
	int ok;

	ok = zmq_msg_init (&msg);
	g_return_val_if_fail (ok == 0, NULL);

	n_bytes = zmq_msg_recv (&msg, socket, 0);
	if (n_bytes > 0)
	{
		void *raw_data;

		raw_data = zmq_msg_data (&msg);
		str = g_strndup (raw_data, n_bytes);
	}

	ok = zmq_msg_close (&msg);
	if (ok != 0)
	{
		g_free (str);
		g_return_val_if_reached (NULL);
	}

	return str;
}

int
main (void)
{
	void *context;
	void *requester;
	char *request;
	char *reply;

	context = zmq_ctx_new ();

	requester = zmq_socket (context, ZMQ_REQ);
	zmq_connect (requester, "tcp://localhost:6000");

	request = "start";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);

	reply = receive_next_message (requester);
	printf ("Reply received: %s\n", reply);
	g_free (reply);

	sleep (10);

	request = "stop";
	printf ("Send request: %s ...\n", request);
	zmq_send (requester, request, strlen (request), 0);

	reply = receive_next_message (requester);
	printf ("Reply received: %s\n", reply);
	g_free (reply);

	zmq_close (requester);
	zmq_ctx_destroy (context);
	return 0;
}
