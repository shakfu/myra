cmake_minimum_required(VERSION 3.16)
project(ant LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(AGENT_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(AGENT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(CURL REQUIRED)

# Vendored; built without our warning flags.
add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)
target_link_libraries(cjson PRIVATE m)
target_compile_options(cjson PRIVATE -Wno-deprecated-declarations) # sprintf, in upstream code

set(WARNINGS -Wall -Wextra)

add_library(agentlib STATIC src/agent.c)
target_include_directories(agentlib PUBLIC src)
target_compile_definitions(agentlib PUBLIC _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_compile_options(agentlib PRIVATE ${WARNINGS})
target_link_libraries(agentlib PUBLIC cjson CURL::libcurl)

add_executable(agent src/main.c)
target_compile_options(agent PRIVATE ${WARNINGS})
target_link_libraries(agent PRIVATE agentlib)

enable_testing()

add_executable(unit tests/unit.c)
target_compile_options(unit PRIVATE ${WARNINGS})
target_link_libraries(unit PRIVATE agentlib)
# One ctest entry per TEST(name) in unit.c; reconfigure when it changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests/unit.c)
file(STRINGS tests/unit.c UNIT_LINES REGEX "^TEST\\([a-z_0-9]+\\)")
foreach(line ${UNIT_LINES})
  string(REGEX REPLACE "^TEST\\(([a-z_0-9]+)\\).*" "\\1" name "${line}")
  add_test(NAME unit.${name} COMMAND unit ${name})
endforeach()

find_program(UV uv)
if(UV)
  add_test(NAME e2e COMMAND ${UV} run --no-project --with pytest pytest -q tests
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(e2e PROPERTIES ENVIRONMENT "AGENT_BIN=$<TARGET_FILE:agent>")
else()
  message(WARNING "uv not found: end-to-end tests (tests/test_agent.py) are not registered")
endif()
