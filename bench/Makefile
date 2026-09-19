# Benchmarks for every module.
#
# These link against the real libccollections.so built by the root Makefile,
# rather than compiling the library's sources in, so that a measured call pays
# the same dynamic-call cost an application pays and the code being measured is
# the code that ships, at the flags it ships with. The rpath points at the
# build tree so the binary runs from a checkout with nothing installed.
#
# Optional comparison libraries are detected here rather than assumed. Each one
# that is missing simply removes its own comparison cases from the run; none is
# required to build or to run the suite.

# Plain assignment, not ?=: make predefines CC as `cc`, so ?= never fires and
# the benchmarks would be built with whatever cc points at rather than with the
# compiler the library itself was built with. A CC= on the command line still
# wins over this.
CC = gcc
ROOT = ..

# `all` must be the default goal; the .build-flags rule below would otherwise
# claim it by being the first target in the file.
.DEFAULT_GOAL := all

INCLUDE_DIR = include
EXTRA_CFLAGS ?=
# -O2 rather than the library's own -O3: this is harness code, and the aim is
# for the measured time to be dominated by the library, not by how aggressively
# the loop around it was optimized. The library itself is already built at -O3.
CFLAGS = -I$(ROOT)/$(INCLUDE_DIR) -I. -std=gnu11 -D_GNU_SOURCE \
	-O2 -g -Wall -Wextra -Werror $(EXTRA_CFLAGS)

LDFLAGS = -L$(ROOT) -Wl,-rpath,$(abspath $(ROOT))
LDLIBS = -lccollections -lpthread -lm

BASELINE = $(CURDIR)/baseline.json

# --- Optional comparison libraries ----------------------------------------
# uthash is a single header, so a compile probe is the only reliable test;
# it has no pkg-config file of its own.
# Compiled with -include rather than an "#include" line in the probe source: a
# literal # in a Makefile has to be escaped, and the escape survives into the
# shell, so printf emits a backslash the compiler then rejects. The probe fails
# for that reason alone and the library is reported absent while installed.
HAVE_UTHASH := $(shell echo 'int main(void){return 0;}' \
	| $(CC) -x c - -include uthash.h -o /dev/null 2>/dev/null && echo 1)
HAVE_GLIB := $(shell pkg-config --exists glib-2.0 2>/dev/null && echo 1)
HAVE_JANSSON := $(shell pkg-config --exists jansson 2>/dev/null && echo 1)
HAVE_YAML := $(shell pkg-config --exists yaml-0.1 2>/dev/null && echo 1)

ifeq ($(HAVE_UTHASH),1)
CFLAGS += -DBENCH_HAVE_UTHASH
endif
ifeq ($(HAVE_GLIB),1)
CFLAGS += -DBENCH_HAVE_GLIB $(shell pkg-config --cflags glib-2.0)
LDLIBS += $(shell pkg-config --libs glib-2.0)
endif
ifeq ($(HAVE_JANSSON),1)
CFLAGS += -DBENCH_HAVE_JANSSON $(shell pkg-config --cflags jansson)
LDLIBS += $(shell pkg-config --libs jansson)
endif
ifeq ($(HAVE_YAML),1)
CFLAGS += -DBENCH_HAVE_YAML $(shell pkg-config --cflags yaml-0.1)
LDLIBS += $(shell pkg-config --libs yaml-0.1)
endif

SRCS = bench.c bench_core.c bench_maps.c bench_concurrency.c bench_cache.c \
       bench_logging.c bench_serialization.c bench_http.c
OBJS = $(SRCS:.c=.o)
BIN = bench

# The library's public headers carry inline functions, type-safe macros and
# struct layouts that end up compiled into these objects, so an edit to one of
# them changes what the harness measures. Without this the objects survive a
# header change while the library is rebuilt around them, and the binary then
# runs last build's inlined code against this build's shared object, which is
# the one thing a measurement must never do.
LIB_HEADERS = $(wildcard $(ROOT)/$(INCLUDE_DIR)/*.h)

# An object file records nothing about the flags that produced it, so a rebuild
# after a comparison library is installed or removed, or after CC changes, would
# otherwise relink stale objects compiled against the previous configuration.
.build-flags: FORCE
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
.PHONY: FORCE
FORCE:

all: $(BIN)

$(BIN): $(OBJS) $(ROOT)/libccollections.so
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

# Listed as a prerequisite of the link so that a library rebuilt after these
# objects relinks the binary, and built through the root makefile so that
# `make -C bench` works from a clean checkout rather than reporting no rule for
# it. The root target is the one that knows the library's own flags.
#
# FORCE, not a prerequisite list: without it make treats the symlink as up to
# date the moment it exists and never descends, so an edit to a library source
# leaves `make -C bench` measuring freshly compiled harness objects against the
# previous build's shared object, which is the one thing a measurement must
# never do. The sub-make is a no-op when the library is current, so nothing is
# relinked needlessly.
$(ROOT)/libccollections.so: FORCE
	@$(MAKE) --no-print-directory -C $(ROOT) $(notdir $@)

%.o: %.c bench.h $(LIB_HEADERS) .build-flags
	$(CC) -c $(CFLAGS) $< -o $@

.PHONY: run
run: $(BIN)
	./$(BIN) --baseline=$(BASELINE) $(BENCH_ARGS)

.PHONY: update
update: $(BIN)
	./$(BIN) --baseline=$(BASELINE) --update $(BENCH_ARGS)

# Measures how far each case's own median moves between whole runs of this
# unchanged library, and records that as the case's gate. Takes several times
# one run's wall clock, and is worth repeating whenever the machine it runs on
# changes, since what it records is a property of that machine.
# Fails the build on a flagged case. Separate from `run` because whether a
# machine repeats well enough to carry a gate is something `calibrate` measures
# rather than something to assume; see the note on --gate in bench.c.
.PHONY: gate
gate: $(BIN)
	./$(BIN) --baseline=$(BASELINE) --gate $(BENCH_ARGS)

.PHONY: calibrate
calibrate: $(BIN)
	./$(BIN) --baseline=$(BASELINE) --calibrate $(BENCH_ARGS)

.PHONY: list
list: $(BIN)
	./$(BIN) --list

.PHONY: clean
clean:
	rm -f $(BIN) $(OBJS) .build-flags

.PHONY: config
config:
	@echo "uthash:  $(if $(HAVE_UTHASH),present,absent)"
	@echo "glib:    $(if $(HAVE_GLIB),present,absent)"
	@echo "jansson: $(if $(HAVE_JANSSON),present,absent)"
	@echo "libyaml: $(if $(HAVE_YAML),present,absent)"
