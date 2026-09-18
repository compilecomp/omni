# Omni — Top-level Makefile
#
# Builds all core libraries and unit tests with C++26.
# Uses g++ (Debian 14.2+) by default; override with CXX=clang++.
#
# Targets:
#   make           - build all unit tests
#   make check     - build and run all unit tests
#   make clean     - remove build artifacts
#
# Per Laws Rule 23 (no magic numbers in logic), all thresholds and
# limits are named constexpr in core/common/types.hpp, NOT in this
# Makefile. Compiler flags are configuration, not logic, so they
# remain here as variables.

CXX      ?= g++
CXXSTD   := -std=c++26
WARN     := -Wall -Wextra -Wpedantic -Werror
OPT      ?= -O2
DEBUG    ?= -g
CPPFLAGS := -I. -Icore -pthread

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(DEBUG)

BUILD_DIR := build

# --- Unit tests ---
TEST_NAMES := \
	tagged_value \
	instruction \
	speculative_arithmetic

TEST_BINS := $(patsubst %,$(BUILD_DIR)/test_%,$(TEST_NAMES))

# --- Core sources (compiled into each test as needed) ---
CORE_SRCS := \
	core/bytecode/bytecode_verifier.cpp \
	core/interpreter/adaptive_quickening.cpp \
	core/interpreter/fusion_engine.cpp \
	core/interpreter/handlers_semantic.cpp \
	core/interpreter/interpreter.cpp \
	core/interpreter/speculative_arithmetic.cpp

.PHONY: all check clean

all: $(TEST_BINS)

check: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		./$$t || exit 1; \
	done

$(BUILD_DIR)/test_%: tests/unit/test_%.cpp $(CORE_SRCS) $(CORE_SRCS:.cpp=.hpp)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(CORE_SRCS) -o $@

clean:
	rm -rf $(BUILD_DIR)
