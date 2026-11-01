# The simulated adapter is development tooling, like the tests: it uses std::string, std::thread
# and the heap freely and is outside the compliance scope of the library.
add_library(elm327_sim STATIC demo.cpp fake_elm327.cpp)
target_include_directories(elm327_sim PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(elm327_sim
    PUBLIC common::base
    PRIVATE common::options)

if(ELM327_BUILD_SIMULATOR)
    add_executable(elm327sim elm327sim.cpp)
    target_link_libraries(elm327sim PRIVATE elm327_sim common::options)
endif()
