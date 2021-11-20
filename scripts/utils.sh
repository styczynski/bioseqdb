#!/bin/bash

COMMAND="$1"

if [[ "$COMMAND" == "docker-build" ]]; then
    bash -c "source ./scripts/pg_docker_build.sh --build-arg RUN_SMOKE_TEST=true -t $2"
elif [[ "$COMMAND" == "docker-push-base" ]]; then
    docker push ghcr.io/covid-genomics/bioseq-postgres-dev:latest
elif [[ "$COMMAND" == "docker-build-base" ]]; then
    bash -c "source ./scripts/pg_docker_build.sh -f base.Dockerfile -t ghcr.io/covid-genomics/bioseq-postgres-dev:latest"
elif [[ "$COMMAND" == "build" ]]; then
    bash -c "source ./scripts/pg_local_build.sh"
elif [[ "$COMMAND" == "docker-run-dev" ]]; then
    bash -c "source ./scripts/pg_docker_run_dev.sh"
elif [[ "$COMMAND" == "docker-login" ]]; then
    bash -c "source ./scripts/ghcr_login.sh"
else
    printf "Invalid command was specified: %s\n" "${COMMAND}"
    exit 1
fi

