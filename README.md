# esp_nas_status

UGREEN NAS 通知桥接 + ESP8266 状态接口服务。

这个项目做两件事：

- `nas_status_server.py` 对 ESP8266 提供 `/status?token=...`
- `bridge_poll.py` 从 UGREEN NAS 消息中心拉取最新通知，再转发到本地状态服务

现在仓库也同时包含 ESP8266 屏幕端固件源码和预编译二进制。

它已经支持：

- 脚本自己用 NAS 用户名/密码登录
- token 过期后自动重新登录
- Docker 镜像通过 GitHub Actions 自动构建并推送到 Docker Hub 和 GHCR

镜像地址：

- `docker.io/hanfu1997/esp_nas_status:latest`

同时也会同步推到：

- `ghcr.io/htx996/esp_nas_status:latest`

## 仓库目录

- `nas_status_server.py`
- `push_event.py`
- `bridge_poll.py`
- `firmware/`
- `bridge_config.example.json`
- `requirements.txt`
- `docker-compose.yml`
- `Dockerfile`
- `.env.example`
- `data/`
- `examples/`

## ESP8266 固件

固件目录：

- `firmware/esp_nas_monitor/`

预编译固件：

- `firmware/releases/esp.bin`

固件目录说明见：

- `firmware/README.md`

## 状态接口

`GET /status?token=你的token`

```json
{
  "cpu": 23,
  "mem": 61,
  "disk": 72,
  "temp": 48,
  "down": "32MB/s",
  "up": "5MB/s",
  "uptime": "2d 03:11:29",
  "event": {
    "id": "524",
    "app": "应用中心",
    "level": "info",
    "time": "2026-05-24 00:17",
    "message": "在线文档安装成功"
  }
}
```

没有最新事件时，`event` 为 `null`。

## 直接用 GHCR 镜像部署

1. 克隆仓库
2. 复制示例配置
3. 启动 compose

```bash
git clone https://github.com/htx996/esp_nas_status.git
cd esp_nas_status
cp .env.example .env
cp bridge_config.example.json bridge_config.json
cp data/ugreen_password.txt.example data/ugreen_password.txt
```

然后编辑这三个文件：

- `.env`
  - `TOKEN`
  - `PORT`
  - `DISK_PATH`
- `bridge_config.json`
  - `server_token`
  - `source.login.username`
- `data/ugreen_password.txt`
  - 改成你的 NAS 登录密码

启动：

```bash
docker compose up -d
```

`docker-compose.yml` 默认使用：

- `hanfu1997/esp_nas_status:${IMAGE_TAG:-latest}`

## 手工推送一条事件

```bash
python3 push_event.py \
  --server http://127.0.0.1:8099 \
  --token changeme \
  --app 应用中心 \
  --level info \
  --time "2026-05-24 00:17" \
  --id appcenter-20260524-0017 \
  --message "在线文档安装成功"
```

## 自动轮询 UGREEN 消息中心

默认配置在 `bridge_config.json`。

默认模式：

- `source.mode = "ugreen_message_api"`
- `source.base_url = "http://127.0.0.1:8023"`
- `source.login.password_file = "data/ugreen_password.txt"`

### 看原始消息

```bash
python3 bridge_poll.py --config bridge_config.json --once --print-source
```

### 只做标准化，不推送

```bash
python3 bridge_poll.py --config bridge_config.json --once --dry-run
```

### 真正推送到状态服务

```bash
python3 bridge_poll.py --config bridge_config.json --once
```

### 持续轮询

```bash
python3 bridge_poll.py --config bridge_config.json
```

## GitHub Actions 自动构建镜像

工作流文件：

- `.github/workflows/docker-publish.yml`

触发条件：

- push 到 `main`
- push `v*` tag
- 手工 `workflow_dispatch`

推送目标：

- `docker.io/hanfu1997/esp_nas_status`
- `ghcr.io/htx996/esp_nas_status`

默认会发布这些 tag：

- `latest`
- 分支名
- Git tag
- `sha-*`

### 开启 Docker Hub 自动发布

按 Docker 官方的 GitHub Actions 做法，需要给仓库补一个变量和一个 secret：

- Repository Variable: `DOCKERHUB_USERNAME`
- Repository Secret: `DOCKERHUB_TOKEN`

其中 `DOCKERHUB_TOKEN` 应该使用 Docker Hub 的 Access Token，不要直接用密码。

这两个值配好后，同一个 workflow 会同时推：

- GHCR
- Docker Hub

如果没配这两个值，workflow 只会继续推 GHCR，不会失败。

## 本地开发构建

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 nas_status_server.py
```

本地手工构建镜像：

```bash
docker build -t esp_nas_status:local .
```

## 注意

- 仓库不会提交真实 `bridge_config.json`
- 仓库不会提交真实 `data/ugreen_password.txt`
- 运行态数据如 `data/latest_event.json`、`data/bridge_state.json` 默认忽略
- 当前桥接默认只转发“最新一条未转发事件”
