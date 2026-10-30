#!/usr/bin/env python3
"""EMS web backend: MQTT for EMS messaging, WebSocket for UI live updates."""

import argparse
import csv
import hmac
import json
import logging
import math
import os
import queue
import sqlite3
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

import paho.mqtt.client as mqtt
from websockets.exceptions import ConnectionClosed
from websockets.sync.server import serve as websocket_serve


LOG = logging.getLogger("ems-web")
QUALITY_NAMES = {1: "正常", 0: "异常", 2: "陈旧", 3: "未初始化"}
QUALITY_CODES = {"good": 1, "bad": 0, "stale": 2, "uninitialized": 3}
MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".map": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".ico": "image/x-icon",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".ttf": "font/ttf",
    ".otf": "font/otf",
}


def initialize_database(db_path):
    """Create the small trend/audit schema. Realtime data never comes from SQL."""
    db_path = Path(db_path)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(str(db_path), timeout=3.0) as connection:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=NORMAL")
        connection.executescript("""
            CREATE TABLE IF NOT EXISTS history (
                tag TEXT NOT NULL, ts INTEGER NOT NULL, value REAL NOT NULL,
                PRIMARY KEY(tag, ts)
            ) WITHOUT ROWID;
            CREATE INDEX IF NOT EXISTS idx_history_tag_ts ON history(tag, ts);
            CREATE TABLE IF NOT EXISTS command_history (
                command_id TEXT PRIMARY KEY, ts INTEGER NOT NULL, done_ts INTEGER,
                point_id TEXT NOT NULL, value REAL NOT NULL,
                status TEXT NOT NULL, result TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_command_history_ts ON command_history(ts);
        """)


def load_points(config_dir):
    points = []
    point_file = Path(config_dir) / "points.conf"
    with point_file.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.reader(line for line in stream if line.strip() and not line.lstrip().startswith("#")):
            if len(row) < 11:
                continue
            kind = row[0].strip().upper()
            if kind not in ("YC", "YX", "YK", "YT"):
                continue
            device = row[9].strip()
            point_id = row[1].strip()
            points.append({
                "id": f"{device}.{point_id}" if device else point_id,
                "name": point_id,
                "device": device,
                "label": row[2].strip() or point_id,
                "type": kind,
                "address": int(row[8]),
                "south_address": int(row[3]),
                "count": int(row[4]),
                "scale": float(row[5]),
                "offset": float(row[6]),
                "unit": row[7].strip(),
                "data_type": row[10].strip().upper(),
                "min_value": float(row[13]) if len(row) > 13 and row[13].strip() else 0.0,
                "max_value": float(row[14]) if len(row) > 14 and row[14].strip() else 65535.0,
                "interlock_point": row[15].strip() if len(row) > 15 else "",
                "interlock_value": float(row[16]) if len(row) > 16 and row[16].strip() else 1.0,
            })
    if not any(p["type"] in ("YC", "YX") for p in points):
        raise ValueError(f"点表中没有可显示的 YC/YX 点: {point_file}")
    return points


class WebSocketHub:
    """Fan out each latest-value update without letting a slow browser block MQTT."""

    def __init__(self):
        self.lock = threading.Lock()
        self._peers = {}
        self.state = None

    def add(self, connection):
        peer = {"connection": connection, "queue": queue.Queue(maxsize=4), "closed": threading.Event()}
        with self.lock:
            self._peers[id(peer)] = peer
        return peer

    def remove(self, peer):
        peer["closed"].set()
        with self.lock:
            self._peers.pop(id(peer), None)

    @staticmethod
    def send(peer, payload):
        if peer["closed"].is_set():
            return
        try:
            peer["queue"].put_nowait(payload)
        except queue.Full:
            try:
                peer["queue"].get_nowait()
            except queue.Empty:
                pass
            try:
                peer["queue"].put_nowait(payload)
            except queue.Full:
                pass

    def broadcast(self, message):
        payload = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        with self.lock:
            peers = list(self._peers.values())
        for peer in peers:
            self.send(peer, payload)

    def handler(self, connection):
        peer = self.add(connection)
        self.send(peer, json.dumps({"type": "live", "data": self.state.live_payload()},
                                   ensure_ascii=False, separators=(",", ":")))

        def writer():
            while not peer["closed"].is_set():
                try:
                    connection.send(peer["queue"].get(timeout=0.5))
                except queue.Empty:
                    continue
                except ConnectionClosed:
                    peer["closed"].set()
                    return
                except Exception:
                    peer["closed"].set()
                    return

        thread = threading.Thread(target=writer, name="ems-websocket-writer", daemon=True)
        thread.start()
        try:
            while not peer["closed"].is_set():
                try:
                    # The UI currently sends no WebSocket payloads. recv keeps close/ping
                    # handling active and detects dropped browser connections.
                    connection.recv(timeout=1)
                except TimeoutError:
                    continue
                except ConnectionClosed:
                    break
        finally:
            self.remove(peer)
            thread.join(timeout=1)


