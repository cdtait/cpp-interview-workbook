cmake_minimum_required(VERSION 3.16)
project(cpp_interview_workbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json so the IDE's IntelliSense uses the real
# compiler flags instead of guessing (and falling back to its TagParser).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# Sanitizer build: -DSANITIZE=address|thread|none (none is the default).
# "address" enables AddressSanitizer + UndefinedBehaviorSanitizer together.
# ThreadSanitizer is incompatible with ASan, so it needs its own build directory.
set(SANITIZE "none" CACHE STRING "Sanitizer to build with: none, address, thread")
set_property(CACHE SANITIZE PROPERTY STRINGS none address thread)

if(SANITIZE STREQUAL "address")
    set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all)
elseif(SANITIZE STREQUAL "thread")
    set(SANITIZER_FLAGS -fsanitize=thread -fno-sanitize-recover=all)
elseif(NOT SANITIZE STREQUAL "none")
    message(FATAL_ERROR "Unknown SANITIZE value '${SANITIZE}' (expected none, address or thread)")
endif()

if(SANITIZER_FLAGS)
    # Sanitizers must be passed to the linker as well as the compiler, and the
    # stack traces are unreadable without frame pointers and debug info.
    add_compile_options(${SANITIZER_FLAGS} -fno-omit-frame-pointer -g)
    add_link_options(${SANITIZER_FLAGS})
endif()

# Several examples (F1) start threads, so every target links the platform
# thread library. Harmless for the single-threaded ones.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)

foreach(source_file IN LISTS SOURCES)
    get_filename_component(target_name "${source_file}" NAME_WE)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE Threads::Threads)

    # Most of the workbook is deliberately built without -O so that the source
    # and the behaviour line up. These examples are about the code the compiler
    # actually emits, so they would be meaningless unoptimised.
    if("${target_name}" MATCHES "^[JK][0-9]")
        target_compile_options("${target_name}" PRIVATE -O2)
    endif()
endforeach()
