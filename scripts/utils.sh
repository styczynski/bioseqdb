#!/bin/bash

COMMAND="$1"

if [[ "$COMMAND" == "docker-build" ]]; then
    bash -c "source ./scripts/pg_docker_build.sh --build-arg RUN_SMOKE_TEST=true -t $2"
elif [[ "$COMMAND" == "build" ]]; then
    bash -c "source ./scripts/pg_local_build.sh"
elif [[ "$COMMAND" == "docker-run-dev" ]]; then
    bash -c "source ./scripts/pg_docker_run_dev.sh"
else
    printf "Invalid command was specified: %s\n" "${COMMAND}"
    exit 1
fi

