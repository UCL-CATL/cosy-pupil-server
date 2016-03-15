#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>

typedef struct _Data Data;
struct _Data
{
	double diameter_px;
	double timestamp;
};

typedef struct _Recorder Recorder;
struct _Recorder
{
	GQueue *data_queue;
};

static void
recorder_init (Recorder *recorder)
{
	recorder->data_queue = g_queue_new ();
}

static void
recorder_finalize (Recorder *recorder)
{
	g_queue_free_full (recorder->data_queue, g_free);
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
receive_next_part (void *socket)
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

	g_return_val_if_fail (topic != NULL && *topic == NULL, FALSE);
	g_return_val_if_fail (json_data != NULL && *json_data == NULL, FALSE);

	*topic = receive_next_part (socket);

	/* Determine if more message parts are to follow. */
	ok = zmq_getsockopt (socket, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (!more)
	{
		return FALSE;
	}

	*json_data = receive_next_part (socket);

	/* Determine if more message parts are to follow.
	 * There must be exactly two parts. If there are more, it's an error.
	 */
	ok = zmq_getsockopt (socket, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (more)
	{
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
parse_json_data (const char *json_data,
		 Recorder   *recorder)
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

int
main (void)
{
	Recorder recorder;
	void *context;
	void *subscriber;
	char *filter;
	int ok;

	context = zmq_ctx_new ();
	subscriber = zmq_socket (context, ZMQ_SUB);
	ok = zmq_connect (subscriber, "tcp://localhost:5000");
	g_assert (ok == 0);

	filter = "pupil_positions";
	ok = zmq_setsockopt (subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	g_assert (ok == 0);

	recorder_init (&recorder);

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

		if (!parse_json_data (json_data, &recorder))
		{
			g_error ("Failed to parse the JSON data.");
		}

		free (topic);
		free (json_data);
	}

	recorder_finalize (&recorder);
	zmq_close (subscriber);
	zmq_ctx_destroy (context);

	return EXIT_SUCCESS;
}
