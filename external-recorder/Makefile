CC = gcc
CFLAGS = -Wall `pkg-config --cflags libczmq msgpack glib-2.0`
LDFLAGS = `pkg-config --libs libczmq msgpack glib-2.0`
EXECUTABLE = external-recorder

.PHONY: clean

all: $(EXECUTABLE)

external-recorder: external-recorder.c

clean:
	rm -f $(EXECUTABLE)
