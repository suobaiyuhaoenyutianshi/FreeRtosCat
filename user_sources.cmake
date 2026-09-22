# 自动收集 Core/Src 和 Core/Inc 下你自己添加的所有 .c 和 .h 文件
file(GLOB_RECURSE USER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/*.h"
)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
# 把这些文件加到你的可执行目标里
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})

# 如果没有 Inc 目录也可以，C 文件会自动被包含