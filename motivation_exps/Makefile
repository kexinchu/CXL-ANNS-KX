CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -fopenmp -Iinclude
LDFLAGS ?= -lnuma -fopenmp

BIN := motivation

all: $(BIN)

$(BIN): src/main.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean
