# elm327ctl, the command-line front-end: one command per run, or the interactive shell. Everything
# but main.cpp is the elm327ctl_core library, so the tests can run commands and the shell in-process.
add_library(elm327ctl_core STATIC
    commands.cpp
    console.cpp
    line_editor.cpp
    options.cpp
    shell.cpp
    terminal.cpp)
# The sources include each other as "cli/<file>.hpp".
target_include_directories(elm327ctl_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(elm327ctl_core
    PUBLIC elm327::elm327
    PRIVATE common::options)

if(ELM327_BUILD_CLI)
    add_executable(elm327ctl main.cpp)
    target_link_libraries(elm327ctl PRIVATE elm327ctl_core common::options)
    install(TARGETS elm327ctl RUNTIME DESTINATION bin)
endif()
