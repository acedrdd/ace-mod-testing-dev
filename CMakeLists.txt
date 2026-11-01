cmake_minimum_required(VERSION 3.22)
project(ace_cull LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PRELOADER_ANDROID_ROOT "" CACHE PATH "Path to a LiteLDev/preloader-android checkout")
if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/Config.hpp")
  message(FATAL_ERROR "PRELOADER_ANDROID_ROOT must point to preloader-android (missing include/pl/Config.hpp)")
endif()

include(FetchContent)
set(ACE_DEPS)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_Declare(boost_pfr GIT_REPOSITORY https://github.com/boostorg/pfr.git GIT_TAG boost-1.88.0)
FetchContent_Declare(magic_enum GIT_REPOSITORY https://github.com/Neargye/magic_enum.git GIT_TAG v0.9.7)
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 11.2.0)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json boost_pfr magic_enum fmt)

if(NOT TARGET preloader)
  add_subdirectory("${PRELOADER_ANDROID_ROOT}" "${CMAKE_BINARY_DIR}/_preloader_android")
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

find_library(ACE_LOG_LIB log)

add_library(ace_cull SHARED AceCull.cpp)
set_target_properties(ace_cull PROPERTIES OUTPUT_NAME "ace_cull")
target_include_directories(ace_cull PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${PRELOADER_ANDROID_ROOT}/include")
target_link_libraries(ace_cull PRIVATE
  nlohmann_json::nlohmann_json Boost::pfr magic_enum::magic_enum fmt::fmt preloader)
if(ACE_LOG_LIB)
  target_link_libraries(ace_cull PRIVATE "${ACE_LOG_LIB}")
endif()
target_compile_options(ace_cull PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden -O2)
