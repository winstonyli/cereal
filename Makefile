# cereal - C99 toolchain with preprocessor analysis
CC      ?= cc
HOSTCC  ?= $(CC)
CFLAGS  ?= -O2 -g
WARN     = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wno-unused-parameter
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc
BUILD    = build

SRCS := $(wildcard src/*.c) $(wildcard src/analysis/*.c)
OBJS := $(SRCS:%.c=$(BUILD)/%.o) $(BUILD)/gen/host_config.o
DEPS := $(OBJS:.o=.d)

all: cereal

cereal: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/gen/host_config.c: tools/probe-host.sh
	sh tools/probe-host.sh "$(HOSTCC)" $@

$(BUILD)/gen/host_config.o: $(BUILD)/gen/host_config.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: cereal
	sh tests/run.sh

check: test

clean:
	rm -rf $(BUILD) cereal

.PHONY: all test check clean
-include $(DEPS)
