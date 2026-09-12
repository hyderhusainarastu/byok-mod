# SPDX-License-Identifier: MIT
# ==============================================================================
# tests/tar/Makefile — host build/run for byok_tar's C test suite
# ==============================================================================
# Builds test_tar.c directly against firmware/common/byok_tar's sources with
# plain clang (no ESP-IDF, no CMake) and runs it under AddressSanitizer +
# UndefinedBehaviorSanitizer. Mirrors tests/proto/Makefile exactly.
#
#   make -C tests/tar test

TAR_DIR := ../../firmware/common/byok_tar

CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -O0 -I$(TAR_DIR)
LDFLAGS := -fsanitize=address,undefined

SRCS := test_tar.c $(TAR_DIR)/byok_tar.c
BIN  := test_tar

.PHONY: test build clean

test: build
	./$(BIN)

build: $(BIN)

$(BIN): $(SRCS) $(TAR_DIR)/byok_tar.h
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM
