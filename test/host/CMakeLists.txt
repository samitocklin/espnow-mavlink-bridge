# Host-side unit tests for the hardware-independent components.
#   cmake -S test/host -B build/host && cmake --build build/host && ctest --test-dir build/host
# Uses Unity and mbedtls sources shipped with ESP-IDF (IDF_PATH or ~/esp/esp-idf).
cmake_minimum_required(VERSION 3.16)
project(bridge_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(DEFINED ENV{IDF_PATH})
    set(IDF_PATH $ENV{IDF_PATH})
else()
    set(IDF_PATH $ENV{HOME}/esp/esp-idf)
endif()
if(NOT EXISTS ${IDF_PATH}/components/unity/unity/src/unity.c)
    message(FATAL_ERROR "ESP-IDF not found at ${IDF_PATH}; set IDF_PATH")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SANITIZE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
set(STRICT -Wall -Wextra -Werror -Wshadow)

add_library(unity STATIC ${IDF_PATH}/components/unity/unity/src/unity.c)
target_include_directories(unity PUBLIC ${IDF_PATH}/components/unity/unity/src)

set(MBED ${IDF_PATH}/components/mbedtls/mbedtls)
add_library(mbed_sha STATIC ${MBED}/library/sha256.c ${MBED}/library/platform_util.c)
target_include_directories(mbed_sha PUBLIC ${MBED}/include ${MBED}/library)

function(bridge_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE
        ${ROOT}/components/link_proto/include
        ${ROOT}/components/mav_framer/include
        ${ROOT}/components/pairing/include)
    target_compile_options(${name} PRIVATE ${STRICT} ${SANITIZE})
    target_link_options(${name} PRIVATE ${SANITIZE})
    target_link_libraries(${name} PRIVATE unity mbed_sha)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()
bridge_test(test_link_proto   ${ROOT}/components/link_proto/link_proto.c)
bridge_test(test_mav_framer   ${ROOT}/components/mav_framer/mav_framer.c)
bridge_test(test_radio_status ${ROOT}/components/mav_framer/radio_status.c)
bridge_test(test_pairing      ${ROOT}/components/pairing/pairing.c)
