# A program outside the tree, built against an installed libmyra by tests/consumer.cmake.
cmake_minimum_required(VERSION 3.16)
project(consumer LANGUAGES C)
set(CMAKE_C_STANDARD 11)
find_package(myra REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE myra::term) # and myra::myra through it
