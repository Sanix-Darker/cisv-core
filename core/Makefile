# CISV Core Library Makefile
CC ?= gcc
AR ?= ar
RANLIB ?= ranlib

# Platform and architecture detection
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
CC_VERSION := $(shell $(CC) --version 2>/dev/null | head -n 1)
CC_IS_CLANG := $(findstring clang,$(CC_VERSION))

# PORTABLE mode: Set PORTABLE=1 to build portable binaries without -march=native
# Example: make PORTABLE=1 shared
PORTABLE ?= 0

# SECURITY: Binary hardening flags
# -fstack-protector-strong: Protect against stack buffer overflows
# -D_FORTIFY_SOURCE=2: Detect buffer overflows in string/memory functions
# -fstack-check: Check for stack overflow at runtime
SECURITY_CFLAGS = -fstack-protector-strong -D_FORTIFY_SOURCE=2

# Base CFLAGS (architecture-independent)
BASE_CFLAGS = -O3 -pipe -fomit-frame-pointer -Wall -Wextra -std=c11 \
              -ffast-math -funroll-loops -fPIC $(SECURITY_CFLAGS)

# Architecture-specific SIMD flags
ifeq ($(PORTABLE),1)
    # Portable mode: Use baseline SIMD that works on all CPUs of each arch
    ifeq ($(UNAME_M),x86_64)
        ARCH_CFLAGS = -msse4.2
    else ifeq ($(UNAME_M),amd64)
        ARCH_CFLAGS = -msse4.2
    else ifeq ($(UNAME_M),arm64)
        ARCH_CFLAGS = -march=armv8-a
    else ifeq ($(UNAME_M),aarch64)
        ARCH_CFLAGS = -march=armv8-a
    else
        ARCH_CFLAGS =
    endif
else
    # Native mode: Optimize for the build machine's CPU
    ifeq ($(UNAME_M),x86_64)
        ARCH_CFLAGS = -march=native -mavx2 -mtune=native
    else ifeq ($(UNAME_M),amd64)
        ARCH_CFLAGS = -march=native -mavx2 -mtune=native
    else ifeq ($(UNAME_M),arm64)
        ifeq ($(UNAME_S),Darwin)
            # Apple Clang rejects -mcpu=native on macOS arm64. The target triple
            # already selects Apple Silicon, so avoid adding an unsupported flag.
            ARCH_CFLAGS =
        else
            ARCH_CFLAGS = -mcpu=native
        endif
    else ifeq ($(UNAME_M),aarch64)
        ARCH_CFLAGS = -march=native
    else
        ARCH_CFLAGS = -march=native
    endif
endif

CFLAGS ?= $(BASE_CFLAGS) $(ARCH_CFLAGS)
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -fsanitize=address -fsanitize=undefined \
               -fno-omit-frame-pointer -fPIC

VALGRIND ?= valgrind
VALGRIND_FLAGS ?= --leak-check=full --show-leak-kinds=all --track-origins=yes --errors-for-leak-kinds=definite,possible --error-exitcode=1

# Platform-specific linker flags
ifeq ($(UNAME_S),Linux)
    # Linux: Full RELRO for GOT protection
    SECURITY_LDFLAGS = -Wl,-z,relro,-z,now
    CFLAGS += -D_GNU_SOURCE -fstack-check
else ifeq ($(UNAME_S),Darwin)
    # macOS: No RELRO equivalent, but use other hardening
    SECURITY_LDFLAGS =
    CFLAGS += -D__APPLE__
else
    SECURITY_LDFLAGS =
endif

LTO_CFLAGS =
LTO_LDFLAGS =

ifneq ($(CC_IS_CLANG),)
    # Clang + plain ar is fragile with LTO archives across platforms. Keep the
    # default build linkable unless the toolchain is wired for LLVM binutils.
    LTO_CFLAGS =
    LTO_LDFLAGS =
else
    LTO_CFLAGS = -flto
    LTO_LDFLAGS = -flto
endif

CFLAGS += $(LTO_CFLAGS)
LDFLAGS ?= $(LTO_LDFLAGS) $(SECURITY_LDFLAGS)

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BUILD_DIR = build

# Source files
SRCS = $(SRC_DIR)/parser.c $(SRC_DIR)/writer.c $(SRC_DIR)/transformer.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS_DEBUG = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.debug.o,$(SRCS))

# Library outputs
STATIC_LIB = $(BUILD_DIR)/libcisv.a
ifeq ($(UNAME_S),Darwin)
    SHARED_LIB = $(BUILD_DIR)/libcisv.dylib
else
    SHARED_LIB = $(BUILD_DIR)/libcisv.so
endif

# Targets
.PHONY: all static shared clean test test-native test-debug valgrind safety install

all: static shared

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Static library
static: $(STATIC_LIB)

$(STATIC_LIB): $(OBJS) | $(BUILD_DIR)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Shared library
shared: $(SHARED_LIB)

$(SHARED_LIB): $(OBJS) | $(BUILD_DIR)
ifeq ($(UNAME_S),Darwin)
	$(CC) -dynamiclib -o $@ $^ $(LDFLAGS)
else
	$(CC) -shared -o $@ $^ $(LDFLAGS)
endif

# Object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(INC_DIR) -c -o $@ $<

$(BUILD_DIR)/%.debug.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_DEBUG) -I$(INC_DIR) -c -o $@ $<

# Tests
TEST_SRCS = $(TEST_DIR)/test_core.c
TEST_BIN = $(BUILD_DIR)/test_core

test: $(TEST_BIN)
	@echo "Running core tests..."
	@./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) $(STATIC_LIB)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $(TEST_SRCS) $(STATIC_LIB) -lm -lpthread

# Native tests (tests/test_native.c)
NATIVE_TEST_SRC = ../tests/test_native.c
NATIVE_TEST_BIN = $(BUILD_DIR)/test_native

test-native: $(NATIVE_TEST_BIN)
	@echo "Running native tests..."
	@./$(NATIVE_TEST_BIN)

$(NATIVE_TEST_BIN): $(NATIVE_TEST_SRC) $(STATIC_LIB)
	$(CC) $(CFLAGS) -I$(INC_DIR) -o $@ $(NATIVE_TEST_SRC) $(STATIC_LIB) -lm -lpthread

test-debug: $(OBJS_DEBUG)
	$(CC) $(CFLAGS_DEBUG) -I$(INC_DIR) -o $(BUILD_DIR)/test_core_debug \
		$(TEST_SRCS) $(OBJS_DEBUG) -lm -lpthread
	@echo "Running debug tests..."
	@./$(BUILD_DIR)/test_core_debug

# Installation
PREFIX ?= /usr/local

install: static shared
	install -d $(PREFIX)/lib
	install -d $(PREFIX)/include/cisv
	install -m 644 $(STATIC_LIB) $(PREFIX)/lib/
	install -m 755 $(SHARED_LIB) $(PREFIX)/lib/
	install -m 644 $(INC_DIR)/cisv/*.h $(PREFIX)/include/cisv/
ifeq ($(UNAME_S),Linux)
	ldconfig || true
endif

uninstall:
	rm -f $(PREFIX)/lib/libcisv.a
	rm -f $(PREFIX)/lib/libcisv.so
	rm -f $(PREFIX)/lib/libcisv.dylib
	rm -rf $(PREFIX)/include/cisv

clean:
	rm -rf $(BUILD_DIR)

valgrind: $(TEST_BIN)
	@echo "Running core tests under valgrind..."
	@$(VALGRIND) $(VALGRIND_FLAGS) ./$(TEST_BIN)

safety: test-debug valgrind
	@echo "Safety checks completed"
