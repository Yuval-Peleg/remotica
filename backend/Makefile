CC ?= gcc
CFLAGS := -std=c99 -Wall -Wextra -O2 -DNO_SSL -DUSE_WEBSOCKET
LDFLAGS := -lpthread

THIRD_PARTY := third_party
CIVETWEB_DIR := $(THIRD_PARTY)/civetweb
CJSON_DIR := $(THIRD_PARTY)/cjson

INCLUDES := -I$(CIVETWEB_DIR)/include -I$(CIVETWEB_DIR)/src -I$(CJSON_DIR)

# Every .c file under src/ (our own code) plus the two vendored libraries.
# Using a wildcard here instead of listing each file means adding a new
# src/*.c file is picked up automatically next build, no Makefile edit
# needed.
SRCS := $(wildcard src/*.c) $(CIVETWEB_DIR)/src/civetweb.c $(CJSON_DIR)/cJSON.c
BUILD_DIR := build
BIN := $(BUILD_DIR)/remotica-backend

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(BIN) $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD_DIR)
