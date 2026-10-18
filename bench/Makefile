# Benchmarks for every module.
#
# They link against the real shared library that the root Makefile builds
# (libccollections.so, or libccollections.dylib on macOS) instead of compiling
# the library sources into themselves. That way a measured call pays the same
# dynamic call cost an application pays, and the code we measure is the code
# that ships, built with the flags it ships with. The rpath points at the build
# tree, so the binary runs from a checkout without installing anything.
#
# The optional comparison libraries are detected, not assumed. A missing one
# just drops its own comparison cases from the run, and you need none of them
# to build or run the suite.

# platform.mk picks the same default compiler as the library build, and a CC=
# on the command line or in the environment wins over it.
include ../platform.mk
ROOT = ..

# `all` has to be the default goal. Without this line the .build-flags rule
# below would be the first target in the file and take that place.
.DEFAULT_GOAL := all

INCLUDE_DIR = include
EXTRA_CFLAGS ?=
# -O2 here and not the library's -O3, because this is harness code: the
# measured time has to come from the library, not from how hard the compiler
# optimized the loop around it. The library itself is built at -O3 anyway.
CFLAGS = -I$(ROOT)/$(INCLUDE_DIR) -I. -std=gnu11 -D_GNU_SOURCE \
	-O2 -g -Wall -Wextra -Werror $(EXTRA_CFLAGS)

LIBRARY = $(ROOT)/libccollections.$(CCOL_SHARED_LIBRARY_SUFFIX)
LDFLAGS = -L$(ROOT) -Wl,-rpath,$(abspath $(ROOT))
LDLIBS = -lccollections -lpthread -lm

BASELINE = $(CURDIR)/baseline.json

# --- Optional comparison libraries ----------------------------------------
# uthash is a single header without a pkg-config file, so a compile probe is
# the only reliable test.
# The probe uses -include instead of an "#include" line in its source. A
# literal # in a Makefile needs an escape, the escape reaches the shell and
# printf writes a backslash that the compiler refuses. The probe would then fail
# for that reason alone and the build would report the library as missing while
# it's installed.
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

# The library's public headers carry inline functions, type-inferred macros
# and struct layouts, and all of them end up inside these objects, so editing a
# header changes what the harness measures. Without this rule the objects
# survive a header change while the library is built again around them, and
# the binary runs the inlined code of the previous build against this build's
# shared object, which a measurement must never do. include/internal/ gets a
# pattern of its own because a *.h wildcard can't match a subdirectory. Without
# that second pattern no internal header is a prerequisite, so editing one
# compiles nothing here, and a later edit to one .c file gives you a binary
# whose objects disagree about that header.
LIB_HEADERS = $(wildcard $(ROOT)/$(INCLUDE_DIR)/*.h) \
              $(wildcard $(ROOT)/$(INCLUDE_DIR)/internal/*.h)

# An object file records nothing about the flags that made it. Without this
# stamp, a build after you install or remove a comparison library, or change
# CC, links old objects compiled against the configuration you had before.
.build-flags: FORCE
	@echo '$(CC) $(CFLAGS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS)' > $@
.PHONY: FORCE
FORCE:

all: $(BIN)

# On macOS a program records the library by its install name, an absolute
# path in the build's LIBDIR, and dyld then ignores the rpath, so the link
# rewrites that entry to point at the library in the build tree.
$(BIN): $(OBJS) $(LIBRARY)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
ifeq ($(CCOL_UNAME_S),Darwin)
	install_name_tool -change "$$(otool -D $(LIBRARY) | tail -n 1)" \
		"$(abspath $(LIBRARY))" $@
endif

# The library is a prerequisite of the link, so if the build makes it again
# after these objects the binary is linked again too. We go through the root
# makefile so that `make -C bench` works from a clean checkout instead of
# saying it has no rule for the library, and the root target is the one that
# knows the library's flags.
#
# It uses FORCE instead of a list of prerequisites. Without FORCE make takes
# the symlink as current as soon as it exists and never goes into the root
# makefile, so after an edit to a library source `make -C bench` would measure
# freshly compiled harness objects against the shared object of the previous
# build, which a measurement must never do. When the library is current the
# sub-make does nothing, so nothing gets linked for no reason.
$(LIBRARY): FORCE
	@$(MAKE) --no-print-directory -C $(ROOT) $(notdir $@)

%.o: %.c bench.h $(LIB_HEADERS) .build-flags
	$(CC) -c $(CFLAGS) $< -o $@

.PHONY: run
run: $(BIN)
	./$(BIN) --baseline=$(BASELINE) $(BENCH_ARGS)

.PHONY: update
update: $(BIN)
	./$(BIN) --baseline=$(BASELINE) --update $(BENCH_ARGS)

# This one measures how far the median of each case moves between whole runs
# of the library with nothing changed in it, and records that movement as the
# gate for the case. It takes several times as long as one run. Run it again
# whenever the machine changes, because what it records belongs to the machine.
# The gate target stops the build on a flagged case. It's kept apart from
# `run` because whether a machine repeats well enough to carry a gate is
# something `calibrate` measures, not something we assume. See the note on
# --gate in bench.c.
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
