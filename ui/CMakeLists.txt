# myra-ui: a desktop front end on CUI (vendor/cui). Built with -DMYRA_UI=ON, or `make test-ui`.
find_package(Threads REQUIRED)

# The amalgamated library, built without our warnings. On macOS its window layer is
# Objective-C, so the one file compiles as such.
add_library(cui STATIC ${CMAKE_SOURCE_DIR}/vendor/cui/cui.c)
target_include_directories(cui PUBLIC ${CMAKE_SOURCE_DIR}/vendor/cui)
target_compile_options(cui PRIVATE -w)
if(APPLE)
  enable_language(OBJC)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/vendor/cui/cui.c PROPERTIES LANGUAGE OBJC)
  target_compile_options(cui PRIVATE -fobjc-arc)
  target_compile_definitions(cui PRIVATE _DARWIN_C_SOURCE) # strict C11 hides u_int and friends
  target_link_libraries(cui PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL")
elseif(UNIX)
  find_package(X11 REQUIRED)
  target_link_libraries(cui PUBLIC X11::X11 ${CMAKE_DL_LIBS} m Threads::Threads)
endif()

add_library(transcript STATIC transcript.c)
target_include_directories(transcript PUBLIC .)
target_compile_options(transcript PRIVATE ${WARNINGS})
target_link_libraries(transcript PUBLIC Threads::Threads)

add_executable(myra-ui main.c view.c worker.c)
target_compile_options(myra-ui PRIVATE ${WARNINGS})
target_link_libraries(myra-ui PRIVATE cui myralib transcript)
install(TARGETS myra-ui RUNTIME DESTINATION bin)

add_executable(ui_transcript tests/test_transcript.c)
target_compile_options(ui_transcript PRIVATE ${WARNINGS})
target_link_libraries(ui_transcript PRIVATE transcript)
add_test(NAME ui.transcript COMMAND ui_transcript)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME ui.smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py
           $<TARGET_FILE:myra-ui>)
  set_tests_properties(ui.smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()
