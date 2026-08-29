CXX ?= g++
CXXFLAGS ?= -O0 -g -std=c++17 -I../..

.PHONY: test clean

test: test_metrics test_placement test_hot_set
	./test_metrics
	./test_placement
	./test_hot_set

test_metrics: test_metrics.cpp ../../serving/metrics.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_metrics.cpp

test_placement: test_placement.cpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_placement.cpp

test_hot_set: test_hot_set.cpp ../../serving/hot_set.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_hot_set.cpp

clean:
	rm -f test_metrics test_placement test_hot_set
