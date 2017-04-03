/*
 * This file is part of cosy-pupil-server.
 *
 * Copyright (C) 2016, 2017 - Université Catholique de Louvain
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>
#include <msgpack.h>

#define PUPIL_REMOTE_ADDRESS "tcp://localhost:50020"
#define REPLIER_ENDPOINT "tcp://*:6000"

#define DEBUG FALSE

typedef struct _Data Data;
struct _Data
{
	double timestamp;
	double pupil_diameter;
	double gaze_norm_pos_x;
	double gaze_norm_pos_y;
	double confidence;
};

typedef struct _Recorder Recorder;
struct _Recorder
{
	/* The zeromq context. */
	void *context;

	/* The requester to the Pupil Remote plugin. */
	void *pupil_remote;

	/* The subscriber to listen to the data coming from Pupil Capture. */
	void *subscriber;

	/* The replier, to listen and reply to some requests coming from another
	 * program than the Pupil (in our case, a Matlab script running on
	 * another computer).
	 */
	void *replier;

	/* Contains a list of recorded Data*. */
	GQueue *data_queue;

	GTimer *timer;

	guint recording : 1;
};

typedef enum
{
	TOPIC_PUPIL,
	TOPIC_GAZE,
	TOPIC_OTHER
} Topic;

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

static void
init_pupil_remote (Recorder *recorder)
{
	int timeout_ms;
	int ok;

	g_assert (recorder->pupil_remote == NULL);

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
		g_error ("Error when setting ZeroMQ socket option for the Pupil Remote: %s",
			 g_strerror (errno));
	}
}

