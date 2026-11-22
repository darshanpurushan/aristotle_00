CXX ?= g++
ARROW_DIR ?= $(CURDIR)/.deps/arrow
ARCH_FLAGS ?= -march=x86-64-v3
CXXFLAGS ?= -O3 -DNDEBUG -flto
CPPFLAGS += -isystem $(ARROW_DIR)/include
ARROW_LIBS = -L$(ARROW_DIR) -Wl,-rpath,$(ARROW_DIR) -l:libparquet.so.2200 -l:libarrow.so.2200

.PHONY: all clean check-fixtures check-dev
all: shopping-sessions

$(ARROW_DIR)/libarrow.so.2200: scripts/fetch-arrow
	sh ./scripts/fetch-arrow "$(ARROW_DIR)"

shopping-sessions: main.cpp input.hpp output.hpp $(ARROW_DIR)/libarrow.so.2200
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH_FLAGS) -std=c++20 -pthread main.cpp $(LDFLAGS) $(ARROW_LIBS) -o $@

check-fixtures: shopping-sessions
	bash ./scripts/check-fixtures ./shopping-sessions "$(FIXTURES)"

check-dev: shopping-sessions
	@test -n "$(DEV_DATA)" && test -n "$(DEV_ANSWER)"
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
		DATA_DIR="$$(realpath "$(DEV_DATA)")" OUTPUT_DIR="$$tmp" ./shopping-sessions && \
		bash ./scripts/check "$$tmp" "$(DEV_ANSWER)"

clean:
	rm -f shopping-sessions
