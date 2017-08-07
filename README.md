cosy-pupil-server
=================

Extend the [Pupil](https://pupil-labs.com/) eye tracking software with tools
useful for neurosciences research at the Cognition and System division (COSY) of
the [Institute of NeuroSciences](http://www.uclouvain.be/en-ions.html) at the
Université Catholique de Louvain, Belgium.

This repository contains the “server” part. It runs on the same computer as the
Pupil software. See the [cosy-pupil-client](https://github.com/UCL-CATL/cosy-pupil-client)
repository for the client part.

This program has been developed on GNU/Linux.

The recommended way to use cosy-pupil-server is with a Docker container. A
Dockerfile is provided, see the instructions below to know how to build and run
the container.

cosy-pupil-server is licensed under the GNU General Public License version 3 or later.

Dependencies
------------

You don't need to install those dependencies, it's installed with the Docker
container. The list is just for documentation purposes.

- [ZeroMQ](http://zeromq.org/)
- [msgpack](http://msgpack.org/)
- [GLib](https://wiki.gnome.org/Projects/GLib)

Build the container image
-------------------------

    # ./build.sh

Run the container
-----------------

    # ./run.sh

external-recorder
-----------------

The current version of external-recorder has been developed and tested with
Pupil Capture version 0.9.3, [docker-pupil](https://github.com/UCL-CATL/docker-pupil)
version 2.

It records some Pupil capture data, but externally to the Pupil software. The
Pupil software is not modified, and it is not a plugin. So normally it will be
easily re-usable for future Pupil versions.

The external-recorder acts as a mediator between Pupil Capture and
[cosy-pupil-client](https://github.com/UCL-CATL/cosy-pupil-client). The
cosy-pupil-client talks only to the external-recorder with the Request-Reply
ZeroMQ pattern. In our case cosy-pupil-client runs on another computer running
a real-time Matlab program.

Pupil Capture publishes a stream of information with ZeroMQ, with the
Publisher-Subscriber communication pattern. The external-recorder creates a
subscriber and reads the messages.

The external-recorder also sends requests to the Pupil Capture software, via
the Pupil Remote plugin, to start and stop the recording (Pupil Capture records
more data than external-recorder, so it's better to start the recording on
Pupil Capture too). This uses the Request-Reply ZeroMQ pattern.

The external-recorder listens to the following ZeroMQ requests coming from
cosy-pupil-client:

- `start`: start recording. The reply should be "ack".
- `stop`: stop recording. The reply should be the number of seconds elapsed
  since the `start` signal, as a floating point number (encoded as a string).
- `receive_data`: receive the recorded data (as a string) since the latest call
  to `receive_data`.

For the `stop` request, the reply is useful to know the latency:

1. Matlab starts a timer.
2. Matlab sends the `start` request.
3. The external-recorder receives the `start` request and starts its own timer
   and sends the reply.
4. Matlab sends the `stop` request.
5. The external-recorder receives the `stop` request, stops its timer and sends
   the reply containing its elapsed time.
6. Matlab receives the reply and stores it in `elapsed_time_pupil`.
7. Matlab stops its timer and stores its elapsed time in `elapsed_time_matlab`.
8. Then Matlab can measure the latency by comparing `elapsed_time_pupil` and
   `elapsed_time_matlab`.

It's important to do that, to be sure that we have recorded what we wanted,
i.e. during the experiment. If the latency is too high, we miss some data. Of
course this timer dance can be done a first time before the actual experiment,
so if the latency is too high we know it directly.
