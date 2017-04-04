#!/usr/bin/env bash

config_file="/etc/sysconfig/docker-storage-setup"

if [ ! -f "${config_file}.orig" ]; then
	cp ${config_file} ${config_file}.orig
fi

echo "STORAGE_DRIVER=overlay" > ${config_file}

docker-storage-setup
