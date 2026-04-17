# 基于 ROCK 3A 的双模式多传感器联动监控终端

## 1. 项目简介

本项目运行在 **Radxa ROCK 3A** 上，目标是实现一个“**双模式多传感器联动监控终端**”。

系统集成了以下硬件与功能：

- **Raspberry Pi Camera Module v2**：用于实时视频预览与抓拍
- **HC-SR501 PIR 人体红外传感器**：用于人体触发检测
- **SHT20 温湿度传感器**：用于采集环境温湿度
- **PWM 无源蜂鸣器**：用于触发时的本地声音提醒
- **Web 控制台**：用于查看状态、实时画面、日志，并切换工作模式

当前系统包含两种运行模式：

- **monitor 模式**：网页端实时查看视频画面，并显示当前传感器值
- **trigger 模式**：PIR 触发后鸣笛提醒、抓拍、读取 SHT20、记录事件日志

---

## 2. 项目目标

系统最终目标如下：

1. **monitor 模式**
   - 网页端实时查看视频画面
   - 实时显示 PIR、温度、湿度等状态信息

2. **trigger 模式**
   - 等待 PIR 触发
   - 触发后先鸣笛提醒
   - 触发后读取 SHT20
   - 抓拍并保存 JPG 图片
   - 追加写入 `events.log`

3. **Web 控制台**
   - 支持切换 `monitor / trigger` 模式
   - 支持查看当前状态
   - 支持查看日志
   - 支持查看最新抓拍图像
   - 在 `monitor` 模式下支持实时视频预览

---

## 3. 硬件组成

- **主控板**：Radxa ROCK 3A
- **摄像头**：Raspberry Pi Camera Module 2
- **PIR 传感器**：HC-SR501
- **温湿度传感器**：SHT20（I2C）
- **无源蜂鸣器**：已支持通过 PWM 接入 `sensor_hub`

### 当前连接信息

- Camera：CSI，用户态可通过 `/dev/video0` 访问
- SHT20：挂载在 `/dev/i2c-2`，设备地址 `0x40`
- PIR：已接入 `sensor_hub` 驱动，不再使用 `libgpiod` 作为正式方案
- Buzzer：接在 `PWM9_M0 / PIN18`，需要同时启用 `sensor-hub.dtbo` 与 `rk3568-pwm9-m0`

---

## 4. 软件架构概览

整个系统分为三层：

### 4.1 内核态：`sensor_hub` 驱动

自定义字符设备：

- `/dev/sensor_hub`

当前已接入两个输入端点和一个输出端点：

- `SH_SENSOR_PIR`
- `SH_SENSOR_SHT20`
- `SH_SENSOR_BUZZER`

支持以下能力：

- `read/poll`：读取传感器事件
- `ioctl`：
  - `SH_IOC_GET_INFO`
  - `SH_IOC_GET_SNAPSHOT`
  - `SH_IOC_CLR_EVENTS`
  - `SH_IOC_FORCE_REFRESH`
  - `SH_IOC_GET_SENSOR_CFG`
  - `SH_IOC_SET_SENSOR_CFG`
  - `SH_IOC_RUN_ACTION`

---

### 4.2 用户态：`monitor_daemon`

用户态 daemon 负责：

- 与 `/dev/sensor_hub` 通信
- 管理摄像头采集
- 执行 trigger 模式下的抓拍流程
- 维护应用状态
- 启动 HTTP 服务供网页访问

主要模块：

- `sensor_client.c`：封装 `sensor_hub` 用户态访问
- `camera_v4l2.c`：封装 V4L2 摄像头访问
- `app_state.c`：维护当前模式、快照、最近抓拍状态
- `http_server.c`：提供 Web API 与页面服务
- `main.c`：主流程与模式切换逻辑

---

### 4.3 Web 控制台

网页端当前支持：

- 查看当前模式
- 查看 PIR 状态
- 查看蜂鸣器状态
- 查看温度、湿度
- 查看最近事件时间
- 查看最近抓拍状态
- 查看最新抓拍图像
- 在 `monitor` 模式下查看实时预览
- 查看事件日志
- 切换 `monitor / trigger` 模式

---

## 5. 目录结构

```text
kernel/
├── overlay/
│   └── sensor-hub-overlay.dts      # 通过 rsetup 加载的第三方 overlay
└── sensor_hub/
    ├── Makefile
    ├── sensor_hub_ioctl.h          # ioctl 定义，与用户态共享
    ├── sensor_hub.h                # 通用结构体定义
    ├── sensor_hub_core.c           # hub 核心实现
    ├── sensor_hub_pir.c            # PIR 驱动接入
    ├── sensor_hub_sht20.c          # SHT20 驱动接入
    └── sensor_hub_buzzer.c         # PWM 蜂鸣器驱动接入

user/
├── daemon/
│   ├── include/
│   │   ├── camera_v4l2.h
│   │   ├── sensor_hub_ioctl.h
│   │   ├── sensor_client.h
│   │   ├── app_state.h
│   │   └── http_server.h
│   ├── camera_v4l2.c               # 摄像头采集封装
│   ├── sensor_client.c             # sensor_hub client 实现
│   ├── app_state.c                 # 应用状态管理
│   ├── http_server.c               # HTTP 服务
│   ├── hub_test.c                  # sensor_hub 测试程序
│   └── main.c                      # 主程序
├── web/
│   └── index.html                  # Web 控制台首页
└── output/                         # 输出目录：照片、日志等

readme.md
```

