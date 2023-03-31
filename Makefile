CC = gcc

SHORT_LIBRARY_NAME = ccollections
LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).so

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj

_create_object_dir := $(shell mkdir -p $(OBJECT_DIR))

CFLAGS = -I$(INCLUDE_DIR) -I. -c -fPIC -fstack-protector-all \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g3 -O3 -Werror
LFLAGS = -shared -lpthread

SOURCE_FILES = $(wildcard $(SOURCE_DIR)/*.c)
HEADER_FILES = $(wildcard $(INCLUDE_DIR)/*.h)
OBJ_FILES = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)

default: all

all: $(LIBRARY_NAME) main

$(LIBRARY_NAME): $(OBJ_FILES)
	$(CC) -o $(LIBRARY_NAME) $(OBJ_FILES) $(LFLAGS)

# $(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(INCLUDE_DIR)/%.h
# 	$(CC) $(CFLAGS) $< -o $@

$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) $(CFLAGS) $< -o $@

main: main.c $(LIBRARY_NAME)
	$(CC) -L. -I$(INCLUDE_DIR) -Wall -Wextra -g3 -O0 -Werror main.c -o main \
	-l$(SHORT_LIBRARY_NAME)

clean:
	rm -rf $(LIBRARY_NAME) $(OBJECT_DIR) main test/*/tests test/*/coverage \
	test/*/*.gcno test/*/*.gcda test/*/*.gcov test/*/*.c.info

run-in-gdb:
	@LD_LIBRARY_PATH=. gdb ./main

run-in-gdb-tui:
	@LD_LIBRARY_PATH=. gdb -tui ./main

run:
	@LD_LIBRARY_PATH=. ./main

run-in-valgrind:
	@LD_LIBRARY_PATH=. valgrind --leak-check=full -s --show-leak-kinds=all ./main
