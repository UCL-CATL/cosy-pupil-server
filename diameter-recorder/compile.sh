#!/usr/bin/env bash

gcc -W -Wall -o diameter-recorder -O2 $(pkg-config --cflags --libs libczmq) diameter-recorder.c
