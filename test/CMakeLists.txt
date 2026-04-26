cmake_minimum_required(VERSION 3.16)
project(threepyrix-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(APPLE AND NOT CMAKE_OSX_SYSROOT)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE XCRUN_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(XCRUN_SDK_PATH)
    set(CMAKE_OSX_SYSROOT "${XCRUN_SDK_PATH}" CACHE STRING "macOS SDK" FORCE)
  endif()
endif()

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

add_library(test_support STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common/test_utils.cpp
)

target_include_directories(test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/common
)

function(add_3pyrix_test TEST_NAME)
  add_executable(${TEST_NAME} ${ARGN})
  target_include_directories(${TEST_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  target_include_directories(${TEST_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${PROJECT_ROOT}/src
    ${PROJECT_ROOT}/src/content
    ${PROJECT_ROOT}/src/core
    ${PROJECT_ROOT}/src/states
    ${PROJECT_ROOT}/lib/PageCache/src
    ${PROJECT_ROOT}/lib/RenderTypes/src
    ${PROJECT_ROOT}/lib/Serialization/src
  )
  target_link_libraries(${TEST_NAME} PRIVATE test_support)
endfunction()

add_3pyrix_test(
  ReaderNavigationTest
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/navigation/ReaderNavigationTest.cpp
  ${PROJECT_ROOT}/src/content/ReaderNavigation.cpp
)

add_3pyrix_test(
  MappedInputManagerTest
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/input/MappedInputManagerTest.cpp
  ${PROJECT_ROOT}/src/MappedInputManager.cpp
)

add_3pyrix_test(
  PageCacheHeaderTest
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/pagecache/PageCacheHeaderTest.cpp
  ${PROJECT_ROOT}/lib/PageCache/src/PageCacheHeader.cpp
)

add_3pyrix_test(
  ReaderTocNavigationTest
  ${CMAKE_CURRENT_SOURCE_DIR}/unit/toc/ReaderTocNavigationTest.cpp
  ${PROJECT_ROOT}/src/states/reader/ReaderTocNavigation.cpp
)
