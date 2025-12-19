CC = gcc
AR = ar
RANLIB = ranlib

SHORT_LIBRARY_NAME = ccollections
SHARED_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).so
STATIC_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).a

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
THIRD_PARTY_DIR = third_party/facio
THIRD_PARTY_OBJ_DIR = obj/third_party
# llhttp (HTTP/1.1 parser used by chttpclient), vendored from nodejs/node's
# deps/llhttp @ llhttp v9.4.2 (node commit b59def5e8113382ac0ba1931c0168677bb2a1761).
# Kept in its own dir/object-subdirectory because llhttp's http.c and facio's
# http.c share a basename; a shared flat object dir would clobber one with the
# other depending on build order.
# Local patch: the upstream __wasm__-only llhttp_alloc/llhttp_free API (and
# its supporting wasm_on_*/wasm_settings glue) was stripped from api.c and
# llhttp.h -- this project never defines __wasm__, so that code was dead and
# unlinkable (the declarations in llhttp.h were not themselves __wasm__-gated).
# Local patch: llhttp_get_type/http_major/http_minor/method/status_code/upgrade
# take a const llhttp_t* in both llhttp.h and api.c, matching the constness of
# llhttp_get_errno/error_reason/error_pos -- all nine are read-only accessors;
# upstream just declared these six inconsistently.
# Reviewed and intentionally left unchanged: llhttp_errno_name/method_name/
# status_name (api.c) abort() on an out-of-range enum value. This is upstream
# behavior; see the comment directly above llhttp_errno_name in api.c for the
# rationale for keeping it.
# Reapply or re-evaluate these trims after any future re-vendor of llhttp.
THIRD_PARTY_LLHTTP_DIR = third_party/llhttp
THIRD_PARTY_LLHTTP_OBJ_DIR = obj/third_party/llhttp
TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -v /tau/)
COVERAGE_TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -vE '/tau/|/mixed/')

_create_object_dir := $(shell mkdir -p $(OBJECT_DIR) $(THIRD_PARTY_OBJ_DIR) $(THIRD_PARTY_LLHTTP_OBJ_DIR))

COMMON_CFLAGS = -I$(INCLUDE_DIR) -I$(THIRD_PARTY_DIR) -I$(THIRD_PARTY_LLHTTP_DIR) -DHAVE_OPENSSL=1 \
	-fstack-protector-all \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g -O3 -Werror -fPIC

# Third party sources are compiled without -Werror and with extra suppression flags
# because facil.io triggers several warnings with GCC/OpenSSL 3.0.
# -Wno-stringop-overflow: facil.io's fio_str_s implements a small-string
# optimization by reusing struct bytes *after* the single-byte `frozen` field
# as inline storage (FIO_STR_SMALL_DATA(s) = (&s->frozen)+1), with capacity
# computed as sizeof(fio_str_s) minus that offset -- provably in-bounds by
# construction. GCC's -Wstringop-overflow instead infers the target's size
# from `frozen`'s own declared type (a single byte), sees an OOB write, and
# only surfaces this at -O2/-O3 (the interprocedural inlining chain needed to
# trace it through fio_str_resize/fio_str_concat requires it) -- hence why
# this is invisible in the -O0 test builds and only appears here. Verified as
# a false positive: FIO_STR_SMALL_CAPA is derived directly from sizeof(fio_str_s),
# so the write can never exceed the struct's real allocation.
THIRD_PARTY_CFLAGS = -I$(INCLUDE_DIR) -I$(THIRD_PARTY_DIR) -I$(THIRD_PARTY_LLHTTP_DIR) -DHAVE_OPENSSL=1 \
	-fPIC -g -O3 \
	-Wno-deprecated-declarations -Wno-cast-function-type \
	-Wno-unused-parameter -Wno-sign-compare -Wno-type-limits \
	-Wno-stringop-overflow

# llhttp's own compilation gets a narrower suppression list than facio's: its
# generated state machine only ever triggers -Wunused-parameter (verified by
# compiling it under the strict -Wall -Wextra flags above -- nothing else
# fires). Reusing facio's broader THIRD_PARTY_CFLAGS here would silently mask
# a real -Wsign-compare/-Wstringop-overflow/etc. regression if a future
# llhttp re-vendor ever introduced one, since third-party objects are not
# built with -Werror.
THIRD_PARTY_LLHTTP_CFLAGS = -I$(INCLUDE_DIR) -I$(THIRD_PARTY_DIR) -I$(THIRD_PARTY_LLHTTP_DIR) -DHAVE_OPENSSL=1 \
	-fPIC -g -O3 \
	-Wno-unused-parameter

