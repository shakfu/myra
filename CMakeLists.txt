cmake_minimum_required(VERSION 3.16)
project(myra VERSION 0.1.2 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MYRA_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(MYRA_WERROR "Treat our own warnings as errors (CI)" OFF)
if(MYRA_SANITIZE)
  # no-recover: undefined behaviour fails the test instead of printing a warning.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(CURL REQUIRED)

# Vendored; built without our warning flags.
add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)
target_link_libraries(cjson PRIVATE m)
target_compile_options(cjson PRIVATE -Wno-deprecated-declarations) # sprintf, in upstream code

set(WARNINGS -Wall -Wextra)
if(MYRA_WERROR)
  list(APPEND WARNINGS -Werror) # our targets only: vendored cJSON keeps its warnings
endif()

add_library(myralib STATIC src/myra.c)
target_include_directories(myralib PUBLIC src)
target_compile_definitions(myralib PUBLIC _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_compile_options(myralib PRIVATE ${WARNINGS})
target_link_libraries(myralib PUBLIC cjson CURL::libcurl)

add_executable(myra src/main.c)
target_compile_options(myra PRIVATE ${WARNINGS})
target_compile_definitions(myra PRIVATE MYRA_VERSION="${PROJECT_VERSION}")
target_link_libraries(myra PRIVATE myralib)

# Line editing and history for the REPL; macOS ships libedit, Linux has libedit-dev.
find_path(EDITLINE_INCLUDE editline/readline.h)
find_library(EDIT_LIB edit)
if(EDITLINE_INCLUDE AND EDIT_LIB)
  target_compile_definitions(myra PRIVATE HAVE_LIBEDIT)
  target_include_directories(myra PRIVATE ${EDITLINE_INCLUDE})
  target_link_libraries(myra PRIVATE ${EDIT_LIB})
else()
  message(STATUS "libedit not found: the REPL has no line editing")
endif()

install(TARGETS myra RUNTIME DESTINATION bin)

enable_testing()

add_executable(unit tests/unit.c)
target_compile_options(unit PRIVATE ${WARNINGS})
target_link_libraries(unit PRIVATE myralib)
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
  set_tests_properties(e2e PROPERTIES ENVIRONMENT "MYRA_BIN=$<TARGET_FILE:myra>")
else()
  message(WARNING "uv not found: end-to-end tests (tests/test_agent.py) are not registered")
endif()
