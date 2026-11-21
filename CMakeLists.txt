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
	src/gles.c
	src/glr.c
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
find_package(Threads REQUIRED)
target_link_libraries(armpsx PUBLIC chdr-static m Threads::Threads ${CMAKE_DL_LIBS})
target_compile_options(armpsx PRIVATE -O2)
# Hidden: inside the libretro core its functions call each other directly, not through the PLT
set_target_properties(armpsx PROPERTIES C_VISIBILITY_PRESET hidden)
# GCC's -O2 cost model won't vectorize the scanline output's 16- to 32-bit widening
set_source_files_properties(src/gpu.c PROPERTIES COMPILE_OPTIONS $<$<C_COMPILER_ID:GNU>:-fvect-cost-model=dynamic>)

# Vulkan: the GL renderer's GL done with Vulkan (src/vkgl.c, from armN64), its shaders compiled by
# glslang, built in from ARMPSX_GLSLANG_DIR.
option(ARMPSX_VULKAN "Build the Vulkan renderer" ON)
set(ARMPSX_GLSLANG_DIR "${CMAKE_SOURCE_DIR}/deps/glslang" CACHE PATH "glslang's source")
if(ARMPSX_VULKAN AND EXISTS ${ARMPSX_GLSLANG_DIR}/glslang/MachineIndependent/ShaderLang.cpp)
	enable_language(CXX)
	include(cmake/glslang.cmake)
	target_sources(armpsx PRIVATE src/vkgl.c)
	target_include_directories(armpsx PUBLIC deps/vulkan/include)
	target_compile_definitions(armpsx PUBLIC ARMPSX_VULKAN)
	target_link_libraries(armpsx PUBLIC glslang)
	set(ARMPSX_LINKER_LANGUAGE CXX)
else()
	set(ARMPSX_LINKER_LANGUAGE C)
endif()

add_executable(psxrun tests/psxrun.c)
set_target_properties(psxrun PROPERTIES LINKER_LANGUAGE ${ARMPSX_LINKER_LANGUAGE})
target_link_libraries(psxrun armpsx)

add_library(armpsx_libretro SHARED libretro/libretro.c)
target_include_directories(armpsx_libretro PRIVATE libretro)
target_link_libraries(armpsx_libretro armpsx)
set_target_properties(armpsx_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden LINKER_LANGUAGE ${ARMPSX_LINKER_LANGUAGE})
