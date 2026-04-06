CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99

TARGET = midi_gen
SRC = midi_gen.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ) output.mid

.PHONY: all clean