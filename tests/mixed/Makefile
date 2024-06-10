CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILES = ../../src/cvector.c \
            ../../src/chashmap.c \
            ../../src/cbstmap.c \
            ../../src/cstring.c \
            ../../src/common.c \
            ../../src/csort.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lm -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s --show-leak-kinds=all ./tests

generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-mixed && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file mixed.c.info && \
	genhtml --ignore-errors inconsistent mixed.c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest

clean:
	rm -rf tests coverage *.gcda *.gcdo *.gcno *.gcov

default: build
