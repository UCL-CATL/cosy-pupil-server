#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>

#define PUPIL_SERVER_ADDRESS "tcp://localhost:5000"
#define REPLIER_ENDPOINT "tcp://*:6000"

#define DEBUG FALSE

typedef struct _Data Data;
struct _Data
{
	double timestamp;
	double diameter_px;
	double confidence;
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

	GTimer *timer;

	guint record : 1;
};

static void
recorder_init (Recorder *recorder)
{
	const char *filter;
	int timeout_ms;
	int ok;

	recorder->context = zmq_ctx_new ();
	recorder->subscriber = zmq_socket (recorder->context, ZMQ_SUB);
	ok = zmq_connect (recorder->subscriber, PUPIL_SERVER_ADDRESS);
	g_assert_cmpint (ok, ==, 0);

	if (DEBUG)
	{
		/* Receive all messages. */
		filter = "";
	}
	else
	{
		filter = "pupil_positions";
	}

	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	g_assert_cmpint (ok, ==, 0);

	/* Don't block the subscriber, to prioritize the replier, to have the
	 * minimum latency between the client and server.
	 */
	timeout_ms = 0;
	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	g_assert_cmpint (ok, ==, 0);

	recorder->replier = zmq_socket (recorder->context, ZMQ_REP);
	ok = zmq_bind (recorder->replier, REPLIER_ENDPOINT);
	g_assert_cmpint (ok, ==, 0);

	/* We need to record at at least 10 Hz, so every 100 ms maximum. Setting
	 * a timeout of 10 ms should be thus a good choice. It will alternate
	 * between the subscriber and the replier every 10 ms (100 Hz).
	 * Normally the Pupil Server sends messages at 30 Hz, so we have
	 * normally the time to process all Pupil messages and change the
	 * socket to see if there is a request.
	 */
	timeout_ms = 10;
	ok = zmq_setsockopt (recorder->replier,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	g_assert_cmpint (ok, ==, 0);

	recorder->data_queue = g_queue_new ();
	recorder->timer = NULL;
	recorder->record = FALSE;
}

static void
recorder_finalize (Recorder *recorder)
{
	zmq_close (recorder->subscriber);
	recorder->subscriber = NULL;

	zmq_close (recorder->replier);
	recorder->replier = NULL;

	zmq_ctx_destroy (recorder->context);
	recorder->context = NULL;

	g_queue_free_full (recorder->data_queue, g_free);
	recorder->data_queue = NULL;

	if (recorder->timer != NULL)
	{
		g_timer_destroy (recorder->timer);
		recorder->timer = NULL;
	}
}

static Data *
data_new (void)
{
	Data *data;

	data = g_new (Data, 1);
	data->timestamp = -1.0;
	data->diameter_px = -1.0;
	data->confidence = -1.0;

	return data;
}

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
		g_warning ("A Pupil message must be in two parts.");
		return FALSE;
	}

	*json_data = receive_next_message (recorder->subscriber);

	/* Determine if more message parts are to follow.
	 * There must be exactly two parts. If there are more, it's an error.
	 */
	ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (more)
	{
		g_warning ("A Pupil message must be in two parts.");
		return FALSE;
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
	double timestamp = -1.0;
	double diameter_px = -1.0;
	double confidence = -1.0;
	gboolean found = FALSE;

	if (json_node_get_node_type (element_node) != JSON_NODE_OBJECT)
	{
		g_warning ("Expected an object inside the JSON array.");
		return;
	}

	object = json_node_get_object (element_node);

	if (json_object_has_member (object, "timestamp"))
	{
		timestamp = json_object_get_double_member (object, "timestamp");
		found = TRUE;
	}
	if (json_object_has_member (object, "diameter"))
	{
		diameter_px = json_object_get_double_member (object, "diameter");
		found = TRUE;
	}
	if (json_object_has_member (object, "confidence"))
	{
		confidence = json_object_get_double_member (object, "confidence");
		found = TRUE;
	}

	if (found)
	{
		Data *data = data_new ();
		data->timestamp = timestamp;
		data->diameter_px = diameter_px;
		data->confidence = confidence;

		g_queue_push_tail (recorder->data_queue, data);

		printf ("timestamp: %lf\n", timestamp);
		printf ("diameter: %lf\n", diameter_px);
		printf ("confidence: %lf\n", confidence);
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
	gboolean ok = TRUE;

	parser = json_parser_new ();
	json_parser_load_from_data (parser, json_data, -1, &error);

	if (error != NULL)
	{
		g_warning ("Error when parsing JSON data: %s", error->message);
		g_error_free (error);
		error = NULL;

		ok = FALSE;
		goto out;
	}

	root_node = json_parser_get_root (parser);
	if (json_node_get_node_type (root_node) != JSON_NODE_ARRAY)
	{
		g_warning ("Error: JSON root node must be an array");
		ok = FALSE;
		goto out;
	}

	array = json_node_get_array (root_node);
	json_array_foreach_element (array,
				    (JsonArrayForeach) array_foreach_cb,
				    recorder);

out:
	g_object_unref (parser);
	return ok;
}

static void
read_all_pupil_messages (Recorder *recorder)
{
	/* Continue */
	gboolean cont = TRUE;

	while (cont)
	{
		char *topic = NULL;
		char *json_data = NULL;

		if (!receive_pupil_message (recorder, &topic, &json_data))
		{
			cont = FALSE;
			goto end;
		}

		if (DEBUG)
		{
			g_print ("%s: %s\n", topic, json_data);
		}

		if (!recorder->record)
		{
			/* Flush the queue of messages, to not get them when the
			 * recording starts.
			 */
			goto end;
		}

		if (!parse_json_data (recorder, json_data))
		{
			g_warning ("Failed to parse the JSON data.");
		}

end:
		g_free (topic);
		g_free (json_data);
	}
}

static char *
recorder_start (Recorder *recorder)
{
	char *reply;

	printf ("start\n");
	recorder->record = TRUE;
	reply = g_strdup ("ack");

	if (recorder->timer == NULL)
	{
		recorder->timer = g_timer_new ();
	}
	else
	{
		g_timer_start (recorder->timer);
	}

	return reply;
}

static char *
recorder_stop (Recorder *recorder)
{
	char *reply;

	if (recorder->timer == NULL)
	{
		reply = g_strdup ("no timer");
		return reply;
	}

	g_timer_stop (recorder->timer);

	printf ("stop\n");
	recorder->record = FALSE;
	reply = g_strdup_printf ("%lf", g_timer_elapsed (recorder->timer, NULL));

	return reply;
}

static char *
receive_data (Recorder *recorder)
{
	GString *str;
	GList *l;

	if (g_queue_is_empty (recorder->data_queue))
	{
		return g_strdup ("no data");
	}

	str = g_string_new (NULL);

	for (l = recorder->data_queue->head; l != NULL; l = l->next)
	{
		Data *data = l->data;

		g_string_append_printf (str,
					"timestamp:%lf\n"
					"diameter_px:%lf\n"
					"confidence:%lf\n",
					data->timestamp,
					data->diameter_px,
					data->confidence);
	}

	return g_string_free (str, FALSE);
}

static void
read_request (Recorder *recorder)
{
	char *request;
	char *reply = NULL;

	request = receive_next_message (recorder->replier);
	if (request == NULL)
	{
		return;
	}

	if (g_str_equal (request, "start"))
	{
		reply = recorder_start (recorder);
	}
	else if (g_str_equal (request, "stop"))
	{
		reply = recorder_stop (recorder);
	}
	else if (g_str_equal (request, "receive_data"))
	{
		/* It's fine to send big messages with ZeroMQ. In our case, if
		 * the recording lasts 2 minutes, the data should be below 1MB.
		 * ZeroMQ supports data blobs from zero to gigabytes large (as
		 * soon as there is enough RAM on both sides). So 1MB should be
		 * fingers in the nose.
		 */
		reply = receive_data (recorder);

		/* TODO save the data also to a local file, in case the
		 * connection between the client and server is lost.
		 */

		g_queue_free_full (recorder->data_queue, g_free);
		recorder->data_queue = g_queue_new ();
	}
	else
	{
		printf ("Unknown request: %s\n", request);
		reply = g_strdup ("unknown request");
	}

	zmq_send (recorder->replier,
		  reply,
		  strlen (reply),
		  0);

	g_free (request);
	g_free (reply);
}

int
main (void)
{
	Recorder recorder;

	recorder_init (&recorder);

	while (TRUE)
	{
		read_all_pupil_messages (&recorder);
		read_request (&recorder);
	}

	recorder_finalize (&recorder);

	return EXIT_SUCCESS;
}
