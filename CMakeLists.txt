cmake_minimum_required(VERSION 3.16)
project(belltracker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -mcpu=cortex-a76 -ffast-math")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED jack)
pkg_check_modules(ALSA REQUIRED alsa)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${JACK_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})

add_executable(belltracker
    main.cpp
    belltracker_cal.cpp
    belltracker_perf.cpp
    belltracker_record.cpp
)

target_link_libraries(belltracker
    ${JACK_LIBRARIES}
    ${ALSA_LIBRARIES}
    m
)
