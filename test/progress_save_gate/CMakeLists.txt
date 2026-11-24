add_executable(ProgressSaveGateTest ProgressSaveGateTest.cpp)

target_link_libraries(ProgressSaveGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressSaveGateTest)
