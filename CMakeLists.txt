cmake_minimum_required(VERSION 3.16)
project(model-gateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_definitions(-DCPPHTTPLIB_NO_SSL)
    add_definitions(-D_WIN32_WINNT=0x0A00)
    set(PLATFORM_DIR win32)
    set(PLATFORM_LIBS ws2_32 winhttp)
elseif(APPLE)
    set(PLATFORM_DIR darwin)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT)
    set(PLATFORM_LIBS
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        "-framework Security"
        "-framework CoreFoundation"
        "-framework CFNetwork"
    )
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

set(SOURCES
    ${PLATFORM_DIR}/main.cpp
    ${PLATFORM_DIR}/http_client.cpp
    src/providers.cpp
    src/server_thread_pool.cpp
)

add_executable(model-gateway ${SOURCES})

target_include_directories(model-gateway PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/${PLATFORM_DIR}
)

target_link_libraries(model-gateway
    ${PLATFORM_LIBS}
)

enable_testing()

if(WIN32)
    add_test(
        NAME win32_smoke
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tests/win32_smoke.ps1 $<TARGET_FILE:model-gateway>
    )
elseif(APPLE)
    add_test(
        NAME darwin_single_instance
        COMMAND /bin/bash ${CMAKE_SOURCE_DIR}/tests/darwin_single_instance.sh $<TARGET_FILE:model-gateway>
    )
endif()
