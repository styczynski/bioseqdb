all: docker-build-dev
	echo "DONE"

build:
	bash ./scripts/utils.sh build

docker-login:
	bash ./scripts/utils.sh docker-login

docker-build-dev:
	bash ./scripts/utils.sh docker-build bioseqdev

docker-build-base:
	bash ./scripts/utils.sh docker-build-base

docker-push-base:
	bash ./scripts/utils.sh docker-push-base

docker-run-dev:
	bash ./scripts/utils.sh docker-run-dev

.PHONY: build docker-login docker-build docker-build-dev docker-run-dev docker-build-base docker-push-base