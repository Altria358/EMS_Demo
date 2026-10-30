#!/bin/bash
# 停止 dev-up.sh 拉起的 v2 进程（只停脚本记录 PID 的进程）
set -uo pipefail
cd "$(dirname "$0")"

RUN_DIR="logs/v2"
for name in web ems; do
    pid_file="$RUN_DIR/$name.pid"
    [ -f "$pid_file" ] || continue
    pid="$(cat "$pid_file")"
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null && echo "⏹  已停止 $name (pid $pid)"
    else
        echo "ℹ️  $name (pid $pid) 已不在运行"
    fi
    rm -f "$pid_file"
done
