/*
 * This file is part of cosy-pupil-server.
 *
 * Copyright (C) 2016 - Université Catholique de Louvain
 *
 * cosy-pupil-server is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * cosy-pupil-server is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * cosy-pupil-server.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Sébastien Wilmet
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>

#define PUPIL_SERVER_ADDRESS "tcp://localhost:5000"
#define PUPIL_REMOTE_ADDRESS "tcp://localhost:50020"
#define REPLIER_ENDPOINT "tcp://*:6000"

#define DEBUG FALSE

typedef struct _Data Data;
struct _Data
{
	double timestamp;
	double gaze_confidence;
	double gaze_norm_pos_x;
	double gaze_norm_pos_y;
	double pupil_confidence;
	double pupil_diameter_px;
};

typedef struct _Recorder Recorder;
struct _Recorder
{
	/* The zeromq context. */
	void *context;

#if 0
	/* The subscriber to listen to the Pupil Broadcast Server. */
	void *subscriber;
#endif

	/* The requester to the Pupil Remote plugin. */
	void *pupil_remote;

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
#if 0
	const char *filter;
#endif
	int timeout_ms;
	int ok;

	recorder->context = zmq_ctx_new ();

#if 0
	recorder->subscriber = zmq_socket (recorder->context, ZMQ_SUB);
	ok = zmq_connect (recorder->subscriber, PUPIL_SERVER_ADDRESS);
	if (ok != 0)
	{
		g_error ("Error when connecting to Pupil Server: %s", g_strerror (errno));
	}

	if (DEBUG)
	{
		/* Receive all messages. */
		filter = "";
	}
	else
	{
		filter = "gaze_positions";
	}

	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	if (ok != 0)
	{
		g_error ("Error when setting zmq socket option for the subscriber to the Pupil Server: %s",
			 g_strerror (errno));
	}

	/* Don't block the subscriber, to prioritize the replier, to have the
	 * minimum latency between the client and server.
	 */
	timeout_ms = 0;
	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	if (ok != 0)
	{
		g_error ("Error when setting zmq socket option for the subscriber to the Pupil Server: %s",
			 g_strerror (errno));
	}
#endif

	recorder->pupil_remote = zmq_socket (recorder->context, ZMQ_REQ);
	ok = zmq_connect (recorder->pupil_remote, PUPIL_REMOTE_ADDRESS);
	if (ok != 0)
	{
		g_error ("Error when connecting to Pupil Remote: %s", g_strerror (errno));
	}

	/* We should receive the reply almost directly, it's on the same
	 * computer. Setting a timeout permits to know if we can't communicate
	 * with the Pupil Remote plugin.
	 */
	timeout_ms = 1000;
	ok = zmq_setsockopt (recorder->pupil_remote,
			     ZMQ_RCVTIMEO,
			     &timeout_ms,
			     sizeof (int));
	if (ok != 0)
	{
		g_error ("Error when setting zmq socket option for the Pupil Remote: %s",
			 g_strerror (errno));
	}

	recorder->replier = zmq_socket (recorder->context, ZMQ_REP);
	ok = zmq_bind (recorder->replier, REPLIER_ENDPOINT);
	if (ok != 0)
	{
		g_error ("Error when creating zmq socket at \"" REPLIER_ENDPOINT "\": %s.\n"
			 "Is another external-recorder process running?",
			 g_strerror (errno));
	}

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
	if (ok != 0)
	{
		g_error ("Error when setting zmq socket option for the replier: %s",
			 g_strerror (errno));
	}

	recorder->data_queue = g_queue_new ();
	recorder->timer = NULL;
	recorder->record = FALSE;
}

static void
recorder_finalize (Recorder *recorder)
{
#if 0
	zmq_close (recorder->subscriber);
	recorder->subscriber = NULL;
#endif

	zmq_close (recorder->pupil_remote);
	recorder->pupil_remote = NULL;

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

#if 0
static Data *
data_new (void)
{
	return g_new0 (Data, 1);
}
#endif

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

#if 0
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
		g_warning ("A Pupil message must be in two parts. Only one part received.");
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
		/* Flush queue, to not receive those parts the next time this
		 * function is called.
		 */
		while (more)
		{
			char *msg;

			msg = receive_next_message (recorder->subscriber);
			g_free (msg);

			ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
			g_return_val_if_fail (ok == 0, FALSE);
		}

		g_warning ("A Pupil message must be in two parts. More than two parts received.");
		return FALSE;
	}

	return TRUE;
}

/* The 'base' node contains the pupil_positions, i.e. the data from the eye
 * camera.
 */
static gboolean
parse_base_node (JsonNode *base_node,
		 double   *pupil_confidence,
		 double   *pupil_diameter_px)
{
	JsonArray *base_array;
	guint array_length;
	gboolean found = FALSE;

	g_assert (pupil_confidence != NULL);
	g_assert (pupil_diameter_px != NULL);

	if (json_node_get_node_type (base_node) != JSON_NODE_ARRAY)
	{
		g_warning ("Expected a JSON array for the 'base' object member.");
		return FALSE;
	}

	base_array = json_node_get_array (base_node);
	array_length = json_array_get_length (base_array);

	if (array_length >= 1)
	{
		JsonNode *array_element = json_array_get_element (base_array, 0);

		if (json_node_get_node_type (array_element) == JSON_NODE_OBJECT)
		{
			JsonObject *base_object = json_node_get_object (array_element);

			if (json_object_has_member (base_object, "confidence"))
			{
				*pupil_confidence = json_object_get_double_member (base_object, "confidence");
				found = TRUE;
			}

			if (json_object_has_member (base_object, "diameter"))
			{
				*pupil_diameter_px = json_object_get_double_member (base_object, "diameter");
				found = TRUE;
			}
		}
		else
		{
			g_warning ("Expected an object inside the 'base' JSON array.");
		}
	}

	if (array_length > 1)
	{
		g_warning ("Expected only one element in the 'base' JSON array. "
			   "Got %u elements. Only the first element has been read.", array_length);
	}

	return found;
}

