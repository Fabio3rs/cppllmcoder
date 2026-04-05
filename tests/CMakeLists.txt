
add_executable(cppllmcoder_tests
    test.cpp
    test_agent_tool_parse.cpp
    test_tool_registry.cpp
    test_lua_context.cpp
    test_schema.cpp
    test_session.cpp
    test_sqlite3raii.cpp
    test_brain_store.cpp
    test_chat_stream.cpp
    test_agent_run_step.cpp
    test_fs_tools.cpp
    test_db_tools.cpp
    test_utf8_text.cpp
)
target_compile_definitions(cppllmcoder_tests PRIVATE "CMAKE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(cppllmcoder_tests cppllmcoder_lib GTest::GTest GTest::Main)
target_include_directories(cppllmcoder_tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-httplib
)
# Mock server for OpenAI-compatible endpoints (chat, embeddings, models)
target_sources(cppllmcoder_tests PRIVATE
    mock_openai_server.cpp
)
# Keeping cpp-httplib header-only for now to avoid optional TLS/zip deps; if we need
# those features later, swap this for add_subdirectory(vendor/cpp-httplib) and link
# httplib::httplib.
gtest_discover_tests(cppllmcoder_tests TEST_LIST allTests PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0:suppressions=${CMAKE_SOURCE_DIR}/lsan.supp"
    DISCOVERY_MODE PRE_TEST)
