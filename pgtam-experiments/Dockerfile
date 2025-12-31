FROM debian

COPY . /wrk

RUN apt-get update -y && apt-get install -y git build-essential bison flex
RUN git clone https://github.com/postgres/postgres && cd postgres && ./configure --without-icu --without-readline --without-zlib && make -j8 && make install
RUN cd /wrk && make && make install
