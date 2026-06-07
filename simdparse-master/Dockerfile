FROM ubuntu:latest

ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && \
    apt-get -y install gcc g++ cmake && \
    rm -rf /var/lib/apt/lists/*

COPY include/ include/
RUN find include/simdparse -type f -name '*.hpp' -exec sh -c 'echo "#include \"${1##*/}\"\nint main(int argc, const char* argv[]) { return 0; }" > ${1%.*}.cpp' _ {} \;
RUN find include/simdparse -type f -name '*.cpp' -print0 | xargs -0L1 g++ -Wall -Werror
RUN find include/simdparse -type f -name '*.cpp' -print0 | xargs -0L1 g++ -mavx2 -Wall -Werror
