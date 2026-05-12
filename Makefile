# Makefile for mew - split-source build.
#
# make            default build (-O2)
# make tiny       smallest binary (-Os -s)
# make strict     -Werror with extra warnings
# make test       build and run the full test suite
# make clean      remove binary and object files

CC      ?= cc
CSTD     = -std=c99
WARN     = -Wall -Wextra -Wpedantic
CFLAGS  ?= -O2
LDFLAGS ?= -lm

STRICT_WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wclobbered -Werror

SRC = src/core.c src/parse.c src/eval.c src/builtins.c src/main.c
HDR = src/mew.h

ifeq ($(OS),Windows_NT)
    EXE = mew.exe
    RM_CMD = -del /Q mew.exe mew-tiny.exe 2>nul
    LDFLAGS =
else
    EXE = mew
    RM_CMD = rm -f mew mew-tiny
endif

all: $(EXE)

$(EXE): $(SRC) $(HDR)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

tiny: $(SRC) $(HDR)
	$(CC) $(CSTD) $(WARN) -Os -s -o $(EXE) $(SRC) $(LDFLAGS)

strict: $(SRC) $(HDR)
	$(CC) $(CSTD) $(STRICT_WARN) $(CFLAGS) -o $(EXE) $(SRC) $(LDFLAGS)

test: $(EXE)
	python tests/run.py

clean:
	$(RM_CMD)

.PHONY: all tiny strict test clean
