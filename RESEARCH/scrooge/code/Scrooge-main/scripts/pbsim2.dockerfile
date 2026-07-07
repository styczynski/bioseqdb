FROM ubuntu:20.04
RUN apt-get update
RUN apt-get install git -y
RUN apt-get install make -y
RUN apt-get install g++ -y
RUN apt-get install python3 -y
RUN git clone https://github.com/yukiteruono/pbsim2.git && cd pbsim2 && ./configure && make && make install
CMD bash
