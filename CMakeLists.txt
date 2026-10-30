cmake_minimum_required(VERSION 3.10)
project(ems_gateway CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBMODBUS_ROOT ${CMAKE_SOURCE_DIR}/third_party/libmodbus)

add_executable(ems_gateway
    src/main.cpp
    src/RealTimeDB.cpp
    src/HistoryStore.cpp
    src/MqttBridge.cpp
    src/Logger.cpp
    src/PointTemplate.cpp
    src/DeviceInstance.cpp
    src/DevicePoller.cpp
    src/PollerManager.cpp
    src/ModbusClient.cpp
    src/ModbusServer.cpp
    src/PointGroup.cpp
)

target_include_directories(ems_gateway PRIVATE
    inc
    ${LIBMODBUS_ROOT}/include
)

find_package(SQLite3 REQUIRED)
target_include_directories(ems_gateway PRIVATE ${SQLite3_INCLUDE_DIRS})

find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MOSQUITTO_LIBRARY mosquitto)
if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
    message(FATAL_ERROR "libmosquitto development files are required (install libmosquitto-dev)")
endif()
target_include_directories(ems_gateway PRIVATE ${MOSQUITTO_INCLUDE_DIR})

if(CMAKE_CXX_COMPILER MATCHES "aarch64")
    message(STATUS "目标平台: ARM64")
    target_link_libraries(ems_gateway PRIVATE
        ${LIBMODBUS_ROOT}/lib/static-arm64/libmodbus.a
    )
else()
    message(STATUS "目标平台: x86")
    target_link_libraries(ems_gateway PRIVATE
        ${LIBMODBUS_ROOT}/lib/static-x86/libmodbus.a
    )
endif()

target_link_libraries(ems_gateway PRIVATE pthread ${SQLite3_LIBRARIES} ${MOSQUITTO_LIBRARY})


add_custom_command(TARGET ems_gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        ${CMAKE_BINARY_DIR}/config
)
