#!/usr/bin/env python3
"""Threaded EMS dashboard backend using a shared local SQLite database."""

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
from urllib.parse import parse_qs, quote, urlparse

from websockets.exceptions import ConnectionClosed
from websockets.sync.server import serve as websocket_serve


LOG = logging.getLogger("ems-web")
QUALITY_NAMES = {1: "正常", 0: "异常", 2: "陈旧", 3: "未初始化"}
LIVE_STALE_MS = 5000
COMMAND_TTL_MS = 15000


def db_connect(db_path, readonly=False):
    db_path = Path(db_path).resolve()
    if readonly:
        uri = "file:" + quote(str(db_path), safe="/:\\") + "?mode=ro"
        connection = sqlite3.connect(uri, uri=True, timeout=3.0)
    else:
        db_path.parent.mkdir(parents=True, exist_ok=True)
        connection = sqlite3.connect(str(db_path), timeout=3.0)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA busy_timeout=3000")
    return connection


def initialize_database(db_path):
    """Keep the backend and EMS schema identical; both access this local file."""
    with db_connect(db_path) as connection:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA synchronous=NORMAL")
        connection.executescript("""
            CREATE TABLE IF NOT EXISTS realtime (
                tag TEXT PRIMARY KEY,
                value REAL NOT NULL,
                ts INTEGER NOT NULL,
                quality INTEGER NOT NULL DEFAULT 1 CHECK(quality BETWEEN 0 AND 3)
            ) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS history (
                tag TEXT NOT NULL,
                ts INTEGER NOT NULL,
                value REAL NOT NULL,
                quality INTEGER NOT NULL DEFAULT 1 CHECK(quality BETWEEN 0 AND 3),
                PRIMARY KEY(tag, ts)
            ) WITHOUT ROWID;
            CREATE INDEX IF NOT EXISTS idx_history_tag_ts ON history(tag, ts);
            CREATE TABLE IF NOT EXISTS commands (
                command_id TEXT PRIMARY KEY,
                issued_at_ms INTEGER NOT NULL,
                expires_at_ms INTEGER NOT NULL,
                action TEXT NOT NULL,
                params TEXT NOT NULL,
                status TEXT NOT NULL DEFAULT 'pending'
                    CHECK(status IN ('pending','executing','done','failed')),
                result TEXT,
                done_ts INTEGER
            );
            CREATE INDEX IF NOT EXISTS idx_commands_status_id ON commands(status, issued_at_ms);
        """)
        columns = {row[1] for row in connection.execute("PRAGMA table_info(history)")}
        if "quality" not in columns:
            connection.execute("ALTER TABLE history ADD COLUMN quality INTEGER NOT NULL DEFAULT 1")


def load_points(config_dir):
    points = []
    point_file = Path(config_dir) / "points.conf"
    with point_file.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = csv.reader(line for line in stream if line.strip() and not line.lstrip().startswith("#"))
        for line_no, row in enumerate(rows, 1):
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
    if not any(point["type"] in ("YC", "YX") for point in points):
        raise ValueError(f"点表中没有可显示的 YC/YX 点: {point_file}")
    return points


class WebSocketHub:
    def __init__(self):
        self.lock = threading.Lock()
        self.peers = {}
        self.state = None

    def add(self, connection):
        peer = {"connection": connection, "queue": queue.Queue(maxsize=4), "closed": threading.Event()}
        with self.lock:
            self.peers[id(peer)] = peer
        return peer

    def remove(self, peer):
        peer["closed"].set()
        with self.lock:
            self.peers.pop(id(peer), None)

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
            peers = list(self.peers.values())
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
                except (ConnectionClosed, Exception):
                    peer["closed"].set()
                    return

        thread = threading.Thread(target=writer, name="ems-websocket-writer", daemon=True)
        thread.start()
        try:
            while not peer["closed"].is_set():
                try:
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
                                 ping_interval=20, ping_timeout=20, max_size=1024 * 1024) as server:
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