------

## 6. 当前已完成功能

### 6.1 Camera

- `/dev/video0` 已可正常使用
- 已完成 `camera_v4l2.c/.h`
- 支持 `1920x1080`
- 支持 `NV12`
- 支持 `mmap` 抓帧
- 支持抓图并转换为 JPG（trigger 路径）

### 6.2 PIR

- GPIO 已确认可用
- 已正式接入 `sensor_hub` 驱动
- PIR 事件可通过 `poll/read` 收到

### 6.3 SHT20

- 已确认工作在 `/dev/i2c-2`
- 地址为 `0x40`
- 已正式接入 `sensor_hub` 驱动

### 6.4 sensor_hub

- `GET_INFO` 正常
- `GET_SNAPSHOT` 正常
- `FORCE_REFRESH(SHT20)` 正常
- PIR 事件流正常
- `RUN_ACTION(BUZZER)` 正常

### 6.5 用户态 daemon

- `monitor` 模式已跑通
- `trigger` 模式已跑通

当前 `trigger` 模式逻辑为：

1. 等待 PIR 事件
2. `RUN_ACTION(BUZZER, ALERT)`
3. `FORCE_REFRESH SHT20`
4. 获取 snapshot
5. 抓拍
6. 转 JPG
7. 写入 `output/events.log`

### 6.6 Web 控制台

当前已完成最小可用版本，支持：

- `GET /api/status`
- `POST /api/mode?value=monitor|trigger`
- `GET /api/logs`
- `GET /stream.mjpg`
- `/output/*.jpg` 静态访问

------

## 7. 两种运行模式

## 7.1 monitor 模式

启动方式：

```bash
./monitor_daemon monitor
```

功能：

- 周期性更新当前传感器状态
- 网页端显示实时视频预览
- 网页端显示当前温湿度、PIR、蜂鸣器状态和最近状态信息

### 推荐实现链路

为获得更可用的实时预览，推荐 monitor 模式采用以下链路：

```text
V4L2 连续采集 NV12
    -> 内存中编码 JPEG
    -> HTTP /stream.mjpg 持续输出
    -> 浏览器 <img src="/stream.mjpg"> 实时显示
```

### 说明

不建议继续使用：

```text
抓图 -> 落盘 live_monitor.jpg -> HTTP 重复读取文件 -> 浏览器轮询刷新
```

这种方式会引入额外的软件编码、文件 I/O 和重复 HTTP 请求，难以获得稳定的高帧率预览。

### 推荐参数

- 分辨率：`1280x720`
- 帧率：`30fps`
- 格式：`NV12`
- 输出：`multipart/x-mixed-replace` MJPEG

------

## 7.2 trigger 模式

启动方式：

```bash
./monitor_daemon trigger
```

功能：

- 等待 PIR 触发
- 触发后蜂鸣器提醒
- 触发后刷新 SHT20 数据
- 获取 snapshot
- 抓拍并保存 JPG
- 记录事件日志

典型流程：

```text
PIR 触发
 -> RUN_ACTION(BUZZER, ALERT)
 -> FORCE_REFRESH(SHT20)
 -> GET_SNAPSHOT
 -> 抓拍一张图片
 -> 保存 output/snap_*.jpg
 -> 追加写入 output/events.log
```

### trigger 模式特点

- 更关注“事件发生时的高质量抓拍”
- 可以继续保留 `1920x1080` 单帧抓拍逻辑
- 不需要承担 monitor 模式的实时视频压力
- 即使没有新触发，也会每秒同步一次 snapshot，保证网页上的 PIR/蜂鸣器状态及时回落

------

## 8. Web API 说明

### 8.1 获取当前状态

```http
GET /api/status
```

返回示例：

```json
{
  "mode": "monitor",
  "pir": 0,
  "buzzer_active": 0,
  "buzzer_freq_hz": 2400,
  "temperature": 25.60,
  "humidity": 48.20,
  "last_image": "output/snap_20260409_203000_1920x1080_nv12.jpg",
  "last_event_time": "2026-04-09 20:30:00",
  "last_capture_ok": 1,
  "snapshot_seq": 123
}
```

------

### 8.2 切换模式

```http
POST /api/mode?value=monitor
POST /api/mode?value=trigger
```

------

### 8.3 查看日志

```http
GET /api/logs
```

------

### 8.4 实时视频流

```http
GET /stream.mjpg
```

说明：

- 仅在 `monitor` 模式下有意义
- 浏览器可直接通过 `<img src="/stream.mjpg">` 显示
- 推荐由后台统一采集/编码，HTTP 客户端共享同一份最新帧数据

