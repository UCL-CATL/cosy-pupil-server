#!/bin/sh

sudo touch /etc/resolv.conf
sudo /home/user/repos/docker-pupil/run.sh /home/user/pupil/recordings
sudo chown -R user:user /home/user/pupil
