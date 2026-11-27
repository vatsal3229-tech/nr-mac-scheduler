add_executable(unit_tests
  test_cqi.cpp
  test_schedulers.cpp
  test_cell.cpp
  test_policy_comparison.cpp
  test_kpi.cpp
  test_wire.cpp
  test_queue.cpp
  test_config.cpp
  test_trace.cpp
  test_prometheus.cpp
)
target_link_libraries(unit_tests PRIVATE nrs_runtime GTest::gtest_main nrs_warnings)

include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)
