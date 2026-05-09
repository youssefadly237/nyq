CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g \
					-D_GNU_SOURCE \
          $(shell pkg-config --cflags libpipewire-0.3 libcjson)
LDFLAGS = -lm \
          $(shell pkg-config --libs libpipewire-0.3 libcjson)

SRC = main.c util.c pw.c
OBJ = $(SRC:.c=.o)
BIN = nyq

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
