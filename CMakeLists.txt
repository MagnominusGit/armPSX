cmake_minimum_required(VERSION 3.13)
project(armPSX C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64)$")
	set(JIT_SOURCE src/jit.c)
else()
	set(JIT_SOURCE src/jit_none.c)
endif()
add_subdirectory(deps/libchdr EXCLUDE_FROM_ALL)

add_library(armpsx STATIC
	src/bus.c
	src/cdrom.c
	src/cpu.c
	src/disc.c
	src/dma.c
	src/gpu.c
	src/gte.c
	src/mdec.c
	${JIT_SOURCE}
	src/psx.c
	src/sio.c
	src/spu.c
	src/timers.c
)
target_include_directories(armpsx PUBLIC src)
target_link_libraries(armpsx PUBLIC chdr-static m)
target_compile_options(armpsx PRIVATE -O2)

add_executable(psxrun tests/psxrun.c)
target_link_libraries(psxrun armpsx)

add_library(armpsx_libretro SHARED libretro/libretro.c)
target_include_directories(armpsx_libretro PRIVATE libretro)
target_link_libraries(armpsx_libretro armpsx)
set_target_properties(armpsx_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
