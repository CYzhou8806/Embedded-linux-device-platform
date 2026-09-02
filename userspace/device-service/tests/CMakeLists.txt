find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(device-service-tests
	test_sequence_tracker.cpp
	test_config.cpp
	test_ring_buffer.cpp
	test_read_exact.cpp
	${CMAKE_SOURCE_DIR}/src/config.cpp
	${CMAKE_SOURCE_DIR}/src/read_exact.cpp
)

target_include_directories(device-service-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(device-service-tests PRIVATE GTest::gtest GTest::gtest_main Threads::Threads nlohmann_json::nlohmann_json)

gtest_discover_tests(device-service-tests)
