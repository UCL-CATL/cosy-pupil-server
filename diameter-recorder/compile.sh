#!/usr/bin/env bash

gcc -W -Wall -o diameter-recorder -O2 $(pkg-config --cflags --libs glib-2.0 libczmq) diameter-recorder.c
