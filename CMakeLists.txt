cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME CARTESTFreertos)


# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Enable CMake support for ASM and C languages


# Core project settings
project(${CMAKE_PROJECT_NAME})
enable_language(C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Debug 构型覆盖：CubeMX 生成的 toolchain 用 -O0 -g3，代码量会超出 64KB Flash，
# 链接失败后 .elf 不生成，调试器就报 "Unable to find executable file"。
# 换成 -Og：体积接近 Release，同时保留 -g3 源码级调试信息。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    string(REPLACE "-O0 -g3" "-Og -g3" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REPLACE "-O0 -g3" "-Og -g3" CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS}")
endif()

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
include(${CMAKE_CURRENT_SOURCE_DIR}/user_sources.cmake)

# 每次构建都打印 RAM/FLASH 占用表（格式同 -Wl,--print-memory-usage）。
# ALL 目标不比较时间戳，零改动也执行，所以不会再被 "no work to do" 吞掉。
# 区域大小取自 stm32f103c8tx_flash.ld：FLASH 64K / RAM 20K
add_custom_target(size ALL
    COMMAND ${CMAKE_COMMAND}
            -DSIZE_TOOL=${CMAKE_SIZE}
            -DELF=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
            -DFLASH_SIZE=65536
            -DRAM_SIZE=20480
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/print_size.cmake
    COMMENT "Flash/RAM usage: ${CMAKE_PROJECT_NAME}.elf"
    VERBATIM
)
add_dependencies(size ${CMAKE_PROJECT_NAME})
