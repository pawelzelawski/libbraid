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
# Compatible with GNU make (Linux) and BSD make (OpenBSD/FreeBSD/NetBSD).
# Uses != for shell assignment — supported by both.
# Compiles each target in one shot — no pattern rules or substitution
# references required (those are GNU make-only when directories are involved).
# See TECH_STACK.md §5 for full build system specification.

# --- Platform detection — != is portable to GNU make >= 3.82 and BSD make ---

OS != uname -s

# --- Compiler ----------------------------------------------------------------

# Clang primary (required), GCC secondary
# Override on the command line: make CC=gcc
CC = clang

# --- Compiler flags ----------------------------------------------------------

CFLAGS_COMMON = -std=c11 -Wall -Wextra -Wpedantic		\
                -Wno-unused-parameter				\
                -D_POSIX_C_SOURCE=200809L			\
                -D_XOPEN_SOURCE=700

# ASan/UBSan — Linux only; OpenBSD clang does not support -fsanitize=address
SANITIZERS != if [ "$(OS)" = "Linux" ]; then echo "-fsanitize=address,undefined"; else echo ""; fi

CFLAGS_DEV    = $(CFLAGS_COMMON)				\
                -O0 -g3 -gdwarf-4 -Werror			\
                $(SANITIZERS)					\
                -fno-omit-frame-pointer				\
                -DBRAID_DEBUG -DBRAID_TEST_CLOCK

CFLAGS_TEST   = $(CFLAGS_DEV)

CFLAGS_TSAN   = $(CFLAGS_COMMON)				\
                -O1 -g3						\
                -fsanitize=thread				\
                -fno-omit-frame-pointer				\
                -DBRAID_DEBUG -DBRAID_TEST_CLOCK

CFLAGS_VG     = $(CFLAGS_COMMON) -O0 -g3 -gdwarf-4 -DBRAID_DEBUG -DBRAID_TEST_CLOCK

CFLAGS_RELEASE = $(CFLAGS_COMMON) -O2

# --- Install paths -----------------------------------------------------------

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

# --- Source files ------------------------------------------------------------

LIB_SRCS = src/braid_table.c		\
	   src/braid_conn.c		\
	   src/braid_reconnect.c	\
	   src/braid_reaper.c		\
	   src/braid_waitq.c		\
	   src/braid_pool.c

# Platform-specific I/O translation unit
PLATFORM_SRCS != if [ "$(OS)" = "Linux" ]; then echo "src/braid_io_epoll.c"; else echo "src/braid_io_kqueue.c"; fi

ALL_LIB_SRCS = $(LIB_SRCS) $(PLATFORM_SRCS)

TEST_SRCS = tests/run_tests.c		\
	    tests/test_table.c		\
	    tests/test_state_machine.c	\
	    tests/test_wait_queue.c	\
	    tests/test_reconnect.c	\
	    tests/test_reaper.c		\
	    tests/test_pool.c

# --- Build paths -------------------------------------------------------------

BUILD_DIR       = build
BUILD_TESTS_DIR = $(BUILD_DIR)/tests
TEST_BIN        = $(BUILD_TESTS_DIR)/run_tests
TEST_BIN_VG     = $(BUILD_TESTS_DIR)/run_tests_vg
LIB_RELEASE     = $(BUILD_DIR)/libbraid.a
REL_DIR         = $(BUILD_DIR)/rel

INCLUDES = -I include/

# --- Phony targets -----------------------------------------------------------

.PHONY: all dev release test test-tsan valgrind lint format clean install

all: dev

# Development build — compile and run tests
dev: $(TEST_BIN)
	@$(TEST_BIN)

# Test target — same as dev
test: $(TEST_BIN)
	@$(TEST_BIN)

# Release — optimised static library
release: $(LIB_RELEASE)

# TSan — Clang only; Linux only (OpenBSD clang has no TSan runtime)
test-tsan:
	@command -v clang >/dev/null 2>&1 || \
	    { echo "TSan target requires Clang"; exit 1; }
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_TSAN) $(INCLUDES) \
	    -o $(BUILD_TESTS_DIR)/run_tests_tsan \
	    $(ALL_LIB_SRCS) $(TEST_SRCS)
	@$(BUILD_TESTS_DIR)/run_tests_tsan

# Valgrind — Linux only; no sanitizers (ASan + Valgrind conflict)
valgrind: $(TEST_BIN_VG)
	@valgrind --leak-check=full		\
	          --show-leak-kinds=all		\
	          --track-origins=yes		\
	          --error-exitcode=1		\
	          $(TEST_BIN_VG); rc=$$?; rm -f vgcore.*; exit $$rc

# clang-tidy + cppcheck
lint:
	clang-tidy $(ALL_LIB_SRCS) -- $(CFLAGS_DEV) $(INCLUDES)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --error-exitcode=1 \
		         --suppress=missingIncludeSystem \
		         src/; \
	else \
		echo "cppcheck not found; skipping cppcheck step"; \
	fi

format:
	clang-format -i $(ALL_LIB_SRCS) src/*.h include/braid.h tests/*.c

install: $(LIB_RELEASE)
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 $(LIB_RELEASE) $(LIBDIR)/libbraid.a
	install -m 644 include/braid.h $(INCLUDEDIR)/braid.h

clean:
	rm -rf $(BUILD_DIR)

# --- Test binary (compile+link in one shot) ----------------------------------

$(TEST_BIN): $(ALL_LIB_SRCS) $(TEST_SRCS)
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_TEST) $(INCLUDES) \
	    -o $(TEST_BIN) \
	    $(ALL_LIB_SRCS) $(TEST_SRCS)

# --- Valgrind binary (no sanitizers, one shot) -------------------------------

$(TEST_BIN_VG): $(ALL_LIB_SRCS) $(TEST_SRCS)
	@mkdir -p $(BUILD_TESTS_DIR)
	$(CC) $(CFLAGS_VG) $(INCLUDES) \
	    -o $(TEST_BIN_VG) \
	    $(ALL_LIB_SRCS) $(TEST_SRCS)

# --- Release static library (explicit per-file compile + ar) -----------------
# Explicit rules — no pattern rules needed, fully BSD make portable.

$(LIB_RELEASE): $(ALL_LIB_SRCS)
	@mkdir -p $(REL_DIR)
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_table.c     -o $(REL_DIR)/braid_table.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_conn.c      -o $(REL_DIR)/braid_conn.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_reconnect.c -o $(REL_DIR)/braid_reconnect.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_reaper.c    -o $(REL_DIR)/braid_reaper.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_waitq.c     -o $(REL_DIR)/braid_waitq.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c src/braid_pool.c      -o $(REL_DIR)/braid_pool.o
	$(CC) $(CFLAGS_RELEASE) $(INCLUDES) -c $(PLATFORM_SRCS)      -o $(REL_DIR)/braid_io.o
	ar rcs $(LIB_RELEASE)				\
	    $(REL_DIR)/braid_table.o			\
	    $(REL_DIR)/braid_conn.o			\
	    $(REL_DIR)/braid_reconnect.o		\
	    $(REL_DIR)/braid_reaper.o			\
	    $(REL_DIR)/braid_waitq.o			\
	    $(REL_DIR)/braid_pool.o			\
	    $(REL_DIR)/braid_io.o
