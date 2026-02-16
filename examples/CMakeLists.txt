if(NOT EMBSH_BUILD_EXAMPLES)
  return()
endif()

add_executable(embsh_basic_demo basic_demo.cpp)
target_link_libraries(embsh_basic_demo PRIVATE embsh)

add_executable(embsh_console_demo console_demo.cpp)
target_link_libraries(embsh_console_demo PRIVATE embsh)

add_executable(embsh_multi_backend multi_backend.cpp)
target_link_libraries(embsh_multi_backend PRIVATE embsh)