class WebSocketService:
    def __init__(self, hub, host, port):
        self.hub = hub
        self.host = host
        self.port = port
        self.server = None
        self.thread = threading.Thread(target=self._run, name="ems-websocket-server", daemon=True)

    def _run(self):
        try:
            with websocket_serve(self.hub.handler, self.host, self.port,
                                 ping_interval=20, ping_timeout=20,
                                 max_size=1024 * 1024) as server:
                self.server = server
                LOG.info("WebSocket 实时推送监听 ws://%s:%d", self.host, self.port)
                server.serve_forever()
        except Exception:
            LOG.exception("WebSocket 服务启动或运行失败")

    def start(self):
        self.thread.start()
        for _ in range(50):
            if self.server is not None or not self.thread.is_alive():
                return self.server is not None
            time.sleep(0.05)
        return self.server is not None

    def close(self):
        if self.server is not None:
            self.server.shutdown()
        if self.thread.is_alive():
            self.thread.join(timeout=3)


class LiveState:
    def __init__(self, points):
        self.points = [p for p in points if p["type"] in ("YC", "YX")]
        self.lock = threading.Lock()
        self.values = {p["id"]: {"value": None, "quality": 3, "time_ms": None} for p in self.points}
        self.mqtt_connected = False
        self.ems_online = False
        self.last_update_ms = None
        self.last_error = "等待 EMS MQTT 遥测"

    def set_mqtt_connected(self, connected):
        with self.lock:
            self.mqtt_connected = connected
            if not connected:
                self.last_error = "MQTT Broker 连接断开"

    def set_ems_status(self, payload):
        online = bool(payload.get("online"))
        with self.lock:
            self.ems_online = online
            if not online:
                self.last_error = "EMS 未在线"

    def update_telemetry(self, payload):
        if not isinstance(payload, dict) or payload.get("type") != "telemetry":
            return False
        items = payload.get("points")
        if not isinstance(items, list):
            return False
        timestamp_ms = payload.get("timestamp_ms")
        if isinstance(timestamp_ms, bool) or not isinstance(timestamp_ms, (int, float)):
            return False
        now_ms = int(time.time() * 1000)
        fresh = {}
        known = {p["id"] for p in self.points}
        for item in items:
            if not isinstance(item, dict) or item.get("id") not in known:
                continue
            value = item.get("value")
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                value = None
            quality_name = str(item.get("quality", "bad")).lower()
            quality = QUALITY_CODES.get(quality_name, 0)
            point_ts = item.get("timestamp_ms", timestamp_ms)
            if isinstance(point_ts, bool) or not isinstance(point_ts, (int, float)):
                point_ts = timestamp_ms
            fresh[item["id"]] = {"value": value, "quality": quality, "time_ms": int(point_ts)}
        if not fresh:
            return False
        with self.lock:
            self.values.update(fresh)
            self.mqtt_connected = True
            self.ems_online = True
            self.last_update_ms = int(timestamp_ms)
            self.last_error = "" if now_ms - int(timestamp_ms) <= 5000 else "EMS 遥测已陈旧"
        return True

    def snapshot(self):
        now_ms = int(time.time() * 1000)
        with self.lock:
            points = []
            for point in self.points:
                value = dict(point)
                value.update(self.values[point["id"]])
                ts = value.get("time_ms")
                if value["quality"] == 1 and ts is not None and now_ms - ts > 5000:
                    value["quality"] = 2
                points.append(value)
            fresh = self.last_update_ms is not None and now_ms - self.last_update_ms <= 5000
            connected = self.mqtt_connected and self.ems_online and fresh
            error = self.last_error
            if not self.mqtt_connected:
                error = "MQTT Broker 连接断开"
            elif not self.ems_online:
                error = "EMS 未在线"
            elif not fresh:
                error = "等待新鲜的 EMS 遥测"
            return {
                "connected": connected,
                "last_error": error,
                "last_update_ms": self.last_update_ms,
                "points": points,
            }


