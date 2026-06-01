CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17
LDFLAGS = -lpthread -lm
SRCS    = main.c matchmaking.c monitor.c
HEADERS = common.h
TARGET  = cmatch

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
