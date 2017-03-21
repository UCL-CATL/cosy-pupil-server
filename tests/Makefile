CC = gcc
CFLAGS = -Wall `pkg-config --cflags libczmq glib-2.0`
LDFLAGS = `pkg-config --libs libczmq glib-2.0`
EXECUTABLE = test-request

.PHONY: clean

all: $(EXECUTABLE)

test-request: test-request.c

clean:
	rm -f $(EXECUTABLE)
