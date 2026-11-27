# cmake -P: install libmyra's dev component from BUILD to a scratch prefix, then
# configure, build and run tests/consumer against it alone.
set(dir ${BUILD}/tmp/consumer)
set(ENV{XDG_STATE_HOME} ${dir}/state) # myra_init reads remembered state
file(REMOVE_RECURSE ${dir})
function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
  if(rc)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}")
  endif()
endfunction()
run(${CMAKE_COMMAND} --install ${BUILD} --prefix ${dir}/prefix --component dev)
set(flags)
if(SANITIZE)
  set(flags -DCMAKE_C_FLAGS=-fsanitize=address,undefined
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined)
endif()
run(${CMAKE_COMMAND} -S ${SRC}/tests/consumer -B ${dir}/build -G ${GENERATOR}
    -DCMAKE_C_COMPILER=${CC} -DCMAKE_PREFIX_PATH=${dir}/prefix ${flags})
run(${CMAKE_COMMAND} --build ${dir}/build)
run(${dir}/build/consumer)
