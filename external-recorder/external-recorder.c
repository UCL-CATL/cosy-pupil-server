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

#define DEBUG TRUE

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

	guint record : 1;
};

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
		filter = "";
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
	recorder->record = FALSE;

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

#if 0
static Data *
data_new (void)
{
	return g_new0 (Data, 1);
}
#endif

/* Free the return value with g_free(). */
static char *
receive_json_data (void *subscriber)
{
	zmq_msg_t msg;
	int n_bytes;
	int ok;
	void *raw_data;
	msgpack_unpacker *unpacker;
	msgpack_unpacked unpacked;
	msgpack_unpack_return unpack_ret;
	msgpack_object obj;
	char *unpacked_str = NULL;

	ok = zmq_msg_init (&msg);
	g_return_val_if_fail (ok == 0, NULL);

	n_bytes = zmq_msg_recv (&msg, subscriber, 0);
	if (n_bytes <= 0)
	{
		goto out;
	}

	raw_data = zmq_msg_data (&msg);

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
			g_warning ("Memory allocation error with msgpack.");
			goto out;
		}
	}

	memcpy (msgpack_unpacker_buffer (unpacker), raw_data, n_bytes);
	msgpack_unpacker_buffer_consumed (unpacker, n_bytes);

	msgpack_unpacked_init (&unpacked);
	unpack_ret = msgpack_unpacker_next (unpacker, &unpacked);
	if (unpack_ret != MSGPACK_UNPACK_SUCCESS)
	{
		g_warning ("Unpacking failed.");
		msgpack_unpacked_destroy (&unpacked);
		goto out;
	}

	obj = unpacked.data;

	if (DEBUG)
	{
		msgpack_object_print (stdout, obj);
		g_print ("\n");
	}

	if (obj.type == MSGPACK_OBJECT_STR)
	{
		unpacked_str = g_strndup (obj.via.str.ptr, obj.via.str.size);
	}
	else if (obj.type == MSGPACK_OBJECT_MAP)
	{
		/* TODO */
		unpacked_str = g_strdup ("map");
	}
	else
	{
		g_warning ("The unpacked data has another type. type=%d", obj.type);
	}

	msgpack_unpacked_destroy (&unpacked);

out:
	if (unpacker != NULL)
	{
		msgpack_unpacker_free (unpacker);
	}

	ok = zmq_msg_close (&msg);
	if (ok != 0)
	{
		g_free (unpacked_str);
		g_return_val_if_reached (NULL);
	}

	return unpacked_str;
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
		g_warning ("A Pupil message must be in two parts. Only one part received.");
		return FALSE;
	}

	*json_data = receive_json_data (recorder->subscriber);

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

end:
		g_free (topic);
		g_free (json_data);
	}
}

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
