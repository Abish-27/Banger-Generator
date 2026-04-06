CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

all:
	$(CC) $(CFLAGS) -o midi_gen midi_gen.c

clean:
	rm -f midi_gen output.mid

.PHONY: all clean