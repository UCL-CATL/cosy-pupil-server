FROM fedora:23
MAINTAINER Sébastien Wilmet

RUN dnf -y update && \
	dnf -y group install "C Development Tools and Libraries" && \
	dnf -y install \
		zeromq-devel \
		czmq-devel \
		glib2-devel \
		json-glib-devel \
		git && \
	dnf clean all

ADD . /root/cosy-pupil-server

# Make sure that the code is compilable
RUN cd /root/cosy-pupil-server && \
	cd external-recorder && make && cd .. && \
	cd tests && make

WORKDIR /root/cosy-pupil-server/external-recorder

# Set default command
CMD ["/usr/bin/bash"]
