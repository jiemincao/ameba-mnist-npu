#!/bin/bash
# 在 acuity 容器內執行一段指令,工作目錄固定在 ~/ameba-toolkit 對應的 /workspace。
# 用法: sh dk.sh '<在容器內要跑的 bash 指令>'
cd "$HOME/ameba-toolkit" || exit 1
exec docker run --rm \
    -e HOME=/workspace \
    -e ACUITY_PATH=/usr/local/acuity_command_line_tools \
    -w /workspace \
    -v "$PWD:/workspace" \
    ghcr.io/ameba-aiot/acuity-toolkit:6.18.8 \
    bash -lc "$1"
