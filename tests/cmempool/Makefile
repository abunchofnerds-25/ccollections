CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILE_PREFIX = cmempool
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage 
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests

generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch -ignore-errors inconsistent --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests coverage *.gcno *.gcda *.gcov *.c.info

default: build
