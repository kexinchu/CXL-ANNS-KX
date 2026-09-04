CXX ?= g++
CXXFLAGS ?= -O0 -g -std=c++17 -I../..

.PHONY: test clean

test: test_metrics test_placement test_hot_set test_diskann_entry test_nav_graph test_score_cache test_prefetch_hub test_pq_table test_extent_run
	./test_metrics
	./test_placement
	./test_hot_set
	./test_diskann_entry
	./test_nav_graph
	./test_score_cache
	./test_prefetch_hub
	./test_pq_table
	./test_extent_run

test_metrics: test_metrics.cpp ../../serving/metrics.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_metrics.cpp

test_placement: test_placement.cpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_placement.cpp

test_hot_set: test_hot_set.cpp ../../serving/hot_set.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_hot_set.cpp

test_diskann_entry: test_diskann_entry.cpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_diskann_entry.cpp

test_nav_graph: test_nav_graph.cpp ../../serving/nav_graph.hpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_nav_graph.cpp

test_score_cache: test_score_cache.cpp ../../serving/score_cache.hpp ../../serving/placement.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_score_cache.cpp

test_prefetch_hub: test_prefetch_hub.cpp ../../serving/prefetch_hub.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_prefetch_hub.cpp

test_pq_table: test_pq_table.cpp ../../serving/pq_table.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_pq_table.cpp

test_extent_run: test_extent_run.cpp ../../serving/hide_fill.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_extent_run.cpp

clean:
	rm -f test_metrics test_placement test_hot_set test_diskann_entry test_nav_graph test_score_cache test_prefetch_hub test_pq_table test_extent_run
