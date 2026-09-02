cmake_minimum_required(VERSION 3.20)
project(device-service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)

add_executable(device-service
	src/main.cpp
	src/device.cpp
	src/read_exact.cpp
	src/acquisition_worker.cpp
	src/config.cpp
	src/metrics.cpp
	src/watchdog.cpp
)

target_include_directories(device-service PRIVATE include ${SYSTEMD_INCLUDE_DIRS})
target_link_libraries(device-service PRIVATE Threads::Threads nlohmann_json::nlohmann_json spdlog::spdlog ${SYSTEMD_LIBRARIES})
target_compile_options(device-service PRIVATE -Wall -Wextra)

option(BUILD_TESTING "Build device-service-tests (requires GTest)" ON)
if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
