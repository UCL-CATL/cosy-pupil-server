cosy-pupil-server
=================

Extend the [Pupil](https://pupil-labs.com/) eye tracking software with tools
useful for neurosciences research at the Cognition and System division (COSY) of
the [Institute of NeuroSciences](http://www.uclouvain.be/en-ions.html) at the
Université Catholique de Louvain, Belgium.

This repository contains the “server” part. It runs on the same computer as the
Pupil software. See the cosy-pupil-client repository for the client part.

external-recorder
-----------------

It records some Pupil capture data, but externally to the Pupil software. The
Pupil software is not modified, and it is not a plugin. So normally it will be
easily re-usable for future versions of the Pupil.

The Pupil Server plugin needs to be enabled. It publishes a stream of
information with ZeroMQ, with the Publisher-Subscriber communication pattern.
The external recorder creates a subscriber and reads the messages.

Additionally, the external recorder uses the Request-Reply ZeroMQ pattern. It
listens for the following requests:
- `start`: start recording. The reply should be "ack".
- `stop`: stop recording. The reply should be the number of seconds elapsed
  since the `start` signal, as a floating point number (encoded as a string).
- `receive_data`: receive the recorded data (as a string) since the latest call
  to `receive_data`.

The requests are sent by another process. In our case on another computer
running a real-time Matlab program (see cosy-pupil-client).
