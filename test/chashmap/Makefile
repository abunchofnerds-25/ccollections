CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILE_PREFIX = chashmap
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c
ALL_SRC_FILES = tests.c $(SRC_FILES)
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage 
LFLAGS =

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test:
	./tests

memtest:
	valgrind --leak-check=full ./tests

generate_coverage_report:
	$(CC)  $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests coverage *.gcno *.gcda *.gcov *.c.info

default: build
