# SPDX-License-Identifier: MIT
# ==============================================================================
# tests/proto/Makefile — host build/run for byok_proto's C test suite
# ==============================================================================
# Builds test_proto.c directly against firmware/common/byok_proto's sources
# with plain clang (no ESP-IDF, no CMake) and runs it under
# AddressSanitizer + UndefinedBehaviorSanitizer.
#
#   make -C tests/proto test
#
# `test` also regenerates vectors.json (written by test_proto.c into this
# directory), which tests/host/test_proto.py loads to cross-check the Python
# reference implementation against this C implementation's actual output.

PROTO_DIR := ../../firmware/common/byok_proto

CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -O0 -I$(PROTO_DIR)
LDFLAGS := -fsanitize=address,undefined

SRCS := test_proto.c $(PROTO_DIR)/byok_proto.c $(PROTO_DIR)/byok_crc32.c
BIN  := test_proto

.PHONY: test build clean

test: build
	./$(BIN)

build: $(BIN)

$(BIN): $(SRCS) $(PROTO_DIR)/byok_proto.h $(PROTO_DIR)/byok_crc32.h
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN) vectors.json
	rm -rf $(BIN).dSYM
