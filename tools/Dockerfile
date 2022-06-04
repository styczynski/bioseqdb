FROM postgres:14-bullseye

LABEL maintainer="bioseqdb - https://github.com/covid-genomics/bioseqdb"

ENV LIB_FLAGS "-g -Wall -O2 -fPIC"

ENV BIOSEQDB_TAG master
ENV BWA_TAG master
ENV HTS_TAG 1.5

RUN set -ex \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      autotools-dev \
	  build-essential \
      ca-certificates \
      cmake \
      g++ \
      git \
      make \
      postgresql-server-dev-14 \
	  libbz2-dev \
	  zlib1g-dev \
	  liblzma-dev \
    && cd /usr/src/ \
    && git clone --recursive 'https://github.com/samtools/htslib.git' --depth 1 --branch "$HTS_TAG" \
    && git clone 'https://github.com/covid-genomics/bioseqdb' --depth 1 --branch "$BIOSEQDB_TAG" \
    && git clone 'https://github.com/lh3/bwa.git' --depth 1 --branch "$BWA_TAG" \
	&& cd /usr/src/bwa && make CFLAGS="$LIB_FLAGS" CXXFLAGS="$LIB_FLAGS" libbwa.a \
	&& cd /usr/src/htslib && autoreconf -i && ./configure && make CFLAGS="$LIB_FLAGS" CXXFLAGS="$LIB_FLAGS" lib-static \
	&& cd /usr/src/bioseqdb && mkdir build && cd build \
	&& cd /usr/src/bioseqdb/build \
	&& cmake .. -DPostgreSQL_TYPE_INCLUDE_DIR=/usr/include/postgresql/14/server \
				-DCMAKE_LIBRARY_PATH='/usr/src/bwa/;/usr/src/htslib/' \
				-DCMAKE_CXX_FLAGS='-I /usr/src/' \
	&& make install \
	&& cd / \
	&& rm -r /usr/src/* \
    && apt-get purge -y --autoremove \
      autoconf \
      automake \
      autotools-dev \
	  build-essential \
      ca-certificates \
      cmake \
      g++ \
      git \
      make \
      postgresql-server-dev-14 \
	  libbz2-dev \
	  zlib1g-dev \
	  liblzma-dev \
	&& apt-get clean \
	&& rm -rf /var/lib/lists/*