static void
array_foreach_cb (JsonArray *array,
		  guint      index,
		  JsonNode  *element_node,
		  Recorder  *recorder)
{
	JsonObject *object;
	double timestamp = -1.0;
	double gaze_confidence = -1.0;
	double gaze_norm_pos_x = -1.0;
	double gaze_norm_pos_y = -1.0;
	double pupil_confidence = -1.0;
	double pupil_diameter_px = -1.0;
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

	if (json_object_has_member (object, "confidence"))
	{
		gaze_confidence = json_object_get_double_member (object, "confidence");
		found = TRUE;
	}

	if (json_object_has_member (object, "norm_pos"))
	{
		JsonNode *norm_pos_node = json_object_get_member (object, "norm_pos");

		if (json_node_get_node_type (norm_pos_node) == JSON_NODE_ARRAY)
		{
			JsonArray *norm_pos_array = json_node_get_array (norm_pos_node);
			guint array_length = json_array_get_length (norm_pos_array);

			if (array_length == 2)
			{
				gaze_norm_pos_x = json_array_get_double_element (norm_pos_array, 0);
				gaze_norm_pos_y = json_array_get_double_element (norm_pos_array, 1);
				found = TRUE;
			}
			else if (array_length != 0)
			{
				g_warning ("Expected zero or two elements inside the norm_pos JSON array. Got %u elements.", array_length);
			}
		}
		else
		{
			g_warning ("Expected a JSON array inside norm_pos.");
		}
	}

	if (json_object_has_member (object, "base"))
	{
		JsonNode *base_node = json_object_get_member (object, "base");

		if (parse_base_node (base_node, &pupil_confidence, &pupil_diameter_px))
		{
			found = TRUE;
		}
	}

	if (found)
	{
		Data *data = data_new ();

		data->timestamp = timestamp;
		data->gaze_confidence = gaze_confidence;
		data->gaze_norm_pos_x = gaze_norm_pos_x;
		data->gaze_norm_pos_y = gaze_norm_pos_y;
		data->pupil_confidence = pupil_confidence;
		data->pupil_diameter_px = pupil_diameter_px;

		g_print ("diameter: %lf\n", pupil_diameter_px);

		g_queue_push_tail (recorder->data_queue, data);
	}
}

/* Parses the JSON data to extract the desired data.
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
#endif

static char *
recorder_start (Recorder *recorder)
{
	const char *request_pupil_remote;
	char *reply_pupil_remote;
	char *reply;

	g_print ("Send request to start recording to the Pupil Remote plugin...\n");
	recorder->record = TRUE;

	request_pupil_remote = "R";
	zmq_send (recorder->pupil_remote,
		  request_pupil_remote,
		  strlen (request_pupil_remote),
		  0);

	reply_pupil_remote = receive_next_message (recorder->pupil_remote);
	if (reply_pupil_remote == NULL)
	{
		g_error ("Timeout. Impossible to communicate with the Pupil Remote plugin.");
	}

	g_print ("Pupil Remote reply: %s\n", reply_pupil_remote);
	g_free (reply_pupil_remote);

	if (recorder->timer == NULL)
	{
		recorder->timer = g_timer_new ();
	}
	else
	{
		g_timer_start (recorder->timer);
	}

	reply = g_strdup ("ack");
	return reply;
}

static char *
recorder_stop (Recorder *recorder)
{
	const char *request_pupil_remote;
	char *reply_pupil_remote;
	char *reply;

	g_print ("Send request to stop recording to the Pupil Remote plugin...\n");

	if (recorder->timer != NULL)
	{
		g_timer_stop (recorder->timer);
		reply = g_strdup_printf ("%lf", g_timer_elapsed (recorder->timer, NULL));
	}
	else
	{
		reply = g_strdup ("no timer");
	}

	request_pupil_remote = "r";
	zmq_send (recorder->pupil_remote,
		  request_pupil_remote,
		  strlen (request_pupil_remote),
		  0);

	reply_pupil_remote = receive_next_message (recorder->pupil_remote);
	if (reply_pupil_remote == NULL)
	{
		g_error ("Timeout. Impossible to communicate with the Pupil Remote plugin.");
	}

	g_print ("Pupil Remote reply: %s\n", reply_pupil_remote);
	g_free (reply_pupil_remote);

	recorder->record = FALSE;

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
					"gaze_confidence:%lf\n"
					"gaze_norm_pos_x:%lf\n"
					"gaze_norm_pos_y:%lf\n"
					"pupil_confidence:%lf\n"
					"pupil_diameter_px:%lf\n",
					data->timestamp,
					data->gaze_confidence,
					data->gaze_norm_pos_x,
					data->gaze_norm_pos_y,
					data->pupil_confidence,
					data->pupil_diameter_px);
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

	g_print ("Request from cosy-pupil-client: %s\n", request);

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

		g_queue_free_full (recorder->data_queue, g_free);
		recorder->data_queue = g_queue_new ();
	}
	else
	{
		g_warning ("Unknown request: %s", request);
		reply = g_strdup ("unknown request");
	}

	g_print ("Send reply to cosy-pupil-client...\n");
	zmq_send (recorder->replier,
		  reply,
		  strlen (reply),
		  0);
	g_print ("done.\n");

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
#if 0
		read_all_pupil_messages (&recorder);
#endif
		read_request (&recorder);
	}

	recorder_finalize (&recorder);

	return EXIT_SUCCESS;
}
