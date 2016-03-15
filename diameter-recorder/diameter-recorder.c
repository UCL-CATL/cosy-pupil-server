#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <zmq.h>

/* Receives the next zmq message part as a string.
 * Free the return value with free() when no longer needed.
 */
static char *
receive_next_part (void *socket)
{
	zmq_msg_t msg;
	int n_bytes;
	char *str = NULL;
	int ok;

	ok = zmq_msg_init (&msg);
	assert (ok == 0);

	n_bytes = zmq_msg_recv (&msg, socket, 0);
	if (n_bytes > 0)
	{
		void *raw_data;

		raw_data = zmq_msg_data (&msg);
		str = strndup ((char *) raw_data, n_bytes);
	}

	ok = zmq_msg_close (&msg);
	assert (ok == 0);

	return str;
}

/* Receive a Pupil message from the Pupil Broadcast Server plugin.
 * It must be a multi-part message, with exactly two parts: the topic and the
 * JSON data.
 * If the number of parts is different than 2, FALSE is returned. If successful,
 * TRUE is returned.
 * Either way, you need to free *topic and *json_data when no longer needed.
 */
static gboolean
receive_pupil_message (void  *socket,
		       char **topic,
		       char **json_data)
{
	int64_t more;
	size_t more_size = sizeof (more);
	int ok;

	assert (topic != NULL && *topic == NULL);
	assert (json_data != NULL && *json_data == NULL);

	*topic = receive_next_part (socket);

	/* Determine if more message parts are to follow. */
	ok = zmq_getsockopt (socket, ZMQ_RCVMORE, &more, &more_size);
	assert (ok == 0);
	if (!more)
	{
		return FALSE;
	}

	*json_data = receive_next_part (socket);

	/* Determine if more message parts are to follow.
	 * There must be exactly two parts. If there are more, it's an error.
	 */
	ok = zmq_getsockopt (socket, ZMQ_RCVMORE, &more, &more_size);
	assert (ok == 0);
	if (more)
	{
		return FALSE;
	}

	return TRUE;
}

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
	while (TRUE)
	{
		char *topic = NULL;
		char *json_data = NULL;

		if (!receive_pupil_message (subscriber, &topic, &json_data))
		{
			g_error ("A Pupil message must be in two parts.");
		}

		printf ("Topic: %s\n", topic);
		printf ("JSON data: %s\n", json_data);

		free (topic);
		free (json_data);
	}

	zmq_close (subscriber);
	zmq_ctx_destroy (context);
	return EXIT_SUCCESS;
}
