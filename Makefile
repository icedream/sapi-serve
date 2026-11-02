# Convenience targets for the whole tree. The real rules stay where they belong: the
# server in serve/Makefile, the fuzz harness in tools/Makefile, the engine in
# third_party/. This file only forwards, so CI and a local checkout run the same thing.

PORT ?= 18080
DOCKER_TAG ?= sapi-serve:dev

.PHONY: all engine server run test test-voice style tidy format fuzz docker clean

all: engine server

engine:
	$(MAKE) -C third_party/ms-sam-mike-mary-decomp

server: engine
	$(MAKE) -C serve

run: server
	$(MAKE) -C serve run

# needs no voice pack: an empty data dir still covers routing, framing, error codes and
# the token rules
test: server
	@mkdir -p out/no-voice
	@SAM_DATA_DIR=out/no-voice ./serve/sapi_serve $(PORT) & \
	  sleep 1; \
	  ./serve/test.sh http://localhost:$(PORT); status=$$?; \
	  kill $$! 2>/dev/null; \
	  exit $$status

# point SAM_DATA_DIR at a voice pack for the render checks
test-voice: server
	@test -n "$$SAM_DATA_DIR" || { echo "set SAM_DATA_DIR to a directory holding <Voice>.spd and <Voice>.sdf" >&2; exit 1; }
	@SAM_DATA_DIR="$$SAM_DATA_DIR" ./serve/sapi_serve $(PORT) & \
	  sleep 1; \
	  ./serve/test.sh --voice http://localhost:$(PORT); status=$$?; \
	  kill $$! 2>/dev/null; \
	  exit $$status

style:
	./tools/check_no_microsoft_files.sh
	./tools/check_ci_names.sh
	$(MAKE) -C serve format-check
	shellcheck serve/test.sh tools/*.sh

format:
	$(MAKE) -C serve format

tidy:
	$(MAKE) -C serve tidy

fuzz:
	$(MAKE) -C tools run

docker:
	docker build -t $(DOCKER_TAG) .

clean:
	$(MAKE) -C serve clean
	$(MAKE) -C tools clean
