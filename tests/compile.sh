#!/usr/bin/env bash

gcc -W -o test-request -O2 $(pkg-config --cflags --libs libczmq) test-request.c
