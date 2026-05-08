CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -g \
          $(shell pkg-config --cflags libcjson)
LDFLAGS = -lm $(shell pkg-config --libs libcjson)

SRC = main.c util.c
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
