# Built as one of optee_examples' subdirectories (the OP-TEE build globs
# every directory there that has a CMakeLists.txt for the client and a
# ta/Makefile for the TA) - see docs/security/optee.md for how it is linked
# in.
project (devid C)

add_executable (devid host/main.c)
target_include_directories (devid PRIVATE ta/include)
target_link_libraries (devid PRIVATE teec)

install (TARGETS devid DESTINATION ${CMAKE_INSTALL_BINDIR})
