# Fuzz the render path. Needs clang with libFuzzer, and SAM_DATA_DIR pointing at a voice
# pack. From the repository root:
#
#   SAM_DATA_DIR=third_party/ms-sam-mike-mary-decomp/data/voice make -C tools run
#
# The engine is compiled with fuzzer-no-link so it carries the coverage counters; only the
# driver links the fuzzer main.
SAM ?= $(abspath ../third_party/ms-sam-mike-mary-decomp)
# make defines CC itself, so `CC ?=` would never win against that built-in default
CC := $(if $(filter default,$(origin CC)),clang,$(CC))
SRC := sam.c sam_lex.c sam_morph.c sam_pos.c sam_norm.c sam_front.c sam4fx.c sam_tts.c
OBJ := $(addprefix build/,$(SRC:.c=.o))

all: build/fuzz_tts

build:
	mkdir -p build

build/%.o: $(SAM)/src/%.c | build
	$(CC) -std=c99 -g -O1 -fsanitize=fuzzer-no-link,address,undefined -I$(SAM)/src -c $< -o $@

build/fuzz_tts: fuzz_tts.c $(OBJ) | build
	$(CC) -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -I$(SAM)/src $< $(OBJ) -lm -o $@

# one seed per corpus line, so a crash shrinks against a sentence that is known to render
build/seeds: ../tools/corpus.txt | build
	rm -rf build/seeds && mkdir -p build/seeds
	n=0; tr -d '\r' < ../tools/corpus.txt | while IFS= read -r line; do \
	  n=$$((n + 1)); printf '%s' "$$line" > build/seeds/$$(printf '%02d' $$n).txt; done

run: build/fuzz_tts build/seeds
	SAM_DATA_DIR=$${SAM_DATA_DIR:?set SAM_DATA_DIR to the voice pack} \
	  ./build/fuzz_tts -max_total_time=$${MAX_TOTAL_TIME:-60} -max_len=4096 build/seeds

clean:
	rm -rf build

.PHONY: all run clean
