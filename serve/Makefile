# sapi_serve: thin HTTP front end for SAPI 5 voices. Links against libsam from the
# vendored ms-sam-mike-mary-decomp checkout, plus libmicrohttpd (HTTP framing) and
# cJSON (request and response JSON).

SAM   ?= $(abspath ../third_party/ms-sam-mike-mary-decomp)
CC    ?= cc
PKG   ?= pkg-config
PKCFGS := $(shell $(PKG) --cflags libmicrohttpd libcjson)
PKLIBS := $(shell $(PKG) --libs libmicrohttpd libcjson)
# override += : an inherited CFLAGS must not drop the flags this build depends on
override CFLAGS += -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -Wframe-larger-than=32768 \
                   -DSAM_VERSION='"$(VERSION)"' -DSAM_ENGINE_COMMIT='"$(ENGINE_COMMIT)"'
override LDFLAGS += -L$(SAM)/build -lsam -lm -Wl,-rpath,$(SAM)/build

# stamped into the binary and printed by --version; pass VERSION=... when building from a
# tree without git
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
ENGINE_COMMIT ?= $(shell git -C $(SAM) rev-parse --short HEAD 2>/dev/null || echo unknown)

SRCS := sapi_serve.c render.c
OBJS := $(SRCS:.c=.o)

all: sapi_serve

$(OBJS): render.h

%.o: %.c
	$(CC) $(CFLAGS) $(PKCFGS) -I$(SAM)/src -c $< -o $@

sapi_serve: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(PKLIBS)

# build the engine too, so a fresh checkout needs one command
engine:
	$(MAKE) -C $(SAM)

run: sapi_serve
	SAM_DATA_DIR=$(SAM)/data/voice ./sapi_serve 8080

clean:
	rm -f sapi_serve $(OBJS)

# Style and static analysis. Config lives at the repository root (.clang-format,
# .clang-tidy); the vendored engine under third_party/ is not reformatted.
CSRC := $(wildcard *.c) $(wildcard *.h)

format:
	clang-format -i $(CSRC)

format-check:
	clang-format --dry-run -Werror $(CSRC)

tidy: sapi_serve.c
	# header-filter keeps diagnostics in this directory: the vendored engine has its own style
	clang-tidy --header-filter='$(CURDIR)/.*' $(CSRC) -- -std=c11 -D_DEFAULT_SOURCE -I$(SAM)/src $(PKCFGS)

.PHONY: all engine run clean format format-check tidy
