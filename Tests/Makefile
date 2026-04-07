# Tests Makefile
#
# Builds unit tests and test pattern generator

CXX = clang++
CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -I../Include
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -I../Include

TARGETS = virtio-gpu-tests virtio-gpu-test-pattern

.PHONY: all clean run-tests

all: $(TARGETS)

virtio-gpu-tests: UnitTests.o
	$(CXX) $(CXXFLAGS) -o $@ $^

virtio-gpu-test-pattern: test-pattern.o
	$(CC) $(CFLAGS) -o $@ $^

UnitTests.o: UnitTests.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test-pattern.o: test-pattern.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o

run-tests: virtio-gpu-tests
	./virtio-gpu-tests

.PHONY: valgrind
valgrind: virtio-gpu-tests
	valgrind --leak-check=full ./virtio-gpu-tests
