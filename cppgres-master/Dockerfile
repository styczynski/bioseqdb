FROM ubuntu:latest
RUN apt-get update
RUN apt-get install -y build-essential libssl-dev libreadline-dev libz-dev bison flex cmake clang-19 clang++-19
RUN useradd -m cppgres
USER cppgres
VOLUME /src
WORKDIR /src