------

### 8.5 查看抓拍图像

```http
GET /output/xxx.jpg
```

------

## 9. 如何编译

## 9.1 内核驱动编译

进入 `kernel/sensor_hub` 目录，执行：

```bash
make
```

编译得到：

```text
sensor_hub.ko
```

------

## 9.2 设备树 overlay 编译

进入 `kernel/overlay` 目录，执行：

```bash
dtc -@ -I dts -O dtb -o sensor-hub.dtbo sensor-hub-overlay.dts
```

生成：

```text
sensor-hub.dtbo
```

------

## 9.3 用户态 APP 编译

进入 `user` 目录，执行：

```bash
gcc -O2 -Wall \
  -I./daemon/include \
  -o monitor_daemon \
  ./daemon/main.c \
  ./daemon/sensor_client.c \
  ./daemon/camera_v4l2.c \
  ./daemon/app_state.c \
  ./daemon/http_server.c \
  -lturbojpeg \
  -lpthread
```

### 说明

- 如果 monitor 模式采用“内存编码 + `/stream.mjpg`”方案，需要链接 `libturbojpeg`
- 如果 trigger 模式仍然沿用 `ffmpeg` 做单帧 JPG 转换，则运行环境中需要可用的 `ffmpeg`

------

## 10. 如何运行

### 10.1 加载 overlay

执行：

```bash
sudo rsetup
```

然后进入 overlay 设置，选择第三方 overlay，并加载上一步生成的：

```text
sensor-hub.dtbo
```

如果你使用的是当前文档里的蜂鸣器接线，还需要在官方 overlay 里启用：

```text
rk3568-pwm9-m0
```

之后重启开发板。

------

### 10.2 加载驱动

进入驱动目录后执行：

```bash
sudo insmod sensor_hub.ko
```

可根据需要使用以下命令确认设备是否正常：

```bash
ls -l /dev/sensor_hub
ls -l /dev/video0
```

------

### 10.3 启动程序

#### 启动 monitor 模式

```bash
./monitor_daemon monitor
```

#### 启动 trigger 模式

```bash
./monitor_daemon trigger
```

------

### 10.4 打开网页控制台

浏览器访问：

```text
http://<开发板IP>:8080/
```

打开后可：

- 查看当前模式
- 查看 PIR / 温湿度
- 查看最新抓拍图
- 观看 monitor 模式实时视频
- 查看事件日志
- 切换 monitor / trigger 模式

------

## 11. 输出结果

输出结果保存在 `output/` 目录下。

典型内容包括：

- `snap_*.jpg`：trigger 模式下抓拍图片
- `events.log`：事件日志

### monitor 模式下

- 网页中应能看到实时视频预览
- 控制台会周期性打印当前传感器状态

### trigger 模式下

- 当 PIR 触发时，程序会抓拍一张图片
- 输出 JPG 到 `output/`
- 同时向 `output/events.log` 追加写入一条记录

------

## 12. 日志示例

```text
2026-04-09 20:30:00 MODE=trigger PIR=1 BUZZER=1 TEMP=25.60C HUMI=48.20%RH IMG=output/snap_20260409_203000_1920x1080_nv12.jpg
```

------

## 13. 调试建议

### 13.1 摄像头相关

确认：

```bash
ls -l /dev/video0
v4l2-ctl --all -d /dev/video0
```

------

### 13.2 sensor_hub 相关

确认：

```bash
ls -l /dev/sensor_hub
```

可使用 `hub_test` 验证：

- `GET_INFO`
- `GET_SNAPSHOT`
- `FORCE_REFRESH(SHT20)`
- PIR 事件

------

### 13.3 Web 预览相关

如果 monitor 画面不流畅，优先检查：

1. 是否仍在使用“落盘 `live_monitor.jpg` 再轮询”的旧实现
2. monitor 分辨率是否过高
3. 是否为每个 HTTP 客户端单独抓帧/单独编码
4. HTTP server 是否被单连接阻塞
5. 是否已改为后台统一采集、统一编码、共享最新帧

------

## 14. 后续可继续优化方向

1. 将 monitor 模式进一步切换为 **Rockchip MPP 硬件 JPEG / H.264 编码**
2. 在网页端加入：
   - 模式状态提示
   - 自动重连
   - 抓拍历史列表
3. 增加更多输出联动策略（蜂鸣器节奏、告警分级等）
4. 增加事件保留策略与日志轮转
5. 增加更细粒度的传感器配置接口

------

## 15. 当前结论

当前项目已经具备完整的基础能力：

- sensor_hub 驱动已接入两个输入端点和一个 PWM 输出端点
- trigger 模式已支持蜂鸣器提醒、抓拍与日志留痕
- monitor 模式已具备网页预览能力
- Web 控制台已可查看状态、日志、图像并切换模式，且 trigger 模式状态会持续同步

下一阶段的重点是：

**在不推翻现有 `sensor_hub + monitor_daemon + web` 架构的前提下，把 monitor 模式的视频链路优化为真正可用的高帧率实时预览。**
