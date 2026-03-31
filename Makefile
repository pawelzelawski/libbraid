# Makefile — libbraid
#
# Primary targets:
#   make          — equivalent to make dev
#   make dev      — debug build with ASan/UBSan, runs tests
#   make release  — optimised build without sanitizers
#   make test     — run test suite (dev build)
#   make test-tsan— run test suite under TSan (Clang only)
#   make valgrind — run test suite under Valgrind (Linux only)
#   make lint     — clang-tidy + cppcheck
#   make format   — clang-format -i on all source files
#   make clean    — remove build artefacts
#   make install  — install libbraid.a and include/braid.h
#
# See TECH_STACK.md §5 for full build system specification.

# Platform detection
OS := $(shell uname -s)

# Compiler — Clang primary (required), GCC secondary
# Override on the command line for GCC validation: make CC=gcc
CC = clang

# Compiler flags
CFLAGS_COMMON = -std=c11 -Wall -Wextra -Wpedantic			\
                -Wno-unused-parameter					\
                -D_POSIX_C_SOURCE=200809L				\
                -D_XOPEN_SOURCE=700

CFLAGS_DEV    = $(CFLAGS_COMMON)					\
                -O0 -g3 -gdwarf-4 -Werror				\
                -fsanitize=address,undefined				\
                -fno-omit-frame-pointer				\
                -DBRAID_DEBUG

CFLAGS_TEST   = $(CFLAGS_DEV) -DBRAID_TEST_CLOCK

CFLAGS_TSAN   = $(CFLAGS_COMMON)					\
                -O1 -g3						\
                -fsanitize=thread					\
                -fno-omit-frame-pointer				\
                -DBRAID_DEBUG -DBRAID_TEST_CLOCK

CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2

# Install paths
PREFIX    ?= /usr/local
LIBDIR    ?= $(PREFIX)/lib
INCLUDEDIR?= $(PREFIX)/include

# Source files — common to all platforms
LIB_SRCS =	src/braid_table.c		\
		src/braid_conn.c		\
		src/braid_reconnect.c		\
		src/braid_reaper.c		\
		src/braid_waitq.c		\
		src/braid_pool.c

# Platform-specific I/O translation unit
ifeq ($(OS),Linux)
    PLATFORM_SRCS = src/braid_io_epoll.c
endif
ifeq ($(OS),OpenBSD)
    PLATFORM_SRCS = src/braid_io_kqueue.c
endif
ifeq ($(OS),FreeBSD)
    PLATFORM_SRCS = src/braid_io_kqueue.c
endif
ifeq ($(OS),NetBSD)
    PLATFORM_SRCS = src/braid_io_kqueue.c
endif

ALL_LIB_SRCS = $(LIB_SRCS) $(PLATFORM_SRCS)

# Test source files
TEST_SRCS = tests/run_tests.c tests/test_table.c

# Build output directories
BUILD_DIR       = build
BUILD_TESTS_DIR = $(BUILD_DIR)/tests

# Build artefacts
LIB_DEV     = $(BUILD_DIR)/libbraid_dev.a
LIB_RELEASE = $(BUILD_DIR)/libbraid.a
TEST_BIN    = $(BUILD_TESTS_DIR)/run_tests

# Object files (dev build)
LIB_OBJS_DEV  = $(patsubst src/%.c,$(BUILD_DIR)/dev/%.o,$(ALL_LIB_SRCS))
TEST_OBJS_DEV = $(patsubst tests/%.c,$(BUILD_DIR)/dev/tests/%.o,$(TEST_SRCS))

# Object files (valgrind build — no sanitizers, DWARF 4 for Valgrind compat)
CFLAGS_VG     = $(CFLAGS_COMMON) -O0 -g3 -gdwarf-4 -DBRAID_DEBUG -DBRAID_TEST_CLOCK
LIB_OBJS_VG   = $(patsubst src/%.c,$(BUILD_DIR)/vg/%.o,$(ALL_LIB_SRCS))
TEST_OBJS_VG  = $(patsubst tests/%.c,$(BUILD_DIR)/vg/tests/%.o,$(TEST_SRCS))
TEST_BIN_VG   = $(BUILD_TESTS_DIR)/run_tests_vg

# Object files (release build)
LIB_OBJS_REL  = $(patsubst src/%.c,$(BUILD_DIR)/rel/%.o,$(ALL_LIB_SRCS))

