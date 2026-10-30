# EMS Gateway

EMS 通过南向 Modbus TCP/RTU 采集和控制 PCS、BMS、电表等设备。EMS 和网页后端运行在同一台 Linux 开发板上，并通过同一个本地 SQLite 文件交换实时值、历史值和控制命令；两个进程之间不建立 Modbus TCP 或 HTTP 连接。浏览器只通过 HTTP/JSON 和 WebSocket 访问网页后端。

## 运行结构

```text
下端设备 ⇄ Modbus TCP/RTU ⇄ EMS ⇄ 本地 SQLite ⇄ 网页后端 ⇄ HTTP/WebSocket ⇄ UI
```

- EMS 每秒把当前值和质量码写到 `realtime`，按配置周期把正常值采样到 `history`。
- 网页后端从 `realtime` 读值，通过 WebSocket 向多个 UI 客户端推送；历史趋势从 `history` 查询。
- UI 用 HTTP/JSON 提交控制请求，后端把校验后的命令插入 `commands`。EMS 轮询并认领命令，重新校验设备、地址、范围、联锁和过期时间后才执行 Modbus 写操作。
- `done` 表示 EMS 的 Modbus 写请求成功，不代表设备已达到目标；操作员仍应检查反馈遥测/遥信。
- SQLite 默认保存 7 天历史，历史采样默认 60 秒；命令终态记录保留 30 天。可通过启动参数调整历史保留设置。

SQLite 表结构：`realtime`（实时值）、`history`（趋势，含 `quality`）、`commands`（命令队列与审计）。质量码：`1` 正常、`0` 异常、`2` 陈旧、`3` 未初始化。命令状态经 `pending → done/failed` 流转。

启动、排错与版本切换见 [`docs/运行手册.md`](docs/运行手册.md)。

## Linux 依赖

Ubuntu/Debian 示例：

```bash
sudo apt update
sudo apt install -y cmake g++ libsqlite3-dev python3-venv python3-dev
```

Python 必须包含标准库 `sqlite3`。创建网页后端环境并安装 WebSocket 依赖：

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r web_backend/requirements.txt
```

## 编译

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

建议在 ARM64 Linux 开发板上本机编译。共享 SQLite 文件应放在开发板本地文件系统，例如 SD 卡上的 ext4 分区；不要让两个进程通过网络共享目录或 Windows 挂载盘访问 WAL 数据库。

## 启动

EMS 和网页后端的 `--db` 必须指向同一个文件。建议从项目根目录启动：

```bash
./build/ems_gateway --config config/simulator --db data/ems.db \
  --history-interval-sec 60 --history-days 7
```

另开终端启动后端：

```bash
source .venv/bin/activate
export EMS_WEB_CONTROL_TOKEN="$(openssl rand -hex 32)"
python web_backend/server.py --config config/simulator --db data/ems.db \
  --bind 0.0.0.0 --port 8080 --ws-bind 0.0.0.0 --ws-port 8081
```

浏览器访问 `http://<开发板IP>:8080`。防火墙开放 HTTP 8080/TCP 和 WebSocket 8081/TCP。控制接口必须配置随机 Bearer 令牌；令牌不要写进点表或提交到 Git。