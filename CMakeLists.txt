cmake_minimum_required(VERSION 3.16)
project(myra VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MYRA_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(MYRA_WERROR "Treat our own warnings as errors (CI)" OFF)
option(MYRA_UI "Build myra-ui, the CUI desktop front end in ui/" OFF)
if(MYRA_SANITIZE)
  # no-recover: undefined behaviour fails the test instead of printing a warning.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Let the linker drop what nothing reaches: most of vendored linenoise is unused here.
add_compile_options(-ffunction-sections -fdata-sections)
if(APPLE)
  add_link_options(-Wl,-dead_strip) # ld64's spelling of --gc-sections
else()
  add_link_options(-Wl,--gc-sections)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(GNUInstallDirs)

# Vendored; built without our warning flags. Its objects go into libmyra, so an
# installed libmyra is one archive.
add_library(cjson OBJECT vendor/cjson/cJSON.c)
target_compile_options(cjson PRIVATE -Wno-deprecated-declarations) # sprintf, in upstream code

add_library(linenoise STATIC
            vendor/linenoise/src/linenoise.c
            vendor/linenoise/src/internal/completion.c
            vendor/linenoise/src/internal/history.c
            vendor/linenoise/src/internal/keyparser.c
            vendor/linenoise/src/internal/render.c
            vendor/linenoise/src/internal/terminal_posix.c
            vendor/linenoise/src/internal/utf8.c)
target_include_directories(linenoise PUBLIC vendor/linenoise/include
                                     PRIVATE vendor/linenoise/include/internal)
# Upstream builds as gnu99; we build as strict c11, which hides strdup, fdopen and
# strcasecmp. Without these their returns pass through int and pointers are truncated.
target_compile_definitions(linenoise PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)

set(WARNINGS -Wall -Wextra)
if(MYRA_WERROR)
  list(APPEND WARNINGS -Werror) # our targets only: vendored code keeps its warnings
endif()

# The library: public headers in include/myra/, sources and private util.h in lib/.
# In-tree targets also get the feature macros; an installed consumer does not. A target
# that uses util.h adds lib/ itself, so myra-swarm and tests see only the public headers.
add_library(myralib STATIC lib/agent.c lib/api.c lib/loop.c lib/state.c lib/text.c lib/tools.c
                    lib/mcp.c $<TARGET_OBJECTS:cjson>)
set_target_properties(myralib PROPERTIES OUTPUT_NAME myra EXPORT_NAME myra)
target_include_directories(myralib PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                                          $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/cjson>
                                          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(myralib PUBLIC $<BUILD_INTERFACE:_DEFAULT_SOURCE>
                           $<BUILD_INTERFACE:_DARWIN_C_SOURCE>
                           $<BUILD_INTERFACE:MYRA_VERSION="${PROJECT_VERSION}">)
target_include_directories(myralib PRIVATE lib)
target_compile_options(myralib PRIVATE ${WARNINGS})
target_link_libraries(myralib PUBLIC CURL::libcurl Threads::Threads PRIVATE m)

# myra on a terminal: replies, notes and permission questions, through myralib's hooks.
add_library(myraterm STATIC term/term.c)
set_target_properties(myraterm PROPERTIES OUTPUT_NAME myra-term EXPORT_NAME term)
target_include_directories(myraterm PRIVATE lib) # util.h
target_compile_options(myraterm PRIVATE ${WARNINGS})
target_link_libraries(myraterm PUBLIC myralib)

# The agent executable.
add_executable(myra cli/main.c cli/acp.c cli/family.c)
target_include_directories(myra PRIVATE lib) # util.h
target_compile_options(myra PRIVATE ${WARNINGS})
target_link_libraries(myra PRIVATE myraterm linenoise)

install(TARGETS myra RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT bin)

# The libraries for other programs: find_package(myra), then link myra::myra, and
# myra::term for a terminal front end.
include(CMakePackageConfigHelpers)
set(MYRA_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/myra)
install(TARGETS myralib myraterm EXPORT myraTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev)
install(FILES include/myra/myra.h include/myra/mcp.h include/myra/term.h vendor/cjson/cJSON.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/myra COMPONENT dev)
install(FILES vendor/cjson/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME cJSON-LICENSE
        COMPONENT dev)
install(EXPORT myraTargets NAMESPACE myra:: DESTINATION ${MYRA_CMAKE_DIR} COMPONENT dev)
file(WRITE ${CMAKE_BINARY_DIR}/myraConfig.cmake
     "include(CMakeFindDependencyMacro)\nfind_dependency(CURL)\nfind_dependency(Threads)\n"
     "include(\"\${CMAKE_CURRENT_LIST_DIR}/myraTargets.cmake\")\n")
# 0.x: a minor release may break the API.
write_basic_package_version_file(${CMAKE_BINARY_DIR}/myraConfigVersion.cmake
                                 COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_BINARY_DIR}/myraConfig.cmake ${CMAKE_BINARY_DIR}/myraConfigVersion.cmake
        DESTINATION ${MYRA_CMAKE_DIR} COMPONENT dev)

enable_testing()

if(MYRA_UI)
  add_subdirectory(ui)
endif()

add_executable(unit tests/unit.c)
target_compile_options(unit PRIVATE ${WARNINGS})
target_link_libraries(unit PRIVATE myraterm)
# One ctest entry per TEST(name) in unit.c; reconfigure when it changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests/unit.c)
file(STRINGS tests/unit.c UNIT_LINES REGEX "^TEST\\([a-z_0-9]+\\)")
foreach(line ${UNIT_LINES})
  string(REGEX REPLACE "^TEST\\(([a-z_0-9]+)\\).*" "\\1" name "${line}")
  add_test(NAME unit.${name} COMMAND unit ${name})
endforeach()

# Install the dev component to a scratch prefix and build tests/consumer against it.
add_test(NAME install.consumer
         COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DSRC=${CMAKE_SOURCE_DIR}
                 -DGENERATOR=${CMAKE_GENERATOR} -DCC=${CMAKE_C_COMPILER}
                 -DSANITIZE=${MYRA_SANITIZE} -P ${CMAKE_SOURCE_DIR}/tests/consumer.cmake)

find_program(UV uv)
if(UV)
  add_test(NAME e2e COMMAND ${UV} run --no-project --with pytest pytest -q tests
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(e2e PROPERTIES ENVIRONMENT "MYRA_BIN=$<TARGET_FILE:myra>")
  # The same suite against scripts/agent.py. The tests set PATH=/usr/bin:/bin, where python3
  # may predate 3.11, so a wrapper pins the interpreter found here.
  find_package(Python3 3.11 COMPONENTS Interpreter)
  if(Python3_FOUND)
    file(WRITE ${CMAKE_BINARY_DIR}/tmp/agent-py
         "#!/bin/sh\nexec '${Python3_EXECUTABLE}' '${CMAKE_SOURCE_DIR}/scripts/agent.py' \"$@\"\n")
    file(COPY ${CMAKE_BINARY_DIR}/tmp/agent-py DESTINATION ${CMAKE_BINARY_DIR}
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
    add_test(NAME e2e-py COMMAND ${UV} run --no-project --with pytest pytest -q tests
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(e2e-py PROPERTIES ENVIRONMENT "MYRA_BIN=${CMAKE_BINARY_DIR}/agent-py")
  else()
    message(WARNING "Python 3.11+ not found: e2e-py (scripts/agent.py) is not registered")
  endif()
else()
  message(WARNING "uv not found: end-to-end tests (tests/test_agent.py) are not registered")
endif()

# myra-swarm: parallel agents on one libmyra; uses UV from above for its tests.
add_subdirectory(swarm)