static void
init_subscriber (Recorder *recorder)
{
	const char *request;
	char *sub_port;
	char *address;
	const char *filter;
	int timeout_ms;
	int ok;

	g_assert (recorder->pupil_remote != NULL);
	g_assert (recorder->subscriber == NULL);

	/* Do the same as in:
	 * https://github.com/pupil-labs/pupil-helpers/blob/master/pupil_remote/filter_messages.py
	 *
	 * Plus tune some ZeroMQ options.
	 */

	/* Ask to Pupil Remote the subscriber port. */
	request = "SUB_PORT";
	zmq_send (recorder->pupil_remote,
		  request,
		  strlen (request),
		  0);

	sub_port = receive_next_message (recorder->pupil_remote);
	if (sub_port == NULL)
	{
		g_error ("Timeout. Impossible to communicate with the Pupil Remote plugin.");
	}

	address = g_strdup_printf ("tcp://localhost:%s", sub_port);

	recorder->subscriber = zmq_socket (recorder->context, ZMQ_SUB);
	ok = zmq_connect (recorder->subscriber, address);
	if (ok != 0)
	{
		g_error ("Error when connecting to the ZeroMQ subscriber: %s",
			 g_strerror (errno));
	}

	if (DEBUG)
	{
		/* Receive all messages. */
		/*filter = "";*/

		filter = "pupil.";
	}
	else
	{
		filter = "pupil.";
	}

	ok = zmq_setsockopt (recorder->subscriber,
			     ZMQ_SUBSCRIBE,
			     filter,
			     strlen (filter));
	if (ok != 0)
	{
		g_error ("Error when setting ZeroMQ socket option for the subscriber: %s",
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
		g_error ("Error when setting ZeroMQ socket option for the subscriber: %s",
			 g_strerror (errno));
	}

	g_free (sub_port);
	g_free (address);
}

static void
init_replier (Recorder *recorder)
{
	int timeout_ms;
	int ok;

	g_assert (recorder->replier == NULL);

	recorder->replier = zmq_socket (recorder->context, ZMQ_REP);
	ok = zmq_bind (recorder->replier, REPLIER_ENDPOINT);
	if (ok != 0)
	{
		g_error ("Error when creating ZeroMQ socket at \"" REPLIER_ENDPOINT "\": %s.\n"
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
		g_error ("Error when setting ZeroMQ socket option for the replier: %s",
			 g_strerror (errno));
	}
}

static void
recorder_init (Recorder *recorder)
{
	g_assert (recorder->context == NULL);
	recorder->context = zmq_ctx_new ();

	init_pupil_remote (recorder);
	init_subscriber (recorder);
	init_replier (recorder);

	recorder->data_queue = g_queue_new ();
	recorder->timer = NULL;
	recorder->recording = FALSE;

	g_print ("Initialized successfully.\n\n");
}

static void
recorder_finalize (Recorder *recorder)
{
	zmq_close (recorder->subscriber);
	recorder->subscriber = NULL;

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

static Data *
data_new (void)
{
	Data *data;

	data = g_new (Data, 1);

	data->timestamp = -1.0;
	data->pupil_diameter = -1.0;
	data->gaze_norm_pos_x = -1.0;
	data->gaze_norm_pos_y = -1.0;
	data->confidence = -1.0;

	return data;
}

/* Returns whether something has been extracted. */
static gboolean
extract_info_from_msgpack_key_value (Data              *data,
				     msgpack_object_kv *key_value)
{
	msgpack_object *key;
	msgpack_object *value;
	msgpack_object_str *key_str;

	key = &key_value->key;
	value = &key_value->val;

	if (key->type != MSGPACK_OBJECT_STR)
	{
		g_warning ("msgpack: expected a string for the key in a key_value pair, "
			   "got type=%d instead.",
			   key->type);
		return FALSE;
	}

	key_str = &key->via.str;
	if (key_str->ptr == NULL)
	{
		return FALSE;
	}

	if (strncmp (key_str->ptr, "timestamp", key_str->size) == 0)
	{
		if (value->type != MSGPACK_OBJECT_FLOAT)
		{
			g_warning ("msgpack: expected a float for the timestamp value, "
				   "got type=%d instead.",
				   value->type);
			return FALSE;
		}

		data->timestamp = value->via.f64;
		return TRUE;
	}
	else if (strncmp (key_str->ptr, "diameter", key_str->size) == 0)
	{
		if (value->type != MSGPACK_OBJECT_FLOAT)
		{
			g_warning ("msgpack: expected a float for the diameter value, "
				   "got type=%d instead.",
				   value->type);
			return FALSE;
		}

		data->pupil_diameter = value->via.f64;
		return TRUE;
	}
	else if (strncmp (key_str->ptr, "confidence", key_str->size) == 0)
	{
		if (value->type != MSGPACK_OBJECT_FLOAT)
		{
			g_warning ("msgpack: expected a float for the confidence value, "
				   "got type=%d instead.",
				   value->type);
			return FALSE;
		}

		data->confidence = value->via.f64;
		return TRUE;
	}
	else if (strncmp (key_str->ptr, "norm_pos", key_str->size) == 0)
	{
		msgpack_object_array *array;
		msgpack_object *first_element;
		msgpack_object *second_element;

		if (value->type != MSGPACK_OBJECT_ARRAY)
		{
			g_warning ("msgpack: expected an array for the norm_pos value, "
				   "got type=%d instead.",
				   value->type);
			return FALSE;
		}

		array = &value->via.array;

		if (array->size != 2)
		{
			g_warning ("msgpack: expected 2 elements in the norm_pos array, "
				   "got %d elements instead.",
				   array->size);
			return FALSE;
		}

		first_element = &array->ptr[0];
		second_element = &array->ptr[1];

		if (first_element->type != MSGPACK_OBJECT_FLOAT ||
		    second_element->type != MSGPACK_OBJECT_FLOAT)
		{
			g_warning ("msgpack: expected float elements in the norm_pos array, "
				   "got types %d and %d instead.",
				   first_element->type,
				   second_element->type);
			return FALSE;
		}

		data->gaze_norm_pos_x = first_element->via.f64;
		data->gaze_norm_pos_y = second_element->via.f64;

		return TRUE;
	}

	return FALSE;
}

static void
extract_info_from_msgpack_root_object (Recorder       *recorder,
				       msgpack_object *obj)
{
	msgpack_object_map *map;
	Data *data;
	uint32_t kv_num;
	gboolean something_extracted = FALSE;

	if (obj->type != MSGPACK_OBJECT_MAP)
	{
		g_warning ("msgpack: expected a map for the root object, got type=%d instead.",
			   obj->type);
		return;
	}

	map = &obj->via.map;

	data = data_new ();

	for (kv_num = 0; kv_num < map->size; kv_num++)
	{
		msgpack_object_kv *key_value;

		key_value = &map->ptr[kv_num];

		if (extract_info_from_msgpack_key_value (data, key_value))
		{
			something_extracted = TRUE;
		}
	}

	if (something_extracted)
	{
		g_print ("%stimestamp=%lf, diameter=%lf, confidence=%lf, x=%lf, y=%lf\n",
			 recorder->recording ? "[Recording] " : "",
			 data->timestamp,
			 data->pupil_diameter,
			 data->confidence,
			 data->gaze_norm_pos_x,
			 data->gaze_norm_pos_y);

		if (recorder->recording)
		{
			g_queue_push_tail (recorder->data_queue, data);
			data = NULL;
		}
	}

	g_free (data);
}

static void
read_msgpack_data (Recorder *recorder)
{
	zmq_msg_t zeromq_msg;
	int n_bytes;
	int ok;
	void *raw_data;
	msgpack_unpacker *unpacker;
	msgpack_unpacked unpacked;
	gboolean unpacked_is_init = FALSE;
	msgpack_unpack_return unpack_ret;
	msgpack_object obj;

	ok = zmq_msg_init (&zeromq_msg);
	g_return_if_fail (ok == 0);

	n_bytes = zmq_msg_recv (&zeromq_msg, recorder->subscriber, 0);
	if (n_bytes <= 0)
	{
		goto out;
	}

	raw_data = zmq_msg_data (&zeromq_msg);

	unpacker = msgpack_unpacker_new (1024);
	if (unpacker == NULL)
	{
		goto out;
	}

	if (msgpack_unpacker_buffer_capacity (unpacker) < n_bytes)
	{
		bool result;

		result = msgpack_unpacker_reserve_buffer (unpacker, n_bytes);
		if (!result)
		{
			g_warning ("msgpack: memory allocation error.");
			goto out;
		}
	}

	memcpy (msgpack_unpacker_buffer (unpacker), raw_data, n_bytes);
	msgpack_unpacker_buffer_consumed (unpacker, n_bytes);

	msgpack_unpacked_init (&unpacked);
	unpacked_is_init = TRUE;

	unpack_ret = msgpack_unpacker_next (unpacker, &unpacked);
	if (unpack_ret != MSGPACK_UNPACK_SUCCESS)
	{
		g_warning ("msgpack: unpacking failed. The Pupil message "
			   "received was apparently not packed with msgpack.");
		goto out;
	}

	obj = unpacked.data;

	if (DEBUG)
	{
		g_print ("msgpack data: ");
		msgpack_object_print (stdout, obj);
		g_print ("\n");
	}

	extract_info_from_msgpack_root_object (recorder, &obj);

out:
	if (unpacker != NULL)
	{
		msgpack_unpacker_free (unpacker);
	}

	if (unpacked_is_init)
	{
		msgpack_unpacked_destroy (&unpacked);
	}

	ok = zmq_msg_close (&zeromq_msg);
	g_return_if_fail (ok == 0);
}

static Topic
determine_topic (const char *topic_str)
{
	if (topic_str == NULL)
	{
		return TOPIC_OTHER;
	}

	if (g_str_has_prefix (topic_str, "pupil"))
	{
		return TOPIC_PUPIL;
	}

	if (g_str_has_prefix (topic_str, "gaze"))
	{
		return TOPIC_GAZE;
	}

	return TOPIC_OTHER;
}

/* Reads a Pupil message from the subscriber.
 * It must be a multi-part message, with exactly two parts: the topic and the
 * msgpack data.
 * Returns: TRUE if a message has been read, FALSE if there were no messages.
 */
static gboolean
read_pupil_message (Recorder *recorder)
{
	char *topic_str;
	Topic topic;
	int64_t more;
	size_t more_size = sizeof (more);
	int ok;

	topic_str = receive_next_message (recorder->subscriber);
	if (topic_str == NULL)
	{
		/* Timeout, no messages. */
		return FALSE;
	}

	if (DEBUG)
	{
		g_print ("Topic: %s\n", topic_str);
	}

	topic = determine_topic (topic_str);

	if (topic != TOPIC_PUPIL && !DEBUG)
	{
		g_warning ("I'm not supposed to receive other topics than with the 'pupil' prefix. "
			   "Topic received: '%s'",
			   topic_str);
	}

	g_free (topic_str);
	topic_str = NULL;

	/* Determine if more message parts are to follow. */
	ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (!more)
	{
		g_warning ("A Pupil message must be in two parts. Only one part received.");
		return TRUE;
	}

	if (topic == TOPIC_PUPIL)
	{
		read_msgpack_data (recorder);
	}
	else
	{
		char *msg;

		msg = receive_next_message (recorder->subscriber);
		g_free (msg);
	}

	/* Determine if more message parts are to follow.
	 * There must be exactly two parts. If there are more, it's an error.
	 */
	ok = zmq_getsockopt (recorder->subscriber, ZMQ_RCVMORE, &more, &more_size);
	g_return_val_if_fail (ok == 0, FALSE);
	if (more)
	{
		g_warning ("A Pupil message must be in two parts. More than two parts received.");

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
	}

	return TRUE;
}

static void
read_all_pupil_messages (Recorder *recorder)
{
	while (read_pupil_message (recorder))
		;
}

static char *
recorder_start (Recorder *recorder)
{
	const char *request_pupil_remote;
	char *reply_pupil_remote;
	char *reply;

	if (recorder->recording)
	{
		g_warning ("Already recording.");
		reply = g_strdup ("already recording");
		return reply;
	}

	g_print ("Send request to start recording to the Pupil Remote plugin...\n");
	recorder->recording = TRUE;

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

	if (!recorder->recording)
	{
		g_warning ("Already stopped.");
		reply = g_strdup ("already stopped");
		return reply;
	}

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

	recorder->recording = FALSE;

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
					"pupil_diameter:%lf\n"
					"gaze_norm_pos_x:%lf\n"
					"gaze_norm_pos_y:%lf\n"
					"confidence:%lf\n",
					data->timestamp,
					data->pupil_diameter,
					data->gaze_norm_pos_x,
					data->gaze_norm_pos_y,
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
		 * long as there is enough RAM on both sides). So 1MB should be
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
	g_print ("done.\n\n");

	g_free (request);
	g_free (reply);
}

int
main (void)
{
	Recorder recorder = { 0 };

	recorder_init (&recorder);

	while (TRUE)
	{
		read_all_pupil_messages (&recorder);
		read_request (&recorder);
	}

	recorder_finalize (&recorder);

	return EXIT_SUCCESS;
}