class AppState:
    def __init__(self, points, db_path, control_token, mqtt_base_topic,
                 history_days, websocket_port):
        self.points = [p for p in points if p["type"] in ("YC", "YX")]
        self.controls = [p for p in points if p["type"] in ("YK", "YT")]
        self.point_ids = {p["id"] for p in self.points}
        self.db_path = Path(db_path).resolve()
        self.history_days = history_days
        self.websocket_port = websocket_port
        self.control_token = control_token
        self.mqtt_base_topic = mqtt_base_topic.strip("/") or "ems/site1"
        self.telemetry_topic = self.mqtt_base_topic + "/telemetry"
        self.command_topic = self.mqtt_base_topic + "/command"
        self.result_topic = self.mqtt_base_topic + "/command/result"
        self.status_topic = self.mqtt_base_topic + "/status"
        self.live = LiveState(points)
        self.hub = WebSocketHub()
        self.hub.state = self
        self.mqtt = None
        self.commands_lock = threading.Lock()
        self.commands = {}

    def live_payload(self):
        payload = self.live.snapshot()
        live_by_id = {point["id"]: point for point in payload["points"]}
        payload["database_available"] = self.db_path.is_file()
        payload["control_enabled"] = bool(self.control_token)
        payload["controls"] = []
        for point in self.controls:
            control = dict(point)
            if point["interlock_point"]:
                feedback = live_by_id.get(point["interlock_point"])
                control["interlock_ok"] = bool(
                    feedback and feedback["quality"] == 1 and feedback["value"] is not None and
                    abs(feedback["value"] - point["interlock_value"]) <= 1e-4
                )
            else:
                control["interlock_ok"] = True
            payload["controls"].append(control)
        return payload

    def remember_command(self, command):
        with self.commands_lock:
            self.commands[command["command_id"]] = command
            if len(self.commands) > 1000:
                oldest = next(iter(self.commands))
                self.commands.pop(oldest, None)

    def mark_command_sent(self, command_id):
        with self.commands_lock:
            command = self.commands.get(command_id)
            if command and command.get("status") not in ("done", "failed"):
                command["status"] = "sent"
                command["result"] = "Broker 已确认消息，等待 EMS 执行结果"
            return dict(command) if command else None

    def mark_command_failed(self, command_id, result):
        with self.commands_lock:
            command = self.commands.get(command_id)
            if command and command.get("status") not in ("done", "failed"):
                command["status"] = "failed"
                command["done_ts"] = int(time.time() * 1000)
                command["result"] = result
            return dict(command) if command else None

    def mark_command_delivery_unknown(self, command_id, result):
        with self.commands_lock:
            command = self.commands.get(command_id)
            if command and command.get("status") not in ("done", "failed"):
                command["status"] = "delivery_unknown"
                command["result"] = result
            return dict(command) if command else None

    def command_status(self, command_id):
        with self.commands_lock:
            item = self.commands.get(command_id)
        if item:
            return dict(item)
        try:
            uri = "file:" + quote(str(self.db_path), safe="/:\\") + "?mode=ro"
            with sqlite3.connect(uri, uri=True, timeout=2.0) as connection:
                row = connection.execute(
                    "SELECT command_id,ts,done_ts,point_id,value,status,result "
                    "FROM command_history WHERE command_id=?", (command_id,)
                ).fetchone()
            if row:
                return {"command_id": row[0], "timestamp_ms": row[1], "done_ts": row[2],
                        "point_id": row[3], "value": row[4], "status": row[5], "result": row[6]}
        except sqlite3.Error as exc:
            LOG.warning("读取命令历史失败: %s", exc)
        return None

    def save_command_history(self, command):
        try:
            with sqlite3.connect(str(self.db_path), timeout=3.0) as connection:
                connection.execute(
                    "INSERT INTO command_history(command_id,ts,done_ts,point_id,value,status,result) "
                    "VALUES(?,?,?,?,?,?,?) ON CONFLICT(command_id) DO UPDATE SET "
                    "ts=CASE WHEN excluded.point_id<>'' THEN excluded.ts ELSE command_history.ts END, "
                    "done_ts=excluded.done_ts, "
                    "point_id=CASE WHEN excluded.point_id<>'' THEN excluded.point_id ELSE command_history.point_id END, "
                    "value=CASE WHEN excluded.point_id<>'' THEN excluded.value ELSE command_history.value END, "
                    "status=excluded.status,result=excluded.result",
                    (command["command_id"], command["timestamp_ms"], command.get("done_ts"),
                     command["point_id"], command["value"], command["status"], command.get("result")),
                )
                cutoff = int(time.time() * 1000) - 30 * 24 * 60 * 60 * 1000
                connection.execute("DELETE FROM command_history WHERE ts < ?", (cutoff,))
        except sqlite3.Error as exc:
            # History/audit storage must not block real-time control delivery.
            LOG.warning("命令审计写入 SQLite 失败（命令已走 MQTT）: %s", exc)

    def accept_command_result(self, payload):
        if not isinstance(payload, dict) or payload.get("type") != "command_result":
            return
        command_id = str(payload.get("command_id", ""))
        if not command_id:
            return
        with self.commands_lock:
            command = self.commands.get(command_id)
            if command is None:
                command = {"command_id": command_id, "timestamp_ms": int(time.time() * 1000),
                           "point_id": "", "value": 0.0}
            command["status"] = str(payload.get("status", "unknown"))
            command["result"] = str(payload.get("result", ""))
            command["done_ts"] = (int(payload.get("timestamp_ms", time.time() * 1000))
                                  if command["status"] in ("done", "failed") else None)
            self.commands[command_id] = command
        self.save_command_history(command)
        self.hub.broadcast({"type": "command_result", "data": command})


