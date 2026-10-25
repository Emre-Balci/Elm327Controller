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
    src/controller/signals.cpp
    src/controller/timing.cpp
    src/obd/client.cpp
    src/obd/dtc.cpp
    src/obd/message.cpp
    src/obd/pid.cpp
    src/obd/vin.cpp)
add_library(elm327::elm327 ALIAS elm327)

configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/elm327/version.hpp" @ONLY)

target_include_directories(elm327 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(elm327
    PUBLIC common::common
    PRIVATE common::options)
