CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
TARGET  ?= r8-dedup
SRCS    := src/main.c src/scanner.c src/hasher.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) src/dedup.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET) *.exe
