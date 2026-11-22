cmake_minimum_required(VERSION 3.15)
project(PwfAuthTimeSync LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AutoTimeSync supports Windows only.")
endif()

# Header-only library: link to it to get the include path and the system libraries.
add_library(pwf_timesync INTERFACE)
target_include_directories(pwf_timesync INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pwf_timesync INTERFACE cxx_std_17)
target_link_libraries(pwf_timesync INTERFACE ws2_32 advapi32 shell32 user32)

add_executable(timesync_example examples/main.cpp)
target_link_libraries(timesync_example PRIVATE pwf_timesync)
target_compile_definitions(timesync_example PRIVATE UNICODE _UNICODE)
set_target_properties(timesync_example PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(timesync_example PRIVATE /W4 /utf-8)
else()
    target_compile_options(timesync_example PRIVATE -Wall -Wextra)
    # The example uses wmain.
    target_link_options(timesync_example PRIVATE -municode)
endif()
