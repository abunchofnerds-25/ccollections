CC = gcc

SHORT_LIBRARY_NAME = ccollections
LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).so

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -v /tau/)

_create_object_dir := $(shell mkdir -p $(OBJECT_DIR))

CFLAGS = -I$(INCLUDE_DIR) -fstack-protector-all \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g3 -O3 -Werror
LFLAGS = -shared -lpthread

SOURCE_FILES = $(wildcard $(SOURCE_DIR)/*.c)
HEADER_FILES = $(wildcard $(INCLUDE_DIR)/*.h)
OBJ_FILES = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)

default: all

test:
	$(foreach folder,$(TEST_FOLDERS),cd $(folder) && make test && cd -;)

memtest:
	$(foreach folder,$(TEST_FOLDERS),cd $(folder) && make memtest && cd -;)

generate_coverage_report:
	$(foreach folder,$(TEST_FOLDERS),cd $(folder) && make generate_coverage_report && cd -;)

all: $(LIBRARY_NAME) main

$(LIBRARY_NAME): $(OBJ_FILES)
	$(CC) -o $(LIBRARY_NAME) $(OBJ_FILES) $(LFLAGS)

$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c -fPIC $(CFLAGS) $< -o $@

main: main.c $(LIBRARY_NAME)
	$(CC) -L. $(CFLAGS) main.c -o main -l$(SHORT_LIBRARY_NAME) -lm

clean:
	rm -rf $(LIBRARY_NAME) $(OBJECT_DIR) main tests/*/tests tests/*/coverage \
	tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info

run-in-gdb:
	@LD_LIBRARY_PATH=. gdb ./main

run-in-gdb-tui:
	@LD_LIBRARY_PATH=. gdb -tui ./main

run:
	@LD_LIBRARY_PATH=. ./main

run-in-valgrind:
	@LD_LIBRARY_PATH=. valgrind --leak-check=full -s --show-leak-kinds=all ./main

HEADER_INSTALL_DIR = /usr/include
LIBRARY_INSTALL_DIR = /usr/lib

SUDO := $(shell [ "$$(id -u)" -eq 0 ] && echo "" || echo "sudo")

install: $(LIBRARY_NAME)
	$(SUDO) install -d $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 644 $(HEADER_FILES) $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 755 $(LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) ldconfig

uninstall:
	$(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(HEADER_FILES)))
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(LIBRARY_NAME)
	$(SUDO) ldconfig
