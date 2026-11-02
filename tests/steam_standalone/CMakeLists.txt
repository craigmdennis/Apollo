cmake_minimum_required(VERSION 3.20)
project(test_steam_standalone CXX)

# Builds the platform-neutral Steam activity reader and its tests without the rest of Apollo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(APOLLO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

set(INSTALL_GTEST OFF)
add_subdirectory("${APOLLO_DIR}/third-party/googletest" googletest)

add_executable(${PROJECT_NAME}
        "${APOLLO_DIR}/src/steam_activity.cpp"
        "${APOLLO_DIR}/tests/unit/test_steam_activity.cpp")
target_include_directories(${PROJECT_NAME} PRIVATE "${APOLLO_DIR}")
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json gtest gtest_main)
