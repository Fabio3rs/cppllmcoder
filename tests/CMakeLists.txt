add_executable(cppllmcoder_tests test.cpp test_agent_tool_parse.cpp)
target_link_libraries(cppllmcoder_tests cppllmcoder_lib GTest::GTest GTest::Main)
gtest_discover_tests(cppllmcoder_tests)
