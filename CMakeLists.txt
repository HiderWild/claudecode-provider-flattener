cmake_minimum_required(VERSION 3.16)
project(model-gateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DCPPHTTPLIB_NO_SSL)
add_definitions(-D_WIN32_WINNT=0x0A00)

set(SOURCES
    src/main.cpp
    src/http_client.cpp
    src/providers.cpp
    src/server_thread_pool.cpp
)

add_executable(model-gateway ${SOURCES})

target_include_directories(model-gateway PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(model-gateway
    ws2_32
    winhttp
)
