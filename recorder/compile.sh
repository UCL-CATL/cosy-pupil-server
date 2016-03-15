#!/usr/bin/env bash

gcc -W -o recorder -O2 $(pkg-config --cflags --libs libczmq glib-2.0 json-glib-1.0) recorder.c
