cmake_minimum_required(VERSION 3.20)
project(test_mic_standalone CXX)

# Builds the platform-neutral mic units and their tests without the rest of Apollo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(APOLLO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(OpenSSL REQUIRED)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

set(INSTALL_GTEST OFF)
add_subdirectory("${APOLLO_DIR}/third-party/googletest" googletest)

set(MIC_SOURCES "${APOLLO_DIR}/src/crypto.cpp")
foreach(unit mic_protocol mic_jitter mic_store mic_pairing)
    if(EXISTS "${APOLLO_DIR}/src/${unit}.cpp")
        list(APPEND MIC_SOURCES "${APOLLO_DIR}/src/${unit}.cpp")
    endif()
endforeach()

file(GLOB MIC_TEST_SOURCES CONFIGURE_DEPENDS "${APOLLO_DIR}/tests/unit/test_mic_*.cpp")

add_executable(${PROJECT_NAME} ${MIC_SOURCES} ${MIC_TEST_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE "${APOLLO_DIR}")
# The Apollo test suite defines SUNSHINE_SOURCE_DIR. The mic tests use it to find tests/fixtures.
target_compile_definitions(${PROJECT_NAME} PRIVATE SUNSHINE_SOURCE_DIR="${APOLLO_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json gtest gtest_main)
