cmake_minimum_required(VERSION 3.13)
project(armPSX C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_library(armpsx STATIC
	src/bus.c
	src/cdrom.c
	src/cpu.c
	src/dma.c
	src/gpu.c
	src/gte.c
	src/mdec.c
	src/psx.c
	src/sio.c
	src/spu.c
	src/timers.c
)
target_include_directories(armpsx PUBLIC src)
target_link_libraries(armpsx PUBLIC m)
target_compile_options(armpsx PRIVATE -O2)

add_executable(psxrun tests/psxrun.c)
target_link_libraries(psxrun armpsx)
