#!/bin/bash
# 一键拉起本地开发链路：MQTT Broker（如未监听）→ EMS → Web 后端
# 用法: ./dev-up.sh        停止: ./dev-down.sh
set -euo pipefail
cd "$(dirname "$0")"

CONFIG_DIR="${CONFIG_DIR:-config/simulator}"
DB_PATH="${EMS_DB_FILE:-data/ems.db}"
MQTT_HOST="${EMS_MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${EMS_MQTT_PORT:-1883}"
BASE_TOPIC="${EMS_MQTT_BASE_TOPIC:-ems/site1}"
HTTP_PORT="${EMS_WEB_PORT:-8080}"
WS_PORT="${EMS_WEB_WS_PORT:-8081}"
RUN_DIR="logs/dev-up"

mkdir -p "$RUN_DIR" data

if [ ! -x build/ems_gateway ]; then
    echo "❌ 未找到 build/ems_gateway，请先执行 ./build.sh" >&2
    exit 1
fi
PYTHON="${PYTHON:-./.venv/bin/python}"
if [ ! -x "$PYTHON" ]; then
    echo "❌ 未找到 $PYTHON，请先创建 venv 并安装 web_backend/requirements.txt" >&2
    exit 1
fi

# 控制令牌：未指定则生成一次并落盘，重复拉起时复用同一个
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

# Broker：端口已在监听就复用（例如系统 mosquitto），否则用仓库配置拉起
if (exec 3<>"/dev/tcp/$MQTT_HOST/$MQTT_PORT") 2>/dev/null; then
    echo "ℹ️  $MQTT_HOST:$MQTT_PORT 已有 Broker 监听，复用现有实例"
else
    mosquitto -c config/mosquitto-ems.conf >"$RUN_DIR/broker.log" 2>&1 &
    echo $! >"$RUN_DIR/broker.pid"
    echo "🚀 已启动 Broker（config/mosquitto-ems.conf）"
    sleep 1
fi

./build/ems_gateway --config "$CONFIG_DIR" --db "$DB_PATH" \
    --mqtt-host "$MQTT_HOST" --mqtt-port "$MQTT_PORT" --mqtt-base-topic "$BASE_TOPIC" \
    --history-interval-sec "${EMS_HISTORY_INTERVAL:-60}" \
    --history-days "${EMS_HISTORY_DAYS:-7}" \
    >"$RUN_DIR/ems.log" 2>&1 &
echo $! >"$RUN_DIR/ems.pid"
echo "🚀 已启动 EMS（$CONFIG_DIR），日志 $RUN_DIR/ems.log"

"$PYTHON" web_backend/server.py --config "$CONFIG_DIR" --db "$DB_PATH" \
    --mqtt-host "$MQTT_HOST" --mqtt-port "$MQTT_PORT" --mqtt-base-topic "$BASE_TOPIC" \
    --bind 0.0.0.0 --port "$HTTP_PORT" --ws-bind 0.0.0.0 --ws-port "$WS_PORT" \
    >"$RUN_DIR/web.log" 2>&1 &
echo $! >"$RUN_DIR/web.pid"
echo "🚀 已启动 Web 后端，日志 $RUN_DIR/web.log"

echo "⏳ 等待后端与实时链路就绪..."
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
    print("❌ 后端未就绪，请查看 logs/dev-up/web.log")
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