# Separate cflags for shared and static builds
SHARED_CFLAGS = $(COMMON_CFLAGS)
STATIC_CFLAGS = $(COMMON_CFLAGS)

# Separate ldflags for shared and static builds
SHARED_LDFLAGS = -shared -lpthread -lz -lssl -lcrypto -lm
STATIC_LDFLAGS =

# facil.io third_party sources (fio_tls_missing.c is excluded: conflicts with openssl)
THIRD_PARTY_SRC_FILES = \
	$(THIRD_PARTY_DIR)/fio.c \
	$(THIRD_PARTY_DIR)/fiobject.c \
	$(THIRD_PARTY_DIR)/fiobj_str.c \
	$(THIRD_PARTY_DIR)/fiobj_hash.c \
	$(THIRD_PARTY_DIR)/fiobj_ary.c \
	$(THIRD_PARTY_DIR)/fiobj_numbers.c \
	$(THIRD_PARTY_DIR)/fiobj_data.c \
	$(THIRD_PARTY_DIR)/http.c \
	$(THIRD_PARTY_DIR)/http1.c \
	$(THIRD_PARTY_DIR)/http_internal.c \
	$(THIRD_PARTY_DIR)/fio_tls_openssl.c

THIRD_PARTY_OBJ_FILES_SHARED = $(THIRD_PARTY_SRC_FILES:$(THIRD_PARTY_DIR)/%.c=$(THIRD_PARTY_OBJ_DIR)/%.o)
THIRD_PARTY_OBJ_FILES_STATIC = $(THIRD_PARTY_SRC_FILES:$(THIRD_PARTY_DIR)/%.c=$(THIRD_PARTY_OBJ_DIR)/%.static.o)

# llhttp third_party sources (pre-generated parser + hand-written API/helper glue)
THIRD_PARTY_LLHTTP_SRC_FILES = \
	$(THIRD_PARTY_LLHTTP_DIR)/llhttp.c \
	$(THIRD_PARTY_LLHTTP_DIR)/api.c \
	$(THIRD_PARTY_LLHTTP_DIR)/http.c

THIRD_PARTY_LLHTTP_OBJ_FILES_SHARED = $(THIRD_PARTY_LLHTTP_SRC_FILES:$(THIRD_PARTY_LLHTTP_DIR)/%.c=$(THIRD_PARTY_LLHTTP_OBJ_DIR)/%.o)
THIRD_PARTY_LLHTTP_OBJ_FILES_STATIC = $(THIRD_PARTY_LLHTTP_SRC_FILES:$(THIRD_PARTY_LLHTTP_DIR)/%.c=$(THIRD_PARTY_LLHTTP_OBJ_DIR)/%.static.o)

