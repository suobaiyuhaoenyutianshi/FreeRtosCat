# 由 CMakeLists.txt 的 size 目标在每次构建后调用，输出与链接器
# -Wl,--print-memory-usage 相同格式的 RAM/FLASH 占用表（不重新链接也能看）。
#
# 调用方式：
#   cmake -DSIZE_TOOL=arm-none-eabi-size -DELF=xxx.elf \
#         -DFLASH_SIZE=65536 -DRAM_SIZE=20480 -P print_size.cmake

if(NOT EXISTS "${ELF}")
    message(FATAL_ERROR "找不到 ${ELF}，请先完整构建一次")
endif()

# size 输出第 2 行是 text/data/bss 三个字节数
execute_process(COMMAND ${SIZE_TOOL} ${ELF}
                OUTPUT_VARIABLE out
                ERROR_QUIET)
string(REPLACE "\r" "" out "${out}")
string(REPLACE "\n" ";" lines "${out}")
list(GET lines 1 nums)
string(REGEX MATCHALL "[^ \t]+" nums "${nums}")
list(GET nums 0 text)
list(GET nums 1 data)
list(GET nums 2 bss)

# 与 ld 口径一致：Flash = 代码 + 初值（.data 的加载副本在 Flash）
#                  RAM   = 初值 + 未初始化段（bss 含 bss 和 _user_heap_stack）
math(EXPR flash_used "${text} + ${data}")
math(EXPR ram_used "${data} + ${bss}")

# 用整数运算凑出两位小数，避免依赖浮点
function(_percent used total var)
    math(EXPR h "(${used} * 10000) / ${total}")
    math(EXPR i "${h} / 100")
    math(EXPR f "${h} % 100")
    if(f LESS 10)
        set(f "0${f}")
    endif()
    set(${var} "${i}.${f}%" PARENT_SCOPE)
endfunction()

_percent(${ram_used} ${RAM_SIZE} ram_pct)
_percent(${flash_used} ${FLASH_SIZE} flash_pct)
math(EXPR ram_kb "${RAM_SIZE} / 1024")
math(EXPR flash_kb "${FLASH_SIZE} / 1024")

message("Memory region         Used Size  Region Size  %age Used")
message("           FLASH:     ${flash_used} B      ${flash_kb} KB     ${flash_pct}")
message("             RAM:     ${ram_used} B      ${ram_kb} KB     ${ram_pct}")
