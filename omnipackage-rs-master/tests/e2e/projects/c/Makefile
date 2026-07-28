CC ?= cc
INSTALL_ROOT ?=

all: helloworld-c

helloworld-c: main.c version.h
	$(CC) -o helloworld-c main.c

install: helloworld-c
	install -d -m755 $(INSTALL_ROOT)/usr/bin/
	install -m755 helloworld-c $(INSTALL_ROOT)/usr/bin/

clean:
	rm -f helloworld-c
