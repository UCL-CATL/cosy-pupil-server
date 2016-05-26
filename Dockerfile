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

WORKDIR /root/cosy-pupil-server

RUN cd external-recorder && make && cd - && \
	cd tests && make && cd -

# Set default command
CMD ["/root/cosy-pupil-server/external-recorder/external-recorder"]
