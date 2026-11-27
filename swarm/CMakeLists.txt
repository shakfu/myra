# myra-swarm: parallel agents, as child myra processes or as threads on libmyra's public
# API. Not installed. Its tests run each case in both modes (docs/dev/agent-context.md).
find_package(Threads REQUIRED)
add_executable(myra-swarm swarm.c)
target_compile_options(myra-swarm PRIVATE ${WARNINGS})
target_link_libraries(myra-swarm PRIVATE myralib Threads::Threads)

if(UV)
  add_test(NAME swarm COMMAND ${UV} run --no-project --with pytest pytest -q tests
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(swarm PROPERTIES ENVIRONMENT
                       "MYRA_SWARM=$<TARGET_FILE:myra-swarm>;MYRA_BIN=$<TARGET_FILE:myra>")
endif()
