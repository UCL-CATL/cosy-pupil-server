#!/usr/bin/env bash

gcc -W -o diameter-recorder -O2 $(pkg-config --cflags --libs libczmq glib-2.0 json-glib-1.0) diameter-recorder.c
