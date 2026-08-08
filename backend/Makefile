CC ?= gcc
CFLAGS := -std=c99 -Wall -Wextra -O2 -DNO_SSL -DUSE_WEBSOCKET
LDFLAGS := -lpthread

# Baked in at build time so GET /api/system can report which build is
# running. `git describe` fails in a release tarball with no .git (and in
# a shallow CI clone with no tags), so this falls back to "dev" rather
# than breaking the build — see .github/workflows/release.yml, which
# checks out with fetch-depth: 0 specifically so real releases get a real
# version here instead of silently shipping as "dev".
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CFLAGS += -DREMOTICA_VERSION=\"$(VERSION)\"

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
