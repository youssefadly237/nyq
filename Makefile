CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g \
          -D_GNU_SOURCE \
          $(shell pkg-config --cflags libpipewire-0.3 libcjson)
LDFLAGS = -lm \
          $(shell pkg-config --libs libpipewire-0.3 libcjson) \
          -lsystemd

SRC = main.c util.c pw.c mpris.c sock.c
HDR = util.h pw.h mpris.h sock.h
OBJ = $(SRC:.c=.o)
BIN = nyq
FMT = clang-format

.PHONY: all clean lint format check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

lint:
	$(FMT) --dry-run --Werror $(SRC) $(HDR)

format:
	$(FMT) -i $(SRC) $(HDR)

check: lint all
