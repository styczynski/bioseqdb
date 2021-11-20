#!/bin/bash

echo $GIT_TOKEN | docker login ghcr.io -u $GIT_LOGIN --password-stdin
