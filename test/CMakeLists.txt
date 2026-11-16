include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0)
FetchContent_MakeAvailable(googletest)

file(GLOB EEPROM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(eeprom_tests ${EEPROM_TEST_SOURCES})
target_link_libraries(eeprom_tests PRIVATE eeprom GTest::gtest_main)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eeprom_tests PRIVATE ${EEPROM_WARNINGS})
endif()

include(GoogleTest)
gtest_discover_tests(eeprom_tests)
