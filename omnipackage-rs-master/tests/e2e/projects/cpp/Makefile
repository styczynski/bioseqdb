CXX ?= c++
INSTALL_ROOT ?=

all: helloworld-cpp

helloworld-cpp: main.cpp version.h
	$(CXX) -o helloworld-cpp main.cpp

install: helloworld-cpp
	install -d -m755 $(INSTALL_ROOT)/usr/bin/
	install -m755 helloworld-cpp $(INSTALL_ROOT)/usr/bin/

clean:
	rm -f helloworld-cpp
