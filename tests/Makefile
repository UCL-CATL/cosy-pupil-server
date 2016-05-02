CC = gcc
CFLAGS = -W `pkg-config --cflags libczmq`
LDFLAGS = `pkg-config --libs libczmq`
EXECUTABLE = test-request

.PHONY: clean

test-request: test-request.c zhelpers.h

all: $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE)
