#!/usr/bin/env bash

list=$(sudo docker ps -a -q)
num=$(sudo docker ps -a -q | wc -l)

if [ "$num" -eq "0" ]; then
	echo "OK, there were no containers to kill."
	exit 0
fi

sudo docker stop $list
sudo docker rm $list
echo "OK, $num container(s) have been killed."
