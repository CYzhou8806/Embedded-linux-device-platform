find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(device-service-tests
	test_sequence_tracker.cpp
	test_config.cpp
	test_ring_buffer.cpp
	test_read_exact.cpp
	test_backpressure_controller.cpp
	${CMAKE_SOURCE_DIR}/src/config.cpp
	${CMAKE_SOURCE_DIR}/src/read_exact.cpp
	${CMAKE_SOURCE_DIR}/src/backpressure_controller.cpp
	${CMAKE_SOURCE_DIR}/src/device.cpp
)

target_include_directories(device-service-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(device-service-tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads nlohmann_json::nlohmann_json spdlog::spdlog)

# PRE_TEST: list the tests when ctest runs, not at build time. Discovery at
# build time executes the test binary, which fails in a cross build (an
# aarch64 binary on an x86 host, e.g. with the Yocto SDK).
gtest_discover_tests(device-service-tests DISCOVERY_MODE PRE_TEST)
