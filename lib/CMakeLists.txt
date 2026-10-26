# libelm327: everything a front-end needs to control an ELM327 adapter. Sources mirror the header
# groups in include/elm327/<group>/:
# - adapter: the serial connection to the adapter and the classification of its replies;
# - obd: the OBD-II services (messages, PIDs, DTCs, VIN) and ObdClient, which requests them;
# - controller: Controller, the connection the front-ends share, with the texts that explain
#   failures, the clean stop on Ctrl+C and the timing of periodic reads.
add_library(elm327 STATIC
    src/adapter/adapter.cpp
    src/adapter/response.cpp
    src/controller/controller.cpp
    src/controller/explain.cpp
    src/controller/timing.cpp
    src/expert/coding.cpp
    src/expert/feature.cpp
    src/expert/uds.cpp
    src/obd/client.cpp
    src/obd/dtc.cpp
    src/obd/message.cpp
    src/obd/pid.cpp
    src/obd/service.cpp
    src/obd/vin.cpp)
# The I/O of the adapter, event descriptors and the stop signals are mostly platform-specific: a
# <name>_posix.cpp and a <name>_win32.cpp each.
if(WIN32)
    target_sources(elm327 PRIVATE
        src/adapter/adapter_win32.cpp src/controller/event_win32.cpp src/controller/signals_win32.cpp)
else()
    target_sources(elm327 PRIVATE
        src/adapter/adapter_posix.cpp src/adapter/tcp_link_posix.cpp src/controller/bridged_link_posix.cpp
        src/controller/event_posix.cpp src/controller/signals_posix.cpp)
endif()
add_library(elm327::elm327 ALIAS elm327)

configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/elm327/version.hpp" @ONLY)

target_include_directories(elm327
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")  # Internal helpers: "detail/<name>.hpp".
target_link_libraries(elm327
    PUBLIC common::common
    PRIVATE common::options)
