# 1. 收集所有 .c（只收集 .c，不要收集 .h）
file(GLOB_RECURSE USER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/freeRTOS/Src/*.c"
)

# system_stm32f1xx.c 已由 CubeMX 生成的 STM32_Drivers 目标编译，
# 这里必须排除，否则两个目标各编一份，链接报 multiple definition
list(REMOVE_ITEM USER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/system_stm32f1xx.c"
)

# 2. 编译源文件
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})

# 3. 添加头文件搜索路径（让编译器能找到你写的 .h）
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/freeRTOS/Inc"
)


