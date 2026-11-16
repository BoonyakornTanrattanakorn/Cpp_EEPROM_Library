cmake_minimum_required(VERSION 3.16)
project(Cpp_EEPROM_Library VERSION 0.1.0 LANGUAGES CXX)

# Tests are built by default only when this is the top-level project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_eeprom_top_level ON)
else()
    set(_eeprom_top_level OFF)
endif()
option(EEPROM_BUILD_TESTS "Build the EEPROM library unit tests (GoogleTest, fetched)" ${_eeprom_top_level})

add_library(eeprom STATIC
    src/crc32.cpp
    src/log_ring.cpp
    src/status.cpp
    src/store.cpp
    src/writer.cpp)
add_library(eeprom::eeprom ALIAS eeprom)
target_include_directories(eeprom PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(eeprom PUBLIC cxx_std_17)
set_target_properties(eeprom PROPERTIES CXX_EXTENSIONS OFF)

# Warnings and language restrictions apply to this library's own sources only; consumers keep their flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(EEPROM_WARNINGS -Wall -Wextra -Wshadow -Wconversion)
    target_compile_options(eeprom PRIVATE ${EEPROM_WARNINGS} -fno-exceptions -fno-rtti)
endif()

if(EEPROM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
