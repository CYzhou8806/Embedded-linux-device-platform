find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(device-service-tests
	test_sequence_tracker.cpp
	test_config.cpp
	test_ring_buffer.cpp
	test_read_exact.cpp
	test_backpressure_controller.cpp
	test_device_state.cpp
	test_clock_calibrator.cpp
	test_supervisor.cpp
	test_control_server.cpp
	${CMAKE_SOURCE_DIR}/src/config.cpp
	${CMAKE_SOURCE_DIR}/src/read_exact.cpp
	${CMAKE_SOURCE_DIR}/src/backpressure_controller.cpp
	${CMAKE_SOURCE_DIR}/src/device.cpp
	${CMAKE_SOURCE_DIR}/src/device_state.cpp
	${CMAKE_SOURCE_DIR}/src/clock_calibrator.cpp
	${CMAKE_SOURCE_DIR}/src/fault_evidence.cpp
	${CMAKE_SOURCE_DIR}/src/supervisor.cpp
	${CMAKE_SOURCE_DIR}/src/control_server.cpp
	${CMAKE_SOURCE_DIR}/src/commands.cpp
	${CMAKE_SOURCE_DIR}/src/watchdog.cpp
	${CMAKE_SOURCE_DIR}/src/acquisition_worker.cpp
	${CMAKE_SOURCE_DIR}/src/latency_logger.cpp
	${CMAKE_SOURCE_DIR}/src/sample_publisher.cpp
)

target_include_directories(device-service-tests PRIVATE ${CMAKE_SOURCE_DIR}/include ${DEVBUS_INCLUDE_DIR})
target_link_libraries(device-service-tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads nlohmann_json::nlohmann_json spdlog::spdlog ${DEVBUS_TARGET})

# PRE_TEST: list the tests when ctest runs, not at build time. Discovery at
# build time executes the test binary, which fails in a cross build (an
# aarch64 binary on an x86 host, e.g. with the Yocto SDK).
gtest_discover_tests(device-service-tests DISCOVERY_MODE PRE_TEST)