# Include path
INCLUDES = -I include/

.PHONY: all dev release test test-tsan valgrind lint format clean install

# Default target
all: dev

# Development build — compile library stubs; compile and run tests
dev: $(TEST_BIN)
	@$(TEST_BIN)

# Release build — optimised static library only
release: $(LIB_RELEASE)

# Test target — same as dev (compiles and runs)
test: $(TEST_BIN)
	@$(TEST_BIN)

# TSan build — Clang only
test-tsan: _check_clang $(BUILD_TESTS_DIR)/run_tests_tsan
	@$(BUILD_TESTS_DIR)/run_tests_tsan

# Valgrind — Linux only; no sanitizers (ASan + Valgrind conflict)
valgrind: $(TEST_BIN_VG)
	valgrind --leak-check=full		\
	         --show-leak-kinds=all		\
	         --track-origins=yes		\
	         --error-exitcode=1		\
	         $(TEST_BIN_VG)

# clang-tidy + cppcheck
lint:
	clang-tidy $(ALL_LIB_SRCS) -- $(CFLAGS_DEV) $(INCLUDES)
	cppcheck --enable=all --error-exitcode=1	\
	         --suppress=missingIncludeSystem	\
	         src/

# clang-format
format:
	clang-format -i $(ALL_LIB_SRCS) src/*.h include/braid.h tests/*.c

# Install
install: $(LIB_RELEASE)
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 $(LIB_RELEASE) $(LIBDIR)/libbraid.a
	install -m 644 include/braid.h $(INCLUDEDIR)/braid.h

# Clean
clean:
	rm -rf $(BUILD_DIR)

# --- Dev build rules ---

$(BUILD_DIR)/dev/tests/%.o: tests/%.c | $(BUILD_DIR)/dev/tests
	$(CC) $(CFLAGS_TEST) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/dev/%.o: src/%.c | $(BUILD_DIR)/dev
	$(CC) $(CFLAGS_DEV) $(INCLUDES) -c $< -o $@

$(TEST_BIN): $(LIB_OBJS_DEV) $(TEST_OBJS_DEV) | $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_TEST) -o $@ $^

# --- Release build rules ---

$(BUILD_DIR)/rel/%.o: src/%.c | $(BUILD_DIR)/rel
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c $< -o $@

$(LIB_RELEASE): $(LIB_OBJS_REL) | $(BUILD_DIR)
	ar rcs $@ $^

# --- Valgrind build rules (no sanitizers) ---

$(BUILD_DIR)/vg/tests/%.o: tests/%.c | $(BUILD_DIR)/vg/tests
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/vg/%.o: src/%.c | $(BUILD_DIR)/vg
	$(CC) $(CFLAGS_VG) $(INCLUDES) -c $< -o $@

$(TEST_BIN_VG): $(LIB_OBJS_VG) $(TEST_OBJS_VG) | $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_VG) -o $@ $^

# --- TSan build rules ---

$(BUILD_DIR)/tsan/tests/%.o: tests/%.c | $(BUILD_DIR)/tsan/tests
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/tsan/%.o: src/%.c | $(BUILD_DIR)/tsan
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) -c $< -o $@

$(BUILD_TESTS_DIR)/run_tests_tsan: \
    $(patsubst src/%.c,$(BUILD_DIR)/tsan/%.o,$(ALL_LIB_SRCS)) \
    $(patsubst tests/%.c,$(BUILD_DIR)/tsan/tests/%.o,$(TEST_SRCS)) \
    | $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_TSAN) -o $@ $^

# --- Directory creation ---

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/dev: | $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR)/dev/tests: | $(BUILD_DIR)/dev
	mkdir -p $@

$(BUILD_DIR)/vg: | $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR)/vg/tests: | $(BUILD_DIR)/vg
	mkdir -p $@

$(BUILD_DIR)/rel: | $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR)/tsan: | $(BUILD_DIR)
	mkdir -p $@

$(BUILD_DIR)/tsan/tests: | $(BUILD_DIR)/tsan
	mkdir -p $@

$(BUILD_TESTS_DIR): | $(BUILD_DIR)
	mkdir -p $@

_check_clang:
	@command -v clang >/dev/null 2>&1 || \
	    { echo "TSan target requires Clang"; exit 1; }
