cmake_minimum_required(VERSION 3.16)
project(model-gateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT)

set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/http_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/providers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/server_thread_pool.cpp
)

add_executable(model-gateway ${SOURCES})

target_include_directories(model-gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

find_package(Threads REQUIRED)
target_link_libraries(model-gateway
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    "-framework Security"
    "-framework CoreFoundation"
    "-framework CFNetwork"
)
