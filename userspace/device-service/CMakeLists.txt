cmake_minimum_required(VERSION 3.20)
project(device-service LANGUAGES C CXX)

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
	src/latency_logger.cpp
	src/metrics.cpp
	src/watchdog.cpp
	src/backpressure_controller.cpp
	src/sample_publisher.cpp
	src/device_state.cpp
	src/clock_calibrator.cpp
	src/fault_evidence.cpp
	src/supervisor.cpp
	src/control_server.cpp
	src/commands.cpp
)

# Operator client for the control socket (plain C, no dependencies - it
# has to run on a BusyBox image with nothing else installed).
add_executable(device-ctl tools/device-ctl.c)
set_target_properties(device-ctl PROPERTIES LINKER_LANGUAGE C)
target_compile_options(device-ctl PRIVATE -Wall -Wextra)

# devbus (userspace/devbus) carries the shared-memory pub/sub this service
# publishes acq/samples on. Two ways to find it, in this order:
#   1. installed (Yocto: the devbus recipe puts headers and libdevbus.a in
#      the sysroot, and device-service's recipe DEPENDS on it);
#   2. the sibling source tree, for a development-host build where nothing
#      has been installed.
option(BUILD_TESTING "Build device-service-tests (requires GTest)" ON)

find_path(DEVBUS_INCLUDE_DIR devbus/devbus.hpp)
find_library(DEVBUS_LIBRARY devbus)
if(DEVBUS_INCLUDE_DIR AND DEVBUS_LIBRARY)
	message(STATUS "devbus: using installed ${DEVBUS_LIBRARY}")
	set(DEVBUS_TARGET ${DEVBUS_LIBRARY})
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../devbus/CMakeLists.txt)
	message(STATUS "devbus: building the sibling source tree")
	# devbus's own tests are not our concern here. Turned off with a normal
	# variable that shadows the cache entry only for the add_subdirectory()
	# call - a CACHE FORCE here would also switch off device-service's own
	# tests below, which is what it silently did before.
	set(BUILD_TESTING OFF)
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../devbus ${CMAKE_BINARY_DIR}/devbus EXCLUDE_FROM_ALL)
	unset(BUILD_TESTING)
	set(DEVBUS_TARGET devbus)
	set(DEVBUS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../devbus/include)
else()
	message(FATAL_ERROR "devbus not found: install it, or keep userspace/devbus next to this directory")
endif()

target_include_directories(device-service PRIVATE include ${SYSTEMD_INCLUDE_DIRS} ${DEVBUS_INCLUDE_DIR})
target_link_libraries(device-service PRIVATE Threads::Threads nlohmann_json::nlohmann_json spdlog::spdlog ${SYSTEMD_LIBRARIES} ${DEVBUS_TARGET})
target_compile_options(device-service PRIVATE -Wall -Wextra)

# Matches README.md's manual install convention (/opt/device-service/) and
# systemd/device-service.service's ExecStart - both the manual and the
# Yocto recipe (yocto/meta-device-platform) install paths agree.
install(TARGETS device-service device-ctl RUNTIME DESTINATION /opt/device-service)

if(BUILD_TESTING)
	enable_testing()
	add_subdirectory(tests)
endif()
