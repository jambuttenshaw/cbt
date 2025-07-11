cmake_minimum_required(VERSION 3.18)

project(cbt)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=1")
endif()

option(DONUT_WITH_ASSIMP "" OFF)
option(DONUT_WITH_DX11 "" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")

add_subdirectory(donut)

include(donut/compileshaders.cmake)

macro(GroupSources curdir)
    file(GLOB children RELATIVE ${PROJECT_SOURCE_DIR}/${curdir} ${PROJECT_SOURCE_DIR}/${curdir}/*)
    foreach(child ${children})
        if(IS_DIRECTORY ${PROJECT_SOURCE_DIR}/${curdir}/${child})
            GroupSources(${curdir}/${child})
        else()
            string(REPLACE "/" "\\" groupname ${curdir})
            source_group(${groupname} FILES ${PROJECT_SOURCE_DIR}/${curdir}/${child})
        endif()
    endforeach()
endmacro()

file(GLOB_RECURSE shaders "shaders/*.hlsl" "shaders/*.hlsli" "shaders/*.h")
GroupSources("shaders")
file(GLOB_RECURSE sources "source/*.cpp" "source/*.h")
GroupSources("source")

set(libcbt_shaders "${CMAKE_CURRENT_SOURCE_DIR}/libcbt/hlsl")
set(libleb_shaders "${CMAKE_CURRENT_SOURCE_DIR}/libleb/hlsl")

set(project cbt)
set(folder "cbt")

donut_compile_shaders_all_platforms(
    TARGET ${project}_shaders
    PROJECT_NAME "cbt"
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
    SHADERMAKE_OPTIONS "-I ${libcbt_shaders} -I ${libleb_shaders}"
    SOURCES ${shaders}
    FOLDER ${folder}
    OUTPUT_BASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${project}
)

add_executable(${project} WIN32 ${sources})
target_include_directories(${project} PUBLIC "shaders")
target_include_directories(${project} PUBLIC "libcbt")
target_include_directories(${project} PUBLIC "libleb")
target_link_libraries(${project} donut_app donut_engine donut_render)
add_dependencies(${project} ${project}_shaders)
set_target_properties(${project} PROPERTIES FOLDER ${folder})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()
