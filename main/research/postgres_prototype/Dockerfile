FROM postgres:alpine

COPY . /usr/src/bioseqdb
RUN apk add --no-cache autoconf automake cmake g++ git make postgresql-dev zlib-dev \
    && git clone --depth 1 --single-branch --recursive "https://github.com/samtools/htslib.git" /usr/src/htslib \
    && git clone --depth 1 --single-branch --branch Apache2 "https://github.com/lh3/bwa.git" /usr/src/bwa \
    && cd /usr/src/htslib && autoreconf -i && ./configure --disable-bz2 --disable-lzma --host x86_64-pc-linux-musl && make CFLAGS="-g -O2 -fPIC" lib-static \
    && cd /usr/src/bwa && sed -i 's/ malloc_wrap.o/ malloc_wrap.o\\/' Makefile && sed -i 's/AOBJS=//' Makefile && sed -i '/rle_auxtab/d' rle.h && make CFLAGS="-g -O2 -fPIC" libbwa.a \
    && cd /usr/src/bioseqdb && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_LIBRARY_PATH="/usr/src/bwa;/usr/src/htslib" -DCMAKE_CXX_FLAGS="-I /usr/src" && make install \
    && (echo "CREATE EXTENSION bioseqdb;" > /docker-entrypoint-initdb.d/bioseqdb.sql) \
    && rm -r /usr/src/htslib /usr/src/bwa /usr/src/bioseqdb/build \
    && apk del autoconf automake cmake g++ git make postgresql-dev zlib-dev
