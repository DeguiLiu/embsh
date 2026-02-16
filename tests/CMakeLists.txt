add_executable(embsh_tests
  test_command_registry.cpp
  test_line_editor.cpp
  test_telnet_server.cpp
  test_console_shell.cpp
  test_uart_shell.cpp
)
target_link_libraries(embsh_tests PRIVATE embsh Catch2::Catch2WithMain)
catch_discover_tests(embsh_tests)