SOURCE_FILES = $(wildcard $(SOURCE_DIR)/*.c)
HEADER_FILES = $(wildcard $(INCLUDE_DIR)/*.h)
THIRD_PARTY_HEADER_FILES = $(wildcard $(THIRD_PARTY_DIR)/*.h)
THIRD_PARTY_LLHTTP_HEADER_FILES = $(wildcard $(THIRD_PARTY_LLHTTP_DIR)/*.h)
OBJ_FILES_SHARED = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)
OBJ_FILES_STATIC = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.static.o)

default: all

test:
	$(foreach folder,$(TEST_FOLDERS),cd $(folder) && make test && cd -;)

memtest:
	$(foreach folder,$(TEST_FOLDERS),cd $(folder) && make memtest && cd -;)

generate_coverage_report:
	$(foreach folder,$(COVERAGE_TEST_FOLDERS),cd $(folder) && make generate_coverage_report && cd -;)

# view_coverage_report:
# 	firefox $(for i in $(ls -1 ./tests/ | grep -vE 'mixed|tau'); do s=$(basename $i); echo ./tests/$s/coverage/src/$s.c.gcov.html; done | xargs)

all: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)

$(SHARED_LIBRARY_NAME): $(OBJ_FILES_SHARED) $(THIRD_PARTY_OBJ_FILES_SHARED) $(THIRD_PARTY_LLHTTP_OBJ_FILES_SHARED)
	$(CC) -o $(SHARED_LIBRARY_NAME) $(OBJ_FILES_SHARED) $(THIRD_PARTY_OBJ_FILES_SHARED) $(THIRD_PARTY_LLHTTP_OBJ_FILES_SHARED) $(SHARED_LDFLAGS)

$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC) $(THIRD_PARTY_OBJ_FILES_STATIC) $(THIRD_PARTY_LLHTTP_OBJ_FILES_STATIC)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(THIRD_PARTY_OBJ_FILES_STATIC) $(THIRD_PARTY_LLHTTP_OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# Objects for shared library (with -fPIC)
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# Objects for static library (still with -fPIC to allow static linking into shared libraries,
# but the static library itself will have pthread statically linked when used)
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c $(STATIC_CFLAGS) $< -o $@

# Third party shared objects (relaxed warning flags, no -Werror)
$(THIRD_PARTY_OBJ_DIR)/%.o: $(THIRD_PARTY_DIR)/%.c $(THIRD_PARTY_HEADER_FILES)
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

# Third party static objects
$(THIRD_PARTY_OBJ_DIR)/%.static.o: $(THIRD_PARTY_DIR)/%.c $(THIRD_PARTY_HEADER_FILES)
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

# llhttp shared/static objects (own object subdirectory: llhttp's http.c and
# facio's http.c share a basename, so they must not land in the same flat dir)
$(THIRD_PARTY_LLHTTP_OBJ_DIR)/%.o: $(THIRD_PARTY_LLHTTP_DIR)/%.c $(THIRD_PARTY_LLHTTP_HEADER_FILES)
	$(CC) -c $(THIRD_PARTY_LLHTTP_CFLAGS) $< -o $@

$(THIRD_PARTY_LLHTTP_OBJ_DIR)/%.static.o: $(THIRD_PARTY_LLHTTP_DIR)/%.c $(THIRD_PARTY_LLHTTP_HEADER_FILES)
	$(CC) -c $(THIRD_PARTY_LLHTTP_CFLAGS) $< -o $@

clean:
	rm -rf $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(OBJECT_DIR) \
		tests/*/tests tests/*/tests_tls tests/*/tests_mem_mgmt \
		tests/*/tests_starts_engine_first tests/*/coverage tests/*/third_party_obj \
		tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info

HEADER_INSTALL_DIR = /usr/local/include
LIBRARY_INSTALL_DIR = /usr/local/lib
MAN_INSTALL_DIR = /usr/local/share/man

# man/<module>/*.3 (functions and their companion type-safe macros, side by
# side -- see man/README) install flat into one man3 dir; real symbol names
# never collide across the two, so nothing is lost by flattening. man/<module>/*.7
# are the module overview pages. Alias pages contain a ".so <module>/<symbol>.3"
# redirect that is relative to the source tree layout, so it is rewritten to
# ".so man3/<symbol>.3" (relative to the installed MANPATH root) as part of install.
MAN3_SRC_FILES = $(wildcard man/*/*.3)
MAN7_SRC_FILES = $(wildcard man/*/*.7)

SUDO := $(shell [ "$$(id -u)" -eq 0 ] && echo "" || echo "sudo")

install: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	$(SUDO) install -d $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 644 $(HEADER_FILES) $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 755 $(SHARED_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) install -m 644 $(STATIC_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) install -d $(MAN_INSTALL_DIR)/man3 $(MAN_INSTALL_DIR)/man7
	if [ -n "$(MAN3_SRC_FILES)" ]; then $(SUDO) install -m 644 $(MAN3_SRC_FILES) $(MAN_INSTALL_DIR)/man3; fi
	if [ -n "$(MAN7_SRC_FILES)" ]; then $(SUDO) install -m 644 $(MAN7_SRC_FILES) $(MAN_INSTALL_DIR)/man7; fi
	$(SUDO) sed -i -E 's#^\.so [A-Za-z0-9_]+/#.so man3/#' $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(MAN3_SRC_FILES)))
	$(SUDO) ldconfig
	-$(SUDO) mandb -q

uninstall:
	$(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(HEADER_FILES)))
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(STATIC_LIBRARY_NAME)
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(MAN3_SRC_FILES)))
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man7/,$(notdir $(MAN7_SRC_FILES)))
	$(SUDO) ldconfig
	-$(SUDO) mandb -q
