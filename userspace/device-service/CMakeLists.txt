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

add_executable(device-service
	src/main.cpp
	src/device.cpp
	src/acquisition_worker.cpp
)

target_include_directories(device-service PRIVATE include)
target_link_libraries(device-service PRIVATE Threads::Threads)
target_compile_options(device-service PRIVATE -Wall -Wextra)
