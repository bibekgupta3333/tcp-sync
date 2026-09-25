# Build:      make                  -> build/tcpsync-server, build/tcpsync-client, build/unit_tests
# Test:       make test             -> unit tests + end-to-end integration test
# Sanitizers: make asan | make tsan -> separate build-asan/ or build-tsan/ tree, then the tests
# Valgrind:   make valgrind         (Linux only)
# Container:  make docker-check     -> Linux: test + asan + tsan + valgrind (use this on macOS,
#                                      where Valgrind is unsupported)

CXX      ?= c++
SAN      ?=
BUILD    ?= build$(if $(SAN),-$(SAN))

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS += -Iinclude -MMD -MP
LDLIBS   += -pthread

ifeq ($(SAN),asan)
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
else ifeq ($(SAN),tsan)
  SANFLAGS := -fsanitize=thread
else ifneq ($(SAN),)
  $(error SAN must be empty, asan or tsan)
endif
CXXFLAGS += $(SANFLAGS)
LDFLAGS  += $(SANFLAGS)

LIB_SRCS := src/net.cpp src/protocol.cpp src/file_store.cpp src/server.cpp src/client.cpp
LIB_OBJS := $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
BINS     := $(BUILD)/tcpsync-server $(BUILD)/tcpsync-client $(BUILD)/unit_tests
ALL_OBJS := $(LIB_OBJS) $(BUILD)/src/server_main.o $(BUILD)/src/client_main.o $(BUILD)/tests/unit_tests.o

.PHONY: all test unit integration asan tsan valgrind helgrind docker-image docker-check clean

all: $(BINS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tcpsync-server: $(BUILD)/src/server_main.o $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/tcpsync-client: $(BUILD)/src/client_main.o $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/unit_tests: $(BUILD)/tests/unit_tests.o $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

unit: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

integration: $(BUILD)/tcpsync-server $(BUILD)/tcpsync-client
	tests/integration_test.sh $(BUILD)

# Sequential on purpose: under `make -j` separate prerequisites would run in parallel.
test: $(BINS)
	$(BUILD)/unit_tests
	tests/integration_test.sh $(BUILD)

asan:
	$(MAKE) SAN=asan test

tsan:
	$(MAKE) SAN=tsan test

valgrind: all
	valgrind --leak-check=full --show-leak-kinds=definite,indirect --error-exitcode=1 $(BUILD)/unit_tests

# Helgrind reports some false positives for std::shared_mutex/condition_variable
# internals, so it doesn't fail the build; read the report.
helgrind: all
	-valgrind --tool=helgrind $(BUILD)/unit_tests

docker-image:
	docker build -t tcpsync-dev .

# TSan needs ASLR restrictions relaxed inside some containers, hence the personality flag.
docker-check: docker-image
	docker run --rm --security-opt seccomp=unconfined tcpsync-dev \
		sh -c 'setarch $$(uname -m) -R sh -c "make -j4 all && make test && make asan && make tsan && make valgrind"'

clean:
	rm -rf build build-asan build-tsan

-include $(ALL_OBJS:.o=.d)
