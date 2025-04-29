CC = gcc
AR = ar
RANLIB = ranlib

SHORT_LIBRARY_NAME = ccollections
SHARED_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).so
STATIC_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).a

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -v /tau/)
COVERAGE_TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -vE '/tau/|/mixed/')

_create_object_dir := $(shell mkdir -p $(OBJECT_DIR))

COMMON_CFLAGS = -I$(INCLUDE_DIR) -fstack-protector-all \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g -O3 -Werror -fPIC

# Separate cflags for shared and static builds
SHARED_CFLAGS = $(COMMON_CFLAGS)
STATIC_CFLAGS = $(COMMON_CFLAGS)

# Separate ldflags for shared and static builds
SHARED_LDFLAGS = -shared -lpthread
STATIC_LDFLAGS =

SOURCE_FILES = $(wildcard $(SOURCE_DIR)/*.c)
HEADER_FILES = $(wildcard $(INCLUDE_DIR)/*.h)
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

$(SHARED_LIBRARY_NAME): $(OBJ_FILES_SHARED)
	$(CC) -o $(SHARED_LIBRARY_NAME) $(OBJ_FILES_SHARED) $(SHARED_LDFLAGS)

$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# Objects for shared library (with -fPIC)
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# Objects for static library (still with -fPIC to allow static linking into shared libraries,
# but the static library itself will have pthread statically linked when used)
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c $(STATIC_CFLAGS) $< -o $@

clean:
	rm -rf $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(OBJECT_DIR) \
		tests/*/tests tests/*/coverage \
		tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info

HEADER_INSTALL_DIR = /usr/include
LIBRARY_INSTALL_DIR = /usr/lib

SUDO := $(shell [ "$$(id -u)" -eq 0 ] && echo "" || echo "sudo")

install: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	$(SUDO) install -d $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 644 $(HEADER_FILES) $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 755 $(SHARED_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) install -m 644 $(STATIC_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) ldconfig

uninstall:
	$(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(HEADER_FILES)))
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(STATIC_LIBRARY_NAME)
	$(SUDO) ldconfig
