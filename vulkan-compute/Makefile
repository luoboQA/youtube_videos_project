CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic
DEBUG_FLAGS = -DDEBUG -O0 -g
RELEASE_FLAGS = -DNDEBUG -O2
LDFLAGS = -lvulkan

config ?= debug

ifeq ($(config), debug)
	CFLAGS += $(DEBUG_FLAGS)
else
	CFLAGS += $(RELEASE_FLAGS)
endif

TARGET = vulkan-compute
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run