cmake_minimum_required(VERSION 3.14)
project(embsh VERSION 0.1.0 LANGUAGES CXX)

# Header-only interface library
add_library(embsh INTERFACE)
target_include_directories(embsh INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(embsh INTERFACE cxx_std_17)

# Options
option(EMBSH_BUILD_TESTS    "Build unit tests (Catch2)"   ON)
option(EMBSH_BUILD_EXAMPLES "Build example programs"      OFF)
option(EMBSH_NO_EXCEPTIONS  "Compile with -fno-exceptions" OFF)

if(EMBSH_NO_EXCEPTIONS)
  target_compile_options(embsh INTERFACE -fno-exceptions -fno-rtti)
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
if(EMBSH_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://ghfast.top/https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(CTest)
  include(Catch)

  add_subdirectory(tests)
endif()

# --------------------------------------------------------------------------
# Examples
# --------------------------------------------------------------------------
if(EMBSH_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
