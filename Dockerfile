# https://github.com/UCL-CATL/cosy-docker-layer
FROM ucl-cosy/cosy-docker-layer:24

MAINTAINER Sébastien Wilmet

RUN dnf -y install \
		zeromq-devel \
		czmq-devel \
		glib2-devel \
		json-glib-devel && \
	dnf clean all

ADD . /root/cosy-pupil-server

# Make sure that the code is compilable
RUN cd /root/cosy-pupil-server && \
	cd external-recorder && make clean && make && cd .. && \
	cd tests && make clean && make

WORKDIR /root/cosy-pupil-server/external-recorder

# Set default command
CMD ["/usr/bin/bash"]
