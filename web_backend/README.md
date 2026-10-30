# EMS Web Dashboard

UI 经 HTTP/JSON 调用后端控制接口，通过 WebSocket 接收实时遥测和命令执行状态。后端与 EMS 之间通过 MQTT Broker 通信，不使用 SQL 传实时值或下发命令。

## 依赖与启动

项目根目录安装 Python 依赖并启动服务：

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r web_backend/requirements.txt
```

确保本机 Mosquitto Broker 正在运行，EMS 与后端使用相同 Broker、Topic 根路径和 SQLite 文件：

```bash
export EMS_WEB_CONTROL_TOKEN="$(openssl rand -hex 32)"
python web_backend/server.py --config config/simulator --db data/ems.db \
  --mqtt-host 127.0.0.1 --mqtt-port 1883 --mqtt-base-topic ems/site1 \
  --bind 0.0.0.0 --port 8080 --ws-bind 0.0.0.0 --ws-port 8081
```

浏览器打开 `http://<开发板IP>:8080`。HTTP 页面/API 默认端口 8080，WebSocket 默认端口 8081。通过防火墙开放这两个端口；MQTT Broker 1883 通常只允许开发板本机服务连接。

## 数据职责

- MQTT 遥测更新后端内存中的最新值；WebSocket 将快照扇出到多个页面客户端。
- UI 控制请求经后端校验后发布到 MQTT。EMS 收到命令后再次检查点表范围、联锁和过期时间，执行 Modbus 写入并通过 MQTT 返回结果。
- EMS 每 60 秒向 SQLite 写一次正常点位快照，默认保留 7 天；可在 EMS 启动参数中调整。
- SQLite 另保留少量命令结果审计/去重记录；它不是实时值缓存或命令队列。

Python 需要提供 `sqlite3` 标准库模块。若 `import sqlite3` 失败，请改用系统发行版提供的 Python 解释器。
