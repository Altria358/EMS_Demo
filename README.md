# EMS Gateway

EMS 网关通过南向 Modbus TCP/RTU 采集和控制 PCS、BMS、电表等设备，并保留北向 Modbus TCP 从站供现有主站或调试工具使用。EMS 与 Web 后端通过本机 MQTT Broker 交换实时遥测和控制消息；SQLite 只保存稀疏历史采样及少量命令审计/去重记录。

## 运行结构

```text
下端设备 ⇄ Modbus TCP/RTU ⇄ EMS ⇄ MQTT Broker ⇄ Web 后端 ⇄ WebSocket ⇄ UI
                                      │
                                      └── EMS 每分钟采样 → SQLite 历史趋势
```

- EMS 将一秒一次的当前点表快照发布到 `<base-topic>/telemetry`，使用 QoS 0 和 retained 最新快照。
- 后端通过 MQTT 订阅实时值，并通过 WebSocket 推送给所有打开页面的客户端。
- UI 经 HTTP/JSON 把控制请求交给后端；后端发布到 `<base-topic>/command`。EMS 重做点表范围、设备、联锁和过期时间校验后，通过 Modbus 写下端设备，再把执行结果发布到 `<base-topic>/command/result`。
- `done` 表示 EMS 的 Modbus 写请求成功，不代表设备已经达到目标；仍需检查反馈遥测/遥信。
- 默认 SQLite 趋势间隔为 60 秒，保留 7 天。命令审计/去重记录保留 30 天。实时值和命令传输不经过 SQLite。

## Linux 安装依赖

Ubuntu/Debian 示例：

```bash
sudo apt update
sudo apt install -y cmake g++ libsqlite3-dev libmosquitto-dev mosquitto mosquitto-clients python3-venv
sudo cp config/mosquitto-ems.conf /etc/mosquitto/conf.d/ems.conf
sudo systemctl enable --now mosquitto
```

Broker 配置只监听回环地址（`127.0.0.1:1883`），并启用持久化会话/保留消息。匿名访问（`allow_anonymous true`）仅在这种纯回环监听下可接受：一旦改成 `0.0.0.0` 或新增任何非回环 listener，就必须关闭匿名访问并配置 `password_file` 与 ACL，否则能访问该端口的任何主机都可以向 `<base-topic>/command` 发布控制命令，进而驱动下端设备。

持久化目录：仓库内的 `config/mosquitto-ems.conf` 使用相对路径 `data/mosquitto/`（相对 Broker 工作目录解析，末尾必须带 `/`，且目录需已存在，仓库已带 `data/mosquitto/.gitkeep`），因此需从项目根目录启动 Broker。若按上面方式安装成系统服务，`/etc/mosquitto/conf.d/` 中的配置应以 `mosquitto` 用户可写的绝对路径为准，通常改成 `persistence_location /var/lib/mosquitto/`。

Python 需带有 `sqlite3` 标准库模块。创建虚拟环境并安装 Web 后端依赖：

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r web_backend/requirements.txt
```

若系统 Python 缺少 `sqlite3` 模块，应使用发行版提供的 Python/SQLite 组件；不要使用未带 SQLite 扩展的自编译 Python。

## 编译

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

ARM64 开发板上直接安装依赖并本机编译最简单。若在 x86 主机交叉编译，需提供与目标 ARM64 匹配的 `libmosquitto` 头文件和库，并把 `CMAKE_SYSROOT` 指向目标 sysroot；不能把 x86 的 MQTT 库链接进 ARM 程序。

## 启动

两个进程默认连接本机 Mosquitto，必须指向同一个 SQLite 文件以共享历史趋势：

```bash
./build/ems_gateway --config config/simulator --db data/ems.db \
  --mqtt-host 127.0.0.1 --mqtt-port 1883 --mqtt-base-topic ems/site1 \
  --history-interval-sec 60 --history-days 7
```

另开终端启动后端：

```bash
source .venv/bin/activate
export EMS_WEB_CONTROL_TOKEN="$(openssl rand -hex 32)"
python web_backend/server.py --config config/simulator --db data/ems.db \
  --mqtt-host 127.0.0.1 --mqtt-port 1883 --mqtt-base-topic ems/site1 \
  --bind 0.0.0.0 --port 8080 --ws-bind 0.0.0.0 --ws-port 8081
```

浏览器访问 `http://<开发板IP>:8080`。UI 实时通道连接 `ws://<开发板IP>:8081`。防火墙需允许 8080/TCP 和 8081/TCP；MQTT 1883 默认只供开发板本机 EMS/后端使用，不要直接暴露给浏览器网络。

若 Mosquitto 配置了用户名和密码，可通过 `EMS_MQTT_USERNAME`、`EMS_MQTT_PASSWORD` 或 `--mqtt-username`、`--mqtt-password` 同时配置 EMS 和后端。

## 消息主题

默认根主题 `ems/site1`，可通过 `--mqtt-base-topic` 修改：

| Topic | 方向 | 内容 |
|---|---|---|
| `ems/site1/telemetry` | EMS → 后端 | 全量当前值、质量码、时间戳 |
| `ems/site1/command` | 后端 → EMS | `device.write` 命令，含唯一 ID 和 15 秒过期时间 |
| `ems/site1/command/result` | EMS → 后端 | `executing`、`done` 或 `failed` 结果 |
| `ems/site1/status` | EMS → 后端 | EMS 在线状态，保留消息和遗嘱 |

QoS 1 表示至少一次投递，重复消息由 EMS 的近期内存缓存和 SQLite 命令记录去重。实际设备动作的“恰好一次”仍取决于下端设备协议及命令本身是否幂等。

## 延伸阅读

- [架构说明](docs/架构说明.md)：完整的数据流、接口契约、配置项、可靠性与运维说明。

