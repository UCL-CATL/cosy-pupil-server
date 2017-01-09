#!/bin/sh

# Run this script as root.
# sudo ./run-pupil.sh

/home/user/repos/docker-pupil/run.sh /home/user/pupil/recordings
chown -R user:user /home/user/pupil
