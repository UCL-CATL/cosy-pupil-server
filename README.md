cosy-pupil-server
=================

Extend the [Pupil](https://pupil-labs.com/) eye tracking software with tools
useful for neurosciences research at the Cognition and System division (COSY) of
the [Institute of NeuroSciences](http://www.uclouvain.be/en-ions.html) at the
Université Catholique de Louvain, Belgium.

This repository contains the “server” part. It runs on the same computer as the
Pupil software. See the [cosy-pupil-client](https://github.com/UCL-CATL/cosy-pupil-client)
repository for the client part.

external-recorder
-----------------

It records some Pupil capture data, but externally to the Pupil software. The
Pupil software is not modified, and it is not a plugin. So normally it will be
easily re-usable for future versions of the Pupil.

The Pupil Server plugin needs to be enabled. It publishes a stream of
information with [ZeroMQ](http://zeromq.org/), with the Publisher-Subscriber
communication pattern. The external recorder creates a subscriber and reads
the messages.

Additionally, the external recorder uses the Request-Reply ZeroMQ pattern. It
listens for the following requests:

- `start`: start recording. The reply should be "ack".
- `stop`: stop recording. The reply should be the number of seconds elapsed
  since the `start` signal, as a floating point number (encoded as a string).
- `receive_data`: receive the recorded data (as a string) since the latest call
  to `receive_data`.

The requests are sent by another process. In our case on another computer
running a real-time Matlab program (see
[cosy-pupil-client](https://github.com/UCL-CATL/cosy-pupil-client)).

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
