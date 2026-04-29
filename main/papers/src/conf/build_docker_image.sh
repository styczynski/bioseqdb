#!/bin/bash

export VERSION="1.0.0"

echo ${GH_PROFILE_TOKEN} | docker login ghcr.io -u styczynski --password-stdin

docker buildx build --platform linux/amd64 -t ghcr.io/styczynski/go-cv-generator:${VERSION} -f Dockerfile . && \
docker push ghcr.io/styczynski/go-cv-generator:${VERSION}