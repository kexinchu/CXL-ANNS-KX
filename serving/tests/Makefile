CXX ?= g++
CXXFLAGS ?= -O0 -g -std=c++17 -I../..

.PHONY: test clean

test: test_metrics test_placement test_hot_set test_diskann_entry test_nav_graph test_score_cache test_distance_metric test_pq_table test_eval_trace test_extent_run test_cont_batch test_dram_window test_page_copy_pool test_vmem_read_only
	./test_metrics
	./test_placement
	./test_hot_set
	./test_diskann_entry
	./test_nav_graph
	./test_score_cache
	./test_distance_metric
	./test_pq_table
	./test_eval_trace
	./test_extent_run
	./test_cont_batch
	./test_dram_window
	./test_page_copy_pool
	./test_vmem_read_only

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

test_distance_metric: test_distance_metric.cpp ../../serving/distance_metric.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_distance_metric.cpp

test_pq_table: test_pq_table.cpp ../../serving/pq_table.hpp ../../serving/distance_metric.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_pq_table.cpp

test_eval_trace: test_eval_trace.cpp ../../serving/eval_trace.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_eval_trace.cpp

test_extent_run: test_extent_run.cpp ../../serving/hide_fill.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_extent_run.cpp

test_cont_batch: test_cont_batch.cpp ../../serving/cont_batch.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_cont_batch.cpp

test_dram_window: test_dram_window.cpp ../../serving/dram_window.hpp
	$(CXX) $(CXXFLAGS) -o $@ test_dram_window.cpp

test_page_copy_pool: test_page_copy_pool.cpp ../../serving/page_copy_pool.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ test_page_copy_pool.cpp

test_vmem_read_only: test_vmem_read_only.cpp ../../serving/search_beam.cpp
	$(CXX) $(CXXFLAGS) -mavx2 -mfma -pthread -o $@ test_vmem_read_only.cpp -lnuma

clean:
	rm -f test_metrics test_placement test_hot_set test_diskann_entry test_nav_graph test_score_cache test_distance_metric test_pq_table test_eval_trace test_extent_run test_cont_batch test_dram_window test_page_copy_pool test_vmem_read_only
