all: docker-build-dev
	echo "DONE"

build:
	bash ./scripts/utils.sh build

docker-build-dev:
	bash ./scripts/utils.sh docker-build bioseqdev

docker-run-dev:
	bash ./scripts/utils.sh docker-run-dev

.PHONY: build docker-build docker-run-dev