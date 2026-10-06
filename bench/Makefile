# The benchmarks for every module.
#
# These link against the real libccollections.so that the root Makefile builds.
# They do not compile the sources of the library into themselves. A measured
# call therefore pays the same dynamic call cost that an application pays, and
# the measured code is the code that ships, at the flags that it ships with.
# The rpath points at the build tree, so the binary runs from a checkout with
# nothing installed.
#
# This Makefile detects the optional comparison libraries. It does not assume
# that they are there. A library that is missing removes its own comparison
# cases from the run. You need none of them to build or to run the suite.

# platform.mk picks the same default compiler as the library build. A CC= on
# the command line, or in the environment, wins over it.
include ../platform.mk
ROOT = ..

# `all` must be the default goal. Without this line, the .build-flags rule
# below becomes the first target in the file and takes that place.
.DEFAULT_GOAL := all

INCLUDE_DIR = include
EXTRA_CFLAGS ?=
# This uses -O2 and not the -O3 of the library. This is harness code. The
# library must decide the measured time, and not how much the compiler
# optimized the loop around it. The library itself is already built at -O3.
CFLAGS = -I$(ROOT)/$(INCLUDE_DIR) -I. -std=gnu11 -D_GNU_SOURCE \
	-O2 -g -Wall -Wextra -Werror $(EXTRA_CFLAGS)

LDFLAGS = -L$(ROOT) -Wl,-rpath,$(abspath $(ROOT))
LDLIBS = -lccollections -lpthread -lm

BASELINE = $(CURDIR)/baseline.json

# --- Optional comparison libraries ----------------------------------------
# uthash is one header, and it has no pkg-config file of its own. A compile
# probe is therefore the only reliable test.
# The probe uses -include and not an "#include" line in its source. A literal #
# in a Makefile needs an escape, and that escape reaches the shell. printf then
# writes a backslash, and the compiler refuses it. The probe fails for that
# reason alone, and the build reports the library as missing while it is
# installed.
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

# The public headers of the library carry inline functions, type-inferred macros
# and struct layouts. All of those end up inside these objects. An edit to one
# header therefore changes what the harness measures. Without this rule, the
# objects survive a header change while the build makes the library again
# around them. The binary then runs the inlined code of the previous build
# against the shared object of this build. A measurement must never do that.
# include/internal/ has a pattern of its own, because a *.h wildcard cannot
# match a subdirectory. Without that second pattern, no internal header is a
# prerequisite. An edit to one of them then compiles nothing here, and a later
# edit to one .c file gives you a binary whose objects disagree about that
# header.
LIB_HEADERS = $(wildcard $(ROOT)/$(INCLUDE_DIR)/*.h) \
              $(wildcard $(ROOT)/$(INCLUDE_DIR)/internal/*.h)

# An object file records nothing about the flags that made it. Without this
# stamp, a build after somebody installs or removes a comparison library, or
# after CC changes, links old objects. Those objects were compiled against the
# configuration before the change.
.build-flags: FORCE
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
.PHONY: FORCE
FORCE:

all: $(BIN)

$(BIN): $(OBJS) $(ROOT)/libccollections.so
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

# The library is a prerequisite of the link. A library that the build makes
# again after these objects therefore links the binary again. The build goes
# through the root makefile, so that `make -C bench` works from a clean
# checkout and does not report that it has no rule for the library. The root
# target is the one that knows the flags of the library.
#
# This uses FORCE and not a list of prerequisites. Without FORCE, make treats
# the symlink as current as soon as it exists, and it never goes into the root
# makefile. An edit to a library source then leaves `make -C bench` measuring
# newly compiled harness objects against the shared object of the previous
# build. A measurement must never do that. The sub-make does nothing when the
# library is current, so it links nothing without need.
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

# This measures how far the median of each case moves between whole runs of
# this library, with nothing changed in it. It records that movement as the
# gate for that case. It takes several times the real time of one run. Run it
# again whenever the machine changes, because what it records is a property of
# that machine.
# This stops the build on a flagged case. It is a separate target from `run`,
# because whether a machine repeats well enough to carry a gate is something
# that `calibrate` measures. Do not assume it. See the note on --gate in
# bench.c.
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
