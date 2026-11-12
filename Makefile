# Build with:  make            (all programs)
#              make test       (unit + integration tests)
#              make asan       (tests under AddressSanitizer + UBSan)
#              make tsan       (agent/server test under ThreadSanitizer)
#              make valgrind   (unit tests under Valgrind)
#              make clean

CC      ?= gcc
BUILD   ?= build
SAN     ?=

WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -g -O2 -pthread -Iinclude \
           $(WARN) $(SAN) -MMD -MP
LDFLAGS := -pthread $(SAN)
LDLIBS  := -lm

LIB_NAMES := util system_info memory cpu disk network process logger metrics config protocol
LIB_OBJS  := $(LIB_NAMES:%=$(BUILD)/%.o)

BINS  := $(BUILD)/sysmon $(BUILD)/sysmon-agent $(BUILD)/sysmon-server
TESTS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_*.c))

.PHONY: all test unit smoke asan tsan valgrind clean
all: $(BINS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sysmon: $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/sysmon-agent: $(BUILD)/agent.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/sysmon-server: $(BUILD)/server.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Each tests/test_*.c becomes its own executable linked against the library objects.
$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) | $(BUILD)/tests
	$(CC) $(CFLAGS) -Itests $< $(LIB_OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD) $(BUILD)/tests:
	mkdir -p $@

unit: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

smoke: $(BINS)
	BUILD=$(BUILD) tests/smoke.sh

test: unit smoke

asan:
	$(MAKE) BUILD=build-asan SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" test

tsan:
	$(MAKE) BUILD=build-tsan SAN="-fsanitize=thread" smoke

valgrind: $(TESTS)
	@for t in $(TESTS); do echo "== valgrind $$t"; \
	    valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all -q $$t || exit 1; done

clean:
	rm -rf build build-asan build-tsan

-include $(wildcard $(BUILD)/*.d)
