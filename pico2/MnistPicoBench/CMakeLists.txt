# Pico 2 (RP2350 / Cortex-M33) MNIST 推論跑分
#
# 為什麼不用 VSCode 的 Pico 擴充建 project:
#   擴充本身不帶 SDK,它只是幫你下載 + 產生這份 CMakeLists。
#   我們已經自己把 SDK 放好了(~/.pico-sdk/sdk/2.1.1),所以直接手寫,
#   路徑一目瞭然,將來在別台機器上也只要改最上面那幾行。
#
# 建置(在 Git Bash / PowerShell 都可以,路徑用正斜線):
#   cmake -S . -B build -G Ninja \
#         -DPICO_SDK_PATH=C:/Users/M90t/.pico-sdk/sdk/2.1.1 \
#         -Dpicotool_DIR=D:/workdir/pico-deps/picotool-2.1.1/picotool \
#         -DCMSIS_NN_PATH=D:/workdir/pico-deps/CMSIS-NN
#   cmake --build build
# 產出:build/mnist_pico_bench.uf2

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# pico2 = RP2350。這一行一定要在 pico_sdk_init() 之前,
# 它決定了 SDK 幫我們選哪一組 board header 和哪個 CPU 架構。
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(mnist_pico_bench C CXX ASM)

pico_sdk_init()

set(CMSIS_NN_PATH "D:/workdir/pico-deps/CMSIS-NN" CACHE PATH "CMSIS-NN 原始碼位置")

# ---------------------------------------------------------------------------
# CMSIS-NN
# ---------------------------------------------------------------------------
# 不用它自己的 CMakeLists(那份會另外開一個 project(),
# 跟 pico-sdk 的 toolchain 設定容易打架),直接把原始碼收進來自己編。
# v7.0.0 是自給自足的:它只 include 自己的標頭 + <stdint.h>,
# 不需要 CMSIS-DSP,也不需要 CMSIS-Core。要不要走 SIMD 路徑是靠
# __ARM_FEATURE_DSP 這個編譯器內建巨集決定的 —— Cortex-M33 有 DSP 擴充,
# arm-none-eabi-gcc 編 -mcpu=cortex-m33 時會自動定義它,所以我們什麼都不用加。
file(GLOB_RECURSE CMSIS_NN_SRC "${CMSIS_NN_PATH}/Source/*.c")

add_library(cmsis_nn STATIC ${CMSIS_NN_SRC})
target_include_directories(cmsis_nn PUBLIC "${CMSIS_NN_PATH}/Include")
# CMSIS-NN 官方就是用 -Ofast 發布效能數字的。這裡跟著用,
# 否則量到的會是「沒開優化的 CMSIS-NN」,對它不公平。
target_compile_options(cmsis_nn PRIVATE -Ofast)

# ---------------------------------------------------------------------------
# 跑分主程式
# ---------------------------------------------------------------------------
add_executable(mnist_pico_bench
    main.c
    net_float.c
    net_int8.c
)

target_include_directories(mnist_pico_bench PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# naive 版本也用 -O2。
# 這點值得說清楚:如果 naive 版故意不開優化,那「量化 + SIMD 快 N 倍」
# 就有一部分是編譯器旗標的功勞,不是硬體或演算法的功勞。
# 三個模式都在同一組旗標下編,倍數才有意義。
target_compile_options(mnist_pico_bench PRIVATE -O2)

target_link_libraries(mnist_pico_bench
    pico_stdlib
    cmsis_nn
    m               # net_float.c 用了 expf
)

# 兩條路都開:
#   USB CDC  插上 USB 就有一個虛擬 COM,不用額外接線
#   UART0    GP0/GP1,接 USB-TTL 用,跟之前手動量測時一樣
# 兩邊會印出同樣的內容,愛用哪個都行。
pico_enable_stdio_usb(mnist_pico_bench 1)
pico_enable_stdio_uart(mnist_pico_bench 1)

# 產生 .uf2(BOOTSEL 拖檔用)以及 .elf / .bin / .dis / .map
pico_add_extra_outputs(mnist_pico_bench)
