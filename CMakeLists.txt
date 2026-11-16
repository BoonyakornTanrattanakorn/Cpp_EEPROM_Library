cmake_minimum_required(VERSION 3.16)
project(Cpp_EEPROM_Library VERSION 0.0.0 LANGUAGES CXX)

add_library(eeprom INTERFACE)
add_library(eeprom::eeprom ALIAS eeprom)
target_include_directories(eeprom INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(eeprom INTERFACE cxx_std_17)
