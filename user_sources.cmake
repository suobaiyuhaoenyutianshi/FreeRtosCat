# 1. 收集所有 .c（只收集 .c，不要收集 .h）
file(GLOB_RECURSE USER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/freeRTOS/Src/*.c"
)

# 2. 编译源文件
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})

# 3. 添加头文件搜索路径（让编译器能找到你写的 .h）
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/freeRTOS/Inc"
)


