ARG PYTHON_VERSION="3.13.3"
ARG CMAKE_VERSION="4.0"
ARG CMAKE_VERSION_BUILD="1"
ARG DEBIAN_VERSION="bookworm"
ARG CTAN_REPO="https://ctan.math.illinois.edu/systems/texlive/tlnet"

FROM "debian:${DEBIAN_VERSION}" as installer
RUN apt-get update && \
    apt-get --no-install-recommends install -y \
        perl-modules \
        liburi-encode-perl \
        gnupg \
        build-essential \
        libssl-dev \
        gcc \
        g++ \
        curl \
        ca-certificates \
        wget && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /install

FROM installer as texlive_installer
ARG PYTHON_VERSION
ARG CMAKE_VERSION
ARG CMAKE_VERSION_BUILD
ARG DEBIAN_VERSION
ARG CTAN_REPO
ARG DEBIAN_FRONTEND=noninteractive 

#COPY texlive.profile .

RUN wget -qO- http://mirror.ctan.org/systems/texlive/tlnet/install-tl-unx.tar.gz | tar -xzf- --strip-components=1 && \
    ./install-tl
    
RUN /usr/local/texlive

#-profile texlive.profile

# CMAKE INSTALL

FROM installer as cmake_installer
ARG PYTHON_VERSION
ARG CMAKE_VERSION
ARG CMAKE_VERSION_BUILD
ARG DEBIAN_VERSION
ARG CTAN_REPO

RUN curl -OL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}.${CMAKE_VERSION_BUILD}/cmake-${CMAKE_VERSION}.${CMAKE_VERSION_BUILD}.tar.gz
RUN mkdir -p /usr/local/cmake

RUN tar -xzvf cmake-${CMAKE_VERSION}.${CMAKE_VERSION_BUILD}.tar.gz && \
    cd cmake-${CMAKE_VERSION}.${CMAKE_VERSION_BUILD} && \
    ./bootstrap --prefix=/usr/local/cmake && \
    make && \
    make install

# ACTUAL IMAGE
FROM "python:${PYTHON_VERSION}-slim-${DEBIAN_VERSION}" as base
ARG PYTHON_VERSION
ARG CMAKE_VERSION
ARG CMAKE_VERSION_BUILD
ARG DEBIAN_VERSION
ARG CTAN_REPO
ARG DEBIAN_FRONTEND=noninteractive

WORKDIR /var/local

# combine into one run command to reduce image size
RUN apt-get update && \
    apt-get install -y perl wget libfontconfig1 rsync && \
    wget -qO- "https://yihui.name/gh/tinytex/tools/install-unx.sh" | sh  && \
    apt-get clean
ENV PATH="${PATH}:/root/bin"

RUN tlmgr repository add ${CTAN_REPO} && \
    tlmgr repository list && \
    #tlmgr repository remove http://mirror.ctan.org/systems/texlive/tlnet && \
    tlmgr option repository ${CTAN_REPO}

RUN tlmgr install luatex
RUN tlmgr install biber
RUN tlmgr path add
RUN tlmgr update --self --all
RUN fmtutil-sys --all

RUN \
  # test the installation; we only test the full installation because
  # in that, all tools are present and have to work
  lualatex --version && printf '\n' && \
  biber --version && printf '\n'

FROM base as baseninja

RUN apt-get install ninja-build
RUN \
   ninja --version

FROM baseninja as baseninjanode

ARG NODE_VERSION=18.10.0
ARG NVM_VERSION=0.39.0

RUN apt install -y curl
RUN curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v${NVM_VERSION}/install.sh | bash
ENV NVM_DIR=/root/.nvm
RUN . "$NVM_DIR/nvm.sh" && nvm install ${NODE_VERSION}
RUN . "$NVM_DIR/nvm.sh" && nvm use v${NODE_VERSION}
RUN . "$NVM_DIR/nvm.sh" && nvm alias default v${NODE_VERSION}
ENV PATH="/root/.nvm/versions/node/v${NODE_VERSION}/bin/:${PATH}"
RUN node --version && npm --version

FROM baseninjanode
ARG PYTHON_VERSION
ARG CMAKE_VERSION
ARG CMAKE_VERSION_BUILD
ARG DEBIAN_VERSION
ARG CTAN_REPO

#COPY --from=texlive_installer /usr/local/texlive /usr/local/texlive
COPY --from=cmake_installer /usr/bin/make /usr/local/bin/
COPY --from=cmake_installer /usr/local/cmake/* /usr/local/bin/
RUN mv /usr/local/bin/cmake-${CMAKE_VERSION} /usr/local/share/
#COPY --from=cmake_installer /usr/share/cmake-${CMAKE_VERSION} /usr/share/cmake-${CMAKE_VERSION}
RUN cmake --version

#RUN tlmgr install beamer pgf babel-english biblatex

# && ls /usr/local/bin/ && printf "\n\nHEHE\n" && ls /usr/share/cmake-${CMAKE_VERSION} && exit 11
#ENV CMAKE_ROOT="/usr/share/cmake-${CMAKE_VERSION}"
#RUN ls /usr/local && cmake --version && exit 55

# RUN apt-get update && \
#     apt-get --no-install-recommends install -y \
#         perl-modules \
#         liburi-encode-perl \
#         gnupg \
#         wget && \
#     rm -rf /var/lib/apt/lists/*

# ENV HOME=/tmp PATH="/usr/local/texlive/bin/x86_64-linux:$PATH"

# RUN tlmgr install \
#         collection-latex \
#         collection-luatex \
#         fontspec && \
#     rm -rf /usr/local/texlive/texmf-var/*

WORKDIR /data

COPY . .
RUN pip3 install -r requirements.txt
RUN python3 ./src/conf/tlmgr_install.py "./latex.lock" "./src/**/*.tex" "./src/**/*.cls" 
RUN cmake -G Ninja .
#&& ninja

LABEL org.opencontainers.image.source="https://github.com/styczynski/go-cv-generator"
LABEL org.opencontainers.image.description="Latex builder"

VOLUME ["/data"]