class MqttService:
    def __init__(self, state, host, port, username="", password=""):
        self.state = state
        self.host = host
        self.port = port
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="ems-web-backend",
            clean_session=False,
            protocol=mqtt.MQTTv311,
        )
        if username:
            self.client.username_pw_set(username, password)
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.will_set(state.mqtt_base_topic + "/backend/status",
                             '{"online":false}', qos=1, retain=True)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message

    def start(self):
        self.client.connect_async(self.host, self.port, keepalive=30)
        self.client.loop_start()

    def close(self):
        try:
            info = self.client.publish(self.state.mqtt_base_topic + "/backend/status",
                                       '{"online":false}', qos=1, retain=True)
            if info.rc == mqtt.MQTT_ERR_SUCCESS:
                try:
                    info.wait_for_publish(timeout=1.0)
                except RuntimeError:
                    LOG.warning("Web 后端离线状态未能在关闭前确认")
        except Exception:
            LOG.exception("关闭 MQTT 客户端失败")
        finally:
            try:
                self.client.disconnect()
                self.client.loop_stop()
            except Exception:
                LOG.exception("停止 MQTT 网络线程失败")

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        code = getattr(reason_code, "value", reason_code)
        if int(code) != 0:
            LOG.error("MQTT Broker 连接失败，原因码=%s", code)
            self.state.live.set_mqtt_connected(False)
            self.state.hub.broadcast({"type": "live", "data": self.state.live_payload()})
            return
        client.subscribe([
            (self.state.telemetry_topic, 0),
            (self.state.result_topic, 1),
            (self.state.status_topic, 1),
        ])
        client.publish(self.state.mqtt_base_topic + "/backend/status",
                       '{"online":true}', qos=1, retain=True)
        self.state.live.set_mqtt_connected(True)
        LOG.info("MQTT Broker 已连接 %s:%d，订阅 %s", self.host, self.port,
                 self.state.telemetry_topic)
        self.state.hub.broadcast({"type": "live", "data": self.state.live_payload()})

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties):
        self.state.live.set_mqtt_connected(False)
        self.state.hub.broadcast({"type": "live", "data": self.state.live_payload()})
        LOG.warning("MQTT Broker 连接断开，客户端自动重连中，原因码=%s", reason_code)

    def _on_message(self, client, userdata, message):
        try:
            payload = json.loads(message.payload.decode("utf-8"))
            if message.topic == self.state.telemetry_topic:
                if self.state.live.update_telemetry(payload):
                    self.state.hub.broadcast({"type": "live", "data": self.state.live_payload()})
            elif message.topic == self.state.status_topic:
                self.state.live.set_ems_status(payload)
                self.state.hub.broadcast({"type": "live", "data": self.state.live_payload()})
            elif message.topic == self.state.result_topic:
                self.state.accept_command_result(payload)
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
            LOG.warning("忽略格式错误的 MQTT 消息，topic=%s", message.topic)

    def publish_command(self, payload):
        if not self.client.is_connected():
            raise RuntimeError("MQTT Broker 未连接")
        info = self.client.publish(self.state.command_topic,
                                   json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
                                   qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(mqtt.error_string(info.rc))
        info.wait_for_publish(timeout=2.0)
        if not info.is_published():
            raise TimeoutError("Broker 未在 2 秒内确认命令消息")


def make_handler(state, web_root):
    class Handler(BaseHTTPRequestHandler):
        server_version = "EMS-Web/2.0"

        def _json(self, status, payload):
            data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self):
            if not state.control_token:
                return False
            return hmac.compare_digest(self.headers.get("Authorization", ""),
                                       "Bearer " + state.control_token)

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path == "/api/live":
                self._json(200, state.live_payload())
                return
            if parsed.path == "/api/config":
                self._json(200, {"websocket_port": state.websocket_port})
                return
            if parsed.path == "/api/history":
                self._history(parse_qs(parsed.query))
                return
            if parsed.path == "/api/control/status":
                if not self._authorized():
                    self._json(401, {"error": "控制令牌无效"})
                    return
                self._json(200, {"authorized": True})
                return
            if parsed.path.startswith("/api/commands/"):
                if not self._authorized():
                    self._json(401, {"error": "控制令牌无效"})
                    return
                command_id = parsed.path.rsplit("/", 1)[-1]
                result = state.command_status(command_id)
                self._json(200 if result else 404,
                           result if result else {"error": "命令历史不存在"})
                return
            self._static(parsed.path)
            return

        def do_POST(self):
            if urlparse(self.path).path != "/api/control":
                self._json(404, {"error": "接口不存在"})
                return
            if not state.control_token:
                self._json(503, {"error": "后端未配置控制令牌，控制接口已关闭"})
                return
            if not self._authorized():
                self._json(401, {"error": "控制令牌无效"})
                return
            try:
                content_length = int(self.headers.get("Content-Length", "0"))
                if content_length < 1 or content_length > 4096:
                    raise ValueError("请求长度无效")
                body = json.loads(self.rfile.read(content_length))
                if not isinstance(body, dict):
                    raise ValueError("请求内容必须是 JSON 对象")
                point_id = str(body.get("point_id", ""))
                if isinstance(body.get("value"), bool):
                    raise ValueError("控制值必须是数值")
                value = float(body["value"])
                if not math.isfinite(value):
                    raise ValueError("控制值必须是有限数值")
            except (ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
                self._json(400, {"error": f"控制请求格式错误: {exc}"})
                return

            point = next((p for p in state.controls if p["id"] == point_id), None)
            if point is None:
                self._json(404, {"error": "控制点未配置"})
                return
            if not state.live.snapshot()["connected"]:
                self._json(409, {"error": "EMS 实时链路未连接，未发送控制命令"})
                return
            if value < point["min_value"] or value > point["max_value"]:
                self._json(400, {"error": f"控制值超出点表范围 {point['min_value']}..{point['max_value']}"})
                return

            live_by_id = {p["id"]: p for p in state.live.snapshot()["points"]}
            if point["interlock_point"]:
                feedback = live_by_id.get(point["interlock_point"])
                if (not feedback or feedback["quality"] != 1 or feedback["value"] is None or
                        abs(feedback["value"] - point["interlock_value"]) > 1e-4):
                    self._json(409, {"error": f"联锁未满足或反馈无效: {point['interlock_point']}"})
                    return

            params = {"deviceId": point["device"], "address": point["south_address"]}
            if point["type"] == "YK":
                if value not in (0.0, 1.0):
                    self._json(400, {"error": "遥控值只能是 0 或 1"})
                    return
                function_code = 5
                params["functionCode"] = function_code
                params["value"] = int(value)
            else:
                if point["count"] != 1:
                    self._json(400, {"error": "此控制卡只支持单寄存器点位，多寄存器控制需要数组控件"})
                    return
                if point["scale"] == 0:
                    self._json(400, {"error": "该 YT 点 scale 为 0，无法转换"})
                    return
                signed_raw = int(round((value - point["offset"]) / point["scale"]))
                if point["data_type"] == "S16":
                    if signed_raw < -32768 or signed_raw > 32767:
                        self._json(400, {"error": "转换后的有符号寄存器值超出范围"})
                        return
                elif signed_raw < 0 or signed_raw > 65535:
                    self._json(400, {"error": "转换后的无符号寄存器值超出范围"})
                    return
                function_code = 6
                params["functionCode"] = function_code
                params["value"] = signed_raw & 0xFFFF

            if not point["device"]:
                self._json(400, {"error": "控制点没有配置所属设备"})
                return
            now_ms = int(time.time() * 1000)
            command_id = uuid.uuid4().hex
            command = {
                "schema_version": 1,
                "type": "command",
                "command_id": command_id,
                "action": "device.write",
                "params": params,
                "issued_at_ms": now_ms,
                "expires_at_ms": now_ms + 15000,
            }
            command_state = {"command_id": command_id, "timestamp_ms": now_ms,
                             "point_id": point_id, "value": value,
                             "status": "sending", "result": "等待 MQTT Broker 确认"}
            state.remember_command(command_state)
            try:
                state.mqtt.publish_command(command)
            except TimeoutError as exc:
                command_state = state.mark_command_delivery_unknown(command_id, str(exc)) or command_state
                state.save_command_history(command_state)
                self._json(202, {"queued": True, "command_id": command_id, "point_id": point_id,
                                 "engineering_value": value, "status": "delivery_unknown",
                                 "message": "Broker 确认超时，命令是否送达未知；请等待 EMS 状态回报，不要立即重复下发"})
                return
            except RuntimeError as exc:
                command_state = state.mark_command_failed(command_id, str(exc)) or command_state
                state.save_command_history(command_state)
                self._json(503, {"error": f"MQTT 命令发送失败: {exc}"})
                return
            command_state = state.mark_command_sent(command_id) or command_state
            state.save_command_history(command_state)
            self._json(202, {"queued": True, "command_id": command_id, "point_id": point_id,
                             "engineering_value": value, "status": "sent",
                             "message": "MQTT Broker 已确认，等待 EMS 执行结果"})

        def _history(self, query):
            point_id = query.get("point", [""])[0]
            if point_id not in state.point_ids:
                self._json(400, {"error": "点名无效"})
                return
            try:
                hours = min(max(float(query.get("hours", ["1"])[0]), 0.01), state.history_days * 24)
                limit = min(max(int(query.get("limit", ["3600"])[0]), 1), 20000)
            except ValueError:
                self._json(400, {"error": "hours 或 limit 参数无效"})
                return
            if not state.db_path.is_file():
                self._json(503, {"error": "历史数据库尚未创建"})
                return
            cutoff = int((time.time() - hours * 3600) * 1000)
            resolution_ms = max(1000, int((hours * 3600 * 1000 + limit - 1) // limit))
            uri = "file:" + quote(str(state.db_path), safe="/:\\") + "?mode=ro"
            try:
                with sqlite3.connect(uri, uri=True, timeout=2.0) as connection:
                    rows = connection.execute(
                        "SELECT bucket_ms, AVG(value), 1 FROM ("
                        "SELECT (ts / ?) * ? AS bucket_ms, value FROM history "
                        "WHERE tag = ? AND ts >= ?) GROUP BY bucket_ms ORDER BY bucket_ms ASC LIMIT ?",
                        (resolution_ms, resolution_ms, point_id, cutoff, limit),
                    ).fetchall()
                self._json(200, {
                    "point_id": point_id,
                    "resolution_ms": resolution_ms,
                    "rows": [{"time_ms": row[0], "value": row[1], "quality": row[2],
                              "quality_name": QUALITY_NAMES.get(row[2], "未知")} for row in rows],
                })
            except sqlite3.Error as exc:
                self._json(503, {"error": f"历史数据库查询失败: {exc}"})

        def _static(self, raw_path):
            """Serve the built frontend from www/, with SPA fallback to index.html."""
            relative = unquote(raw_path).lstrip("/")
            target = (web_root / relative).resolve()
            try:
                target.relative_to(web_root)
            except ValueError:
                # Path traversal outside www/ is never served.
                self._json(404, {"error": "接口不存在"})
                return
            if target.is_dir():
                target = target / "index.html"
            if target.is_file():
                self._file(target, MIME_TYPES.get(target.suffix.lower(),
                                                  "application/octet-stream"))
                return
            index = web_root / "index.html"
            # Deep links such as /dashboard fall back to the SPA entry point;
            # missing files with an extension keep returning 404.
            if index.is_file() and "." not in Path(relative).name:
                self._file(index, "text/html; charset=utf-8")
                return
            self._json(404, {"error": "接口不存在"})

        def _file(self, path, content_type):
            try:
                data = path.read_bytes()
            except OSError:
                self._json(404, {"error": "前端页面文件不存在"})
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            LOG.info("%s - %s", self.address_string(), fmt % args)

    return Handler


def main():
    parser = argparse.ArgumentParser(description="EMS web backend")
    parser.add_argument("--config", default="config/simulator", help="EMS 点表目录")
    parser.add_argument("--db", default=os.environ.get("EMS_DB_FILE", "data/ems.db"),
                        help="SQLite 历史数据库；EMS 使用同一文件写入稀疏采样")
    parser.add_argument("--bind", default="0.0.0.0", help="HTTP 页面/API 监听地址")
    parser.add_argument("--port", type=int, default=8080, help="HTTP 页面/API 端口")
    parser.add_argument("--ws-bind", default=None, help="WebSocket 监听地址，默认与 --bind 相同")
    parser.add_argument("--ws-port", type=int, default=8081, help="WebSocket 实时推送端口")
    parser.add_argument("--mqtt-host", default=os.environ.get("EMS_MQTT_HOST", "127.0.0.1"))
    parser.add_argument("--mqtt-port", type=int, default=int(os.environ.get("EMS_MQTT_PORT", "1883")))
    parser.add_argument("--mqtt-base-topic", default=os.environ.get("EMS_MQTT_BASE_TOPIC", "ems/site1"))
    parser.add_argument("--mqtt-username", default=os.environ.get("EMS_MQTT_USERNAME", ""))
    parser.add_argument("--mqtt-password", default=os.environ.get("EMS_MQTT_PASSWORD", ""))
    parser.add_argument("--history-days", type=int, default=7, help="历史查询保留上限天数")
    parser.add_argument("--control-token", default=os.environ.get("EMS_WEB_CONTROL_TOKEN", ""),
                        help="控制接口 Bearer 令牌；也可通过 EMS_WEB_CONTROL_TOKEN 设置")
    args = parser.parse_args()
    args.mqtt_base_topic = args.mqtt_base_topic.strip("/") or "ems/site1"
    if not (1 <= args.port <= 65535 and 1 <= args.ws_port <= 65535 and
            1 <= args.mqtt_port <= 65535 and 1 <= args.history_days <= 365):
        parser.error("HTTP/MQTT/WebSocket 端口或历史保留天数超出范围")
    if "+" in args.mqtt_base_topic or "#" in args.mqtt_base_topic:
        parser.error("MQTT 根 Topic 不能包含通配符 + 或 #")

    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s %(message)s")
    points = load_points(args.config)
    db_path = Path(args.db).resolve()
    initialize_database(db_path)
    web_root = Path(__file__).resolve().parent / "www"
    state = AppState(points, db_path, args.control_token, args.mqtt_base_topic,
                     args.history_days, args.ws_port)
    state.mqtt = MqttService(state, args.mqtt_host, args.mqtt_port,
                             args.mqtt_username, args.mqtt_password)
    state.mqtt.start()
    ws_service = WebSocketService(state.hub, args.ws_bind or args.bind, args.ws_port)
    if not ws_service.start():
        state.mqtt.close()
        raise RuntimeError("WebSocket 服务无法启动")
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(state, web_root))
    server.daemon_threads = True
    LOG.info("HTTP 页面/API 监听 http://%s:%d", args.bind, args.port)
    LOG.info("MQTT EMS 通道: %s:%d topic=%s", args.mqtt_host, args.mqtt_port,
             state.mqtt_base_topic)
    LOG.info("SQLite 仅用于稀疏历史趋势与命令审计: %s", db_path)
    LOG.info("网页控制接口: %s", "已启用（令牌保护）" if args.control_token else "已关闭（未配置令牌）")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
        ws_service.close()
        state.mqtt.close()


if __name__ == "__main__":
    main()