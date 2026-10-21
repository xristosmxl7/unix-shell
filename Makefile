CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic

TARGET = osh
SRC = src/osh.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
