CC = gcc
CFLAGS = -Wall `pkg-config --cflags libczmq glib-2.0 json-glib-1.0`
LDFLAGS = `pkg-config --libs libczmq glib-2.0 json-glib-1.0`
EXECUTABLE = external-recorder

.PHONY: clean

external-recorder: external-recorder.c

all: $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE)
