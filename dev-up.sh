#!/bin/bash
# 一键拉起 v2（数据库版）：EMS + Web 后端，两者通过同一个 SQLite 文件交换数据。
# v2 不依赖 MQTT Broker。
# 用法: ./dev-up.sh        停止: ./dev-down.sh
set -euo pipefail
cd "$(dirname "$0")"

CONFIG_DIR="${CONFIG_DIR:-config/simulator}"
DB_PATH="${EMS_DB_FILE:-data/ems-v2.db}"
HTTP_PORT="${EMS_WEB_PORT:-8080}"
WS_PORT="${EMS_WEB_WS_PORT:-8081}"
RUN_DIR="logs/v2"

mkdir -p "$RUN_DIR" "$(dirname "$DB_PATH")"

if [ ! -x build/ems_gateway ]; then
    echo "❌ 未找到 build/ems_gateway，请先执行 ./build.sh" >&2
    exit 1
fi
PYTHON="${PYTHON:-./.venv/bin/python}"
if [ ! -x "$PYTHON" ]; then
    echo "❌ 未找到 $PYTHON，请先创建 venv 并安装 web_backend/requirements.txt" >&2
    exit 1
fi

# 控制令牌：未指定则生成一次并落盘，重复拉起时复用
if [ -z "${EMS_WEB_CONTROL_TOKEN:-}" ]; then
    if [ -s "$RUN_DIR/token" ]; then
        EMS_WEB_CONTROL_TOKEN="$(cat "$RUN_DIR/token")"
    else
        EMS_WEB_CONTROL_TOKEN="$(openssl rand -hex 32)"
        printf '%s' "$EMS_WEB_CONTROL_TOKEN" >"$RUN_DIR/token"
        chmod 600 "$RUN_DIR/token"
    fi
fi
export EMS_WEB_CONTROL_TOKEN

./build/ems_gateway --config "$CONFIG_DIR" --db "$DB_PATH" \
    --history-interval-sec "${EMS_HISTORY_INTERVAL:-60}" \
    --history-days "${EMS_HISTORY_DAYS:-7}" \
    >"$RUN_DIR/ems.log" 2>&1 &
echo $! >"$RUN_DIR/ems.pid"
echo "🚀 已启动 EMS（$CONFIG_DIR，库 $DB_PATH），日志 $RUN_DIR/ems.log"

"$PYTHON" web_backend/server.py --config "$CONFIG_DIR" --db "$DB_PATH" \
    --bind 0.0.0.0 --port "$HTTP_PORT" --ws-bind 0.0.0.0 --ws-port "$WS_PORT" \
    --control-token "$EMS_WEB_CONTROL_TOKEN" \
    >"$RUN_DIR/web.log" 2>&1 &
echo $! >"$RUN_DIR/web.pid"
echo "🚀 已启动 Web 后端，日志 $RUN_DIR/web.log"

echo "⏳ 等待后端与实时数据就绪..."
for _ in $(seq 1 40); do
    if curl -fsS "http://127.0.0.1:$HTTP_PORT/api/live" >"$RUN_DIR/live.json" 2>/dev/null; then
        break
    fi
    sleep 0.25
done

"$PYTHON" - "$RUN_DIR/live.json" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as stream:
        data = json.load(stream)
except Exception:
    print("❌ 后端未就绪，请查看 logs/v2/web.log")
    raise SystemExit(0)

print(f"connected={data.get('connected')} control_enabled={data.get('control_enabled')} "
      f"last_error={data.get('last_error')!r}")
for point in data.get("points", []):
    print(f"  {point['id']:<22} {point['type']} value={point['value']} quality={point['quality']}")
for control in data.get("controls", []):
    print(f"  [控制] {control['id']:<22} {control['type']} "
          f"range={control['min_value']}..{control['max_value']} interlock_ok={control['interlock_ok']}")
PY

echo "页面:     http://127.0.0.1:$HTTP_PORT"
echo "WebSocket: ws://127.0.0.1:$WS_PORT"
echo "控制令牌: $EMS_WEB_CONTROL_TOKEN"
echo "停止:     ./dev-down.sh"
