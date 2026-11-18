add_executable(bt_cli bt_cli.cpp)
target_link_libraries(bt_cli PRIVATE bt_engine)

# Writes a small procedural photo set with analyzer output, for tests of
# bindings that cannot link the C++ test fixture directly.
add_executable(bt_fixture bt_fixture.cpp)
target_link_libraries(bt_fixture PRIVATE bt_engine)
target_include_directories(bt_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
