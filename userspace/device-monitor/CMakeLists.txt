cmake_minimum_required(VERSION 3.20)
project(device-monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(OpenSSL 3 REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(devmon-prometheus STATIC src/prometheus.cpp)
target_include_directories(devmon-prometheus PUBLIC include)
target_link_libraries(devmon-prometheus PUBLIC nlohmann_json::nlohmann_json)

add_executable(device-monitor src/main.cpp)
target_link_libraries(device-monitor PRIVATE devmon-prometheus OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(device-monitor PRIVATE -Wall -Wextra)
target_compile_options(devmon-prometheus PRIVATE -Wall -Wextra)

install(TARGETS device-monitor RUNTIME DESTINATION /opt/device-monitor)

option(BUILD_TESTING "Build device-monitor tests (requires GTest)" ON)
if(BUILD_TESTING)
	enable_testing()
	find_package(GTest REQUIRED)
	include(GoogleTest)
	add_executable(device-monitor-tests tests/test_prometheus.cpp)
	target_link_libraries(device-monitor-tests PRIVATE devmon-prometheus GTest::gtest GTest::gtest_main)
	gtest_discover_tests(device-monitor-tests DISCOVERY_MODE PRE_TEST)
	# End to end: a throwaway PKI, a fake control socket, curl and
	# openssl s_client as clients. Needs openssl, curl and python3.
	add_test(NAME mtls-end-to-end COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mtls.sh $<TARGET_FILE:device-monitor>)
endif()
