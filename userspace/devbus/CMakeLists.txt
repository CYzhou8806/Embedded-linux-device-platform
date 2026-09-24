cmake_minimum_required(VERSION 3.20)
project(devbus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(devbus
	src/os.cpp
	src/core.cpp
	src/introspect.cpp
)
target_include_directories(devbus PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:include>
)
target_link_libraries(devbus PUBLIC Threads::Threads)
target_compile_options(devbus PRIVATE -Wall -Wextra -Wpedantic)

add_executable(devbus-ls tools/devbus_ls.cpp)
target_link_libraries(devbus-ls PRIVATE devbus)

add_executable(devbus-example-pub examples/sample_publisher.cpp)
target_link_libraries(devbus-example-pub PRIVATE devbus)
add_executable(devbus-example-sub examples/sample_subscriber.cpp)
target_link_libraries(devbus-example-sub PRIVATE devbus)
add_executable(acq-bridge examples/acq_bridge.cpp)
target_link_libraries(acq-bridge PRIVATE devbus)

# The queue benchmark compares against device-service's existing
# mutex+condvar RingBuffer, so it borrows that one header.
add_executable(devbus-bench bench/devbus_bench.cpp)
target_include_directories(devbus-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../device-service/include)
target_link_libraries(devbus-bench PRIVATE devbus)

install(TARGETS devbus devbus-ls devbus-bench acq-bridge RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
install(DIRECTORY include/devbus DESTINATION include)  # includes acq_sample.hpp, which device-service also uses

option(BUILD_TESTING "Build devbus-tests (requires GTest)" ON)
if(BUILD_TESTING)
	enable_testing()
	find_package(GTest REQUIRED)
	include(GoogleTest)
	add_executable(devbus-tests tests/test_devbus.cpp)
	target_link_libraries(devbus-tests PRIVATE devbus GTest::gtest GTest::gtest_main)
	# PRE_TEST: list the tests when ctest runs, not at build time. Discovery at
	# build time executes the test binary, which fails in a cross build (an
	# aarch64 binary on an x86 host, e.g. with the Yocto SDK).
	gtest_discover_tests(devbus-tests DISCOVERY_MODE PRE_TEST)
endif()