class AppState:
    def __init__(self, points, db_path, control_token, history_days, websocket_port):
        self.points = [p for p in points if p["type"] in ("YC", "YX")]
        self.controls = [p for p in points if p["type"] in ("YK", "YT")]
        self.point_ids = {p["id"] for p in self.points}
        self.db_path = Path(db_path).resolve()
        self.control_token = control_token
        self.history_days = history_days
        self.websocket_port = websocket_port
        self.hub = WebSocketHub()
        self.hub.state = self
        self._last_command_states = {}
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._poll_database,
                                        name="ems-sqlite-poller", daemon=True)

    def start(self):
        self._thread.start()

    def close(self):
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=3)

    def _connect(self, readonly=False):
        return db_connect(self.db_path, readonly=readonly)

    def live_payload(self):
        now_ms = int(time.time() * 1000)
        values = {}
        last_update_ms = None
        error = "等待 EMS 写入实时数据"
        try:
            with self._connect(readonly=True) as connection:
                rows = connection.execute("SELECT tag,value,ts,quality FROM realtime").fetchall()
            for row in rows:
                values[row["tag"]] = {"value": row["value"], "quality": row["quality"],
                                       "time_ms": row["ts"]}
            fresh_times = [v["time_ms"] for v in values.values()
                           if v["quality"] == 1 and v["time_ms"] is not None]
            last_update_ms = max(fresh_times) if fresh_times else None
            error = "" if last_update_ms is not None and now_ms - last_update_ms <= LIVE_STALE_MS else \
                    "EMS 实时数据已陈旧或尚未采集"
        except (sqlite3.Error, OSError) as exc:
            error = f"共享 SQLite 不可读: {exc}"

        points = []
        for point in self.points:
            item = dict(point)
            value = values.get(point["id"], {"value": None, "quality": 3, "time_ms": None})
            item.update(value)
            if item["quality"] == 1 and item["time_ms"] is not None and now_ms - item["time_ms"] > LIVE_STALE_MS:
                item["quality"] = 2
            points.append(item)
        connected = last_update_ms is not None and now_ms - last_update_ms <= LIVE_STALE_MS
        live_by_id = {p["id"]: p for p in points}
        controls = []
        for point in self.controls:
            control = dict(point)
            feedback = live_by_id.get(point["interlock_point"]) if point["interlock_point"] else None
            control["interlock_ok"] = not point["interlock_point"] or bool(
                feedback and feedback["quality"] == 1 and feedback["value"] is not None and
                abs(feedback["value"] - point["interlock_value"]) <= 1e-4
            )
            controls.append(control)
        return {"connected": connected, "last_error": error, "last_update_ms": last_update_ms,
                "database_available": self.db_path.is_file(), "control_enabled": bool(self.control_token),
                "points": points, "controls": controls}

    def command_status(self, command_id):
        try:
            with self._connect(readonly=True) as connection:
                row = connection.execute(
                    "SELECT command_id,issued_at_ms,done_ts,status,result,params "
                    "FROM commands WHERE command_id=?", (command_id,)
                ).fetchone()
            if not row:
                return None
            params = json.loads(row["params"] or "{}")
            return {"command_id": row["command_id"], "timestamp_ms": row["issued_at_ms"],
                    "done_ts": row["done_ts"], "point_id": params.get("point_id", ""),
                    "value": params.get("engineering_value"), "status": row["status"],
                    "result": row["result"] or ""}
        except (sqlite3.Error, ValueError, OSError) as exc:
            LOG.warning("读取命令状态失败: %s", exc)
            return None

    def _poll_database(self):
        while not self._stop.wait(1.0):
            payload = self.live_payload()
            self.hub.broadcast({"type": "live", "data": payload})
            if not self.db_path.is_file():
                continue
            try:
                with self._connect(readonly=True) as connection:
                    rows = connection.execute(
                        "SELECT command_id,issued_at_ms,done_ts,status,result,params FROM commands "
                        "WHERE status IN ('done','failed') AND done_ts >= ? ORDER BY done_ts DESC LIMIT 100",
                        (int(time.time() * 1000) - 60000,),
                    ).fetchall()
                for row in reversed(rows):
                    command_id = row["command_id"]
                    current = (row["status"], row["done_ts"])
                    if self._last_command_states.get(command_id) == current:
                        continue
                    self._last_command_states[command_id] = current
                    params = json.loads(row["params"] or "{}")
                    self.hub.broadcast({"type": "command_result", "data": {
                        "command_id": command_id, "timestamp_ms": row["issued_at_ms"],
                        "done_ts": row["done_ts"], "point_id": params.get("point_id", ""),
                        "value": params.get("engineering_value"), "status": row["status"],
                        "result": row["result"] or ""}})
            except (sqlite3.Error, ValueError, OSError) as exc:
                LOG.warning("轮询 SQLite 命令结果失败: %s", exc)

    def enqueue_command(self, point, engineering_value, params):
        now_ms = int(time.time() * 1000)
        command_id = uuid.uuid4().hex
        stored_params = dict(params)
        stored_params["point_id"] = point["id"]
        stored_params["engineering_value"] = engineering_value
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "INSERT INTO commands(command_id,issued_at_ms,expires_at_ms,action,params,status) "
                "VALUES(?,?,?,?,?,'pending')",
                (command_id, now_ms, now_ms + COMMAND_TTL_MS, "device.write",
                 json.dumps(stored_params, ensure_ascii=False, separators=(",", ":"))),
            )
        return command_id, now_ms


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
            return bool(state.control_token) and hmac.compare_digest(
                self.headers.get("Authorization", ""), "Bearer " + state.control_token)

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path == "/api/live":
                self._json(200, state.live_payload())
            elif parsed.path == "/api/config":
                self._json(200, {"websocket_port": state.websocket_port})
            elif parsed.path == "/api/history":
                self._history(parse_qs(parsed.query))
            elif parsed.path == "/api/control/status":
                if not self._authorized():
                    self._json(401, {"error": "控制令牌无效"})
                else:
                    self._json(200, {"authorized": True})
            elif parsed.path.startswith("/api/commands/"):
                if not self._authorized():
                    self._json(401, {"error": "控制令牌无效"})
                    return
                command = state.command_status(parsed.path.rsplit("/", 1)[-1])
                self._json(200 if command else 404,
                           command if command else {"error": "命令不存在"})
            elif parsed.path in ("/", "/index.html"):
                self._file(web_root / "index.html", "text/html; charset=utf-8")
            else:
                self._json(404, {"error": "接口不存在"})

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
                length = int(self.headers.get("Content-Length", "0"))
                if length < 1 or length > 4096:
                    raise ValueError("请求长度无效")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict) or isinstance(body.get("value"), bool):
                    raise ValueError("请求内容必须包含点名和数值 value")
                point_id = str(body["point_id"])
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
            live = state.live_payload()
            if not live["connected"]:
                self._json(409, {"error": "EMS 实时数据已陈旧，未入队控制命令"})
                return
            if value < point["min_value"] or value > point["max_value"]:
                self._json(400, {"error": f"控制值超出点表范围 {point['min_value']}..{point['max_value']}"})
                return

            live_by_id = {p["id"]: p for p in live["points"]}
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
                params.update({"functionCode": 5, "value": int(value)})
            else:
                if point["count"] != 1 or point["scale"] == 0:
                    self._json(400, {"error": "该 YT 点暂不支持单寄存器缩放控制"})
                    return
                raw = int(round((value - point["offset"]) / point["scale"]))
                if point["data_type"] == "S16":
                    if not -32768 <= raw <= 32767:
                        self._json(400, {"error": "转换后的有符号寄存器值超出范围"})
                        return
                elif not 0 <= raw <= 65535:
                    self._json(400, {"error": "转换后的无符号寄存器值超出范围"})
                    return
                params.update({"functionCode": 6, "value": raw & 0xFFFF})
            if not point["device"]:
                self._json(400, {"error": "控制点没有配置所属设备"})
                return
            try:
                command_id, now_ms = state.enqueue_command(point, value, params)
            except sqlite3.Error as exc:
                LOG.exception("控制命令写入 SQLite 失败")
                self._json(503, {"error": f"控制命令未入队: {exc}"})
                return
            self._json(202, {"queued": True, "command_id": command_id, "point_id": point_id,
                             "engineering_value": value, "status": "pending",
                             "message": "控制命令已写入共享 SQLite 队列，等待 EMS 取出执行"})

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
            cutoff = int((time.time() - hours * 3600) * 1000)
            resolution_ms = max(1000, int((hours * 3600 * 1000 + limit - 1) // limit))
            try:
                with state._connect(readonly=True) as connection:
                    rows = connection.execute(
                        "SELECT bucket_ms,AVG(value),MIN(quality) FROM ("
                        "SELECT (ts / ?) * ? AS bucket_ms,value,quality FROM history "
                        "WHERE tag=? AND ts>=? AND quality=1) "
                        "GROUP BY bucket_ms ORDER BY bucket_ms ASC LIMIT ?",
                        (resolution_ms, resolution_ms, point_id, cutoff, limit),
                    ).fetchall()
                self._json(200, {"point_id": point_id, "resolution_ms": resolution_ms,
                                 "rows": [{"time_ms": row[0], "value": row[1], "quality": row[2],
                                           "quality_name": QUALITY_NAMES.get(row[2], "未知")} for row in rows]})
            except (sqlite3.Error, OSError) as exc:
                self._json(503, {"error": f"历史数据库查询失败: {exc}"})

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
    parser = argparse.ArgumentParser(description="EMS web backend using shared local SQLite")
    parser.add_argument("--config", default="config/simulator", help="EMS 点表目录")
    parser.add_argument("--db", default=os.environ.get("EMS_DB_FILE", "data/ems.db"),
                        help="EMS 与后端共用的 SQLite 文件路径")
    parser.add_argument("--bind", default="0.0.0.0", help="HTTP 页面/API 监听地址")
    parser.add_argument("--port", type=int, default=8080, help="HTTP 页面/API 端口")
    parser.add_argument("--ws-bind", default=None, help="WebSocket 监听地址，默认与 --bind 相同")
    parser.add_argument("--ws-port", type=int, default=8081, help="UI 实时推送端口")
    parser.add_argument("--history-days", type=int, default=7, help="历史查询保留上限天数")
    parser.add_argument("--control-token", default=os.environ.get("EMS_WEB_CONTROL_TOKEN", ""),
                        help="控制接口 Bearer 令牌；也可通过 EMS_WEB_CONTROL_TOKEN 设置")
    args = parser.parse_args()
    if not (1 <= args.port <= 65535 and 1 <= args.ws_port <= 65535 and
            1 <= args.history_days <= 365):
        parser.error("HTTP/WebSocket 端口或历史保留天数超出范围")

    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s %(message)s")
    points = load_points(args.config)
    db_path = Path(args.db).resolve()
    initialize_database(db_path)
    web_root = Path(__file__).resolve().parent / "www"
    state = AppState(points, db_path, args.control_token, args.history_days, args.ws_port)
    state.start()
    ws_service = WebSocketService(state.hub, args.ws_bind or args.bind, args.ws_port)
    if not ws_service.start():
        state.close()
        raise RuntimeError("WebSocket 服务无法启动")
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(state, web_root))
    server.daemon_threads = True
    LOG.info("HTTP 页面/API 监听 http://%s:%d", args.bind, args.port)
    LOG.info("EMS 数据通道：共享 SQLite %s（无 EMS Modbus TCP/HTTP 接口连接）", db_path)
    LOG.info("UI 实时推送：WebSocket :%d", args.ws_port)
    LOG.info("网页控制接口：%s", "已启用（令牌保护）" if args.control_token else "已关闭（未配置令牌）")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()
        ws_service.close()
        state.close()


if __name__ == "__main__":
    main()
