#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>

#define PUPIL_SERVER_ADDRESS "tcp://localhost:5000"
#define REPLIER_ENDPOINT "tcp://*:6000"

typedef struct _Data Data;
struct _Data
{
	double diameter_px;
	double timestamp;
};

typedef struct _Recorder Recorder;
struct _Recorder
{
	/* The zeromq context. */
	void *context;

	/* The subscriber to listen to the Pupil Broadcast Server. */
	void *subscriber;

	/* The replier, to listen and reply to some requests coming from another
	 * program than the Pupil (in our case, a Matlab script running on
	 * another computer).
	 */
	void *replier;

	/* Contains a list of recorded Data*. */
	GQueue *data_queue;
};

static void
recorder_init (Recorder *recorder)
{
	const char *filter;
	int ok;

	/* We need to record at at least 10 Hz, so every 100 ms maximum. Setting
	 * a timeout of 10 ms should be thus a good choice. It will alternate
	 * between the subscriber and the replier every 10 ms (100 Hz).
	 * Normally the Pupil Server sends messages at 30 Hz, so we have
	 * normally the time to process all Pupil messages and change the
	 * socket to see if there is a request.
	 */
	int timeout_ms = 10;

	recorder->context = zmq_ctx_new ();
	recorder->subscriber = zmq_socket (recorder->context, ZMQ_SUB);
	ok = zmq_connect (recorder->subscriber, PUPIL_SERVER_ADDRESS);
	g_assert (ok == 0);

	filter = "pupil_positions";
	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	g_assert (ok == 0);

	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	g_assert (ok == 0);

	recorder->replier = zmq_socket (recorder->context, ZMQ_REP);
	ok = zmq_bind (recorder->replier, REPLIER_ENDPOINT);
	g_assert (ok == 0);

	ok = zmq_setsockopt (recorder->replier,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	g_assert (ok == 0);

	recorder->data_queue = g_queue_new ();
}

static void
recorder_finalize (Recorder *recorder)
{
	g_queue_free_full (recorder->data_queue, g_free);
	recorder->data_queue = NULL;

	zmq_close (recorder->replier);
	recorder->replier = NULL;

	zmq_close (recorder->subscriber);
	recorder->subscriber = NULL;

	zmq_ctx_destroy (recorder->context);
	recorder->context = NULL;
}

static Data *
data_new (void)
{
	Data *data;

	data = g_new (Data, 1);
	data->diameter_px = -1.0;
	data->timestamp = -1.0;

	return data;
}

/* Receives the next zmq message part as a string.
 * Free the return value with free() when no longer needed.
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
		str = strndup ((char *) raw_data, n_bytes);
	}

	ok = zmq_msg_close (&msg);
	g_return_val_if_fail (ok == 0, NULL);

	return str;
}

/* Receive a Pupil message from the Pupil Broadcast Server plugin.
 * It must be a multi-part message, with exactly two parts: the topic and the
 * JSON data.
 * If successful, TRUE is returned.
 * Regardless of the return value, you need to free *topic and *json_data when
 * no longer needed.
 */
static gboolean
receive_pupil_message (Recorder  *recorder,
		       char     **topic,
		       char     **json_data)
{
	int64_t more;
	size_t more_size = sizeof (more);
	int ok;

	g_return_val_if_fail (topic != NULL && *topic == NULL, FALSE);
	g_return_val_if_fail (json_data != NULL && *json_data == NULL, FALSE);

	*topic = receive_next_message (recorder->subscriber);
	if (*topic == NULL)
	{
		/* Timeout, no messages. */
		return FALSE;
	}

	/* Determine if more message parts are to follow. */
	ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (!more)
	{
		g_error ("A Pupil message must be in two parts.");
	}

	*json_data = receive_next_message (recorder->subscriber);

	/* Determine if more message parts are to follow.
	 * There must be exactly two parts. If there are more, it's an error.
	 */
	ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (more)
	{
		g_error ("A Pupil message must be in two parts.");
	}

	return TRUE;
}

static void
array_foreach_cb (JsonArray *array,
		  guint      index,
		  JsonNode  *element_node,
		  Recorder  *recorder)
{
	JsonObject *object;
	double diameter_px = -1.0;
	double timestamp = -1.0;
	gboolean found = FALSE;

	if (json_node_get_node_type (element_node) != JSON_NODE_OBJECT)
	{
		g_error ("Error: expected an object inside the JSON array");
		return;
	}

	object = json_node_get_object (element_node);

	if (json_object_has_member (object, "diameter"))
	{
		diameter_px = json_object_get_double_member (object, "diameter");
		found = TRUE;
	}
	if (json_object_has_member (object, "timestamp"))
	{
		timestamp = json_object_get_double_member (object, "timestamp");
		found = TRUE;
	}

	if (found)
	{
		Data *data = data_new ();
		data->diameter_px = diameter_px;
		data->timestamp = timestamp;

		g_queue_push_tail (recorder->data_queue, data);

		printf ("diameter: %lf\n", diameter_px);
		printf ("timestamp: %lf\n", timestamp);
	}
}

/* Parses the JSON data to extract the diameter of the pupil (in pixels), and
 * the timestamp.
 * Returns TRUE if successful.
 */
static gboolean
parse_json_data (Recorder   *recorder,
		 const char *json_data)
{
	JsonParser *parser;
	JsonNode *root_node;
	JsonArray *array;
	GError *error = NULL;

	parser = json_parser_new ();
	json_parser_load_from_data (parser, json_data, -1, &error);

	if (error != NULL)
	{
		g_warning ("Error when parsing JSON data: %s", error->message);
		g_clear_error (&error);
		return FALSE;
	}

	root_node = json_parser_get_root (parser);
	if (json_node_get_node_type (root_node) != JSON_NODE_ARRAY)
	{
		g_warning ("Error: JSON root node must be an array");
		return FALSE;
	}

	array = json_node_get_array (root_node);
	json_array_foreach_element (array,
				    (JsonArrayForeach) array_foreach_cb,
				    recorder);

	g_object_unref (parser);

	return TRUE;
}

static void
read_all_pupil_messages (Recorder *recorder)
{
	while (TRUE)
	{
		char *topic = NULL;
		char *json_data = NULL;

		if (receive_pupil_message (recorder, &topic, &json_data))
		{
			printf ("Topic: %s\n", topic);
			printf ("JSON data: %s\n", json_data);

			if (!parse_json_data (recorder, json_data))
			{
				g_error ("Failed to parse the JSON data.");
			}
		}

		free (topic);
		free (json_data);
	}
}

int
main (void)
{
	Recorder recorder;

	recorder_init (&recorder);

	read_all_pupil_messages (&recorder);

	recorder_finalize (&recorder);

	return EXIT_SUCCESS;
}
