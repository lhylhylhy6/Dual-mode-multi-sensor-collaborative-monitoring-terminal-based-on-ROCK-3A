# 接口与运行说明

## 1. 运行前提

这个项目依赖的运行环境不是普通 PC，而是带有以下资源的 Linux 开发板环境：

- ROCK 3A 开发板
- 可用的 `/dev/video0`
- 可用的 `i2c-2`
- 可加载设备树 overlay
- 可加载自定义内核模块

从当前代码实现看，运行时还有两个容易忽略的前提：

- 用户态程序需要能访问 `libturbojpeg`
- `trigger` 模式转换 JPG 时依赖 `ffmpeg`

## 2. 重要路径约定

当前守护进程里使用了相对路径：

- `WEB_INDEX_PATH = "web/index.html"`
- `OUTPUT_DIR = "output"`

这意味着最稳妥的运行方式是：

- 先进入 `user/` 目录
- 再启动 `monitor_daemon`

否则 Web 页面和输出目录路径可能对不上。

## 3. 编译说明

## 3.1 编译内核模块

进入内核驱动目录：

```bash
cd kernel/sensor_hub
make
```

输出文件：

```text
sensor_hub.ko
```

当前 `Makefile` 会把以下对象链接成模块：

- `sensor_hub_core.o`
- `sensor_hub_pir.o`
- `sensor_hub_sht20.o`
- `sensor_hub_buzzer.o`

## 3.2 编译设备树 overlay

进入 overlay 目录：

```bash
cd kernel/overlay
dtc -@ -I dts -O dtb -o sensor-hub.dtbo sensor-hub-overlay.dts
```

输出文件：

```text
sensor-hub.dtbo
```

说明：

- overlay 已经按当前接线写死了 `PWM9_M0 / PIN18`
- `sensor-hub.dtbo` 负责描述 `pwms = <&pwm9 0 416667 0>` 和蜂鸣器默认参数
- 板端还需要在 `rsetup` 里额外启用官方 overlay `rk3568-pwm9-m0`，这样 PWM9 的 pinmux 才会真正生效

## 3.3 编译用户态守护进程

进入 `user/` 目录：

```bash
cd user
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

说明：

- `monitor` 模式的 MJPEG 编码依赖 `libturbojpeg`
- `trigger` 模式运行时需要系统有 `ffmpeg`

## 3.4 编译 `hub_test`

如果你要单独验证驱动协议，可以编译 `hub_test.c`：

```bash
cd user
gcc -O2 -Wall \
  -I./daemon/include \
  -o hub_test \
  ./daemon/hub_test.c \
  ./daemon/sensor_client.c
```

它适合做这些事情：

- 检查 `/dev/sensor_hub` 是否能打开
- 查看 `GET_INFO`
- 查看 `GET_SNAPSHOT`
- 手动触发 `FORCE_REFRESH(SHT20)`
- 手动触发一次蜂鸣器 `ALERT`
- 等待 PIR 与蜂鸣器输出事件

## 4. 启动顺序

推荐启动顺序如下。

### 4.1 加载 overlay

在板端执行：

```bash
sudo rsetup
```

需要做两件事：

- 在官方 overlay 里启用 `rk3568-pwm9-m0`
- 把 `sensor-hub.dtbo` 作为第三方 overlay 加载

完成后重启开发板。

### 4.2 加载驱动

```bash
cd kernel/sensor_hub
sudo insmod sensor_hub.ko
```

检查设备节点：

```bash
ls -l /dev/sensor_hub
ls -l /dev/video0
```

### 4.3 启动用户态程序

进入 `user/` 目录后启动：

```bash
cd user
./monitor_daemon monitor
```

或者：

```bash
cd user
./monitor_daemon trigger
```

如果不传参数，程序默认进入 `trigger` 模式。

### 4.4 打开浏览器

访问：

```text
http://<开发板IP>:8080/
```

## 5. HTTP API 说明

## 5.1 获取当前状态

请求：

```http
GET /api/status
```

返回字段：

| 字段 | 含义 |
| --- | --- |
| `mode` | 当前模式，`monitor` 或 `trigger` |
| `pir` | 当前 PIR 电平 |
| `buzzer_active` | 蜂鸣器是否正在鸣叫 |
| `buzzer_freq_hz` | 当前蜂鸣器频率，单位 `Hz` |
| `temperature` | 当前温度，单位 `°C` |
| `humidity` | 当前湿度，单位 `%RH` |
| `last_image` | 最近一张抓拍图片相对路径 |
| `last_event_time` | 最近事件时间 |
| `last_capture_ok` | 最近抓拍是否成功 |
| `snapshot_seq` | 最近一次快照序号 |

示例：

```json
{
  "mode": "monitor",
  "pir": 0,
  "buzzer_active": 0,
  "buzzer_freq_hz": 2400,
  "temperature": 23.74,
  "humidity": 54.94,
  "last_image": "output/snap_20260415_094444_1920x1080_nv12.jpg",
  "last_event_time": "2026-04-15 09:44:45",
  "last_capture_ok": 1,
  "snapshot_seq": 123
}
```

## 5.2 切换模式

请求：

```http
POST /api/mode?value=monitor
POST /api/mode?value=trigger
```

说明：

- HTTP 层只改写 `app_state.mode`
- 真实的摄像头重配与线程切换由主线程完成

## 5.3 查看日志

请求：

```http
GET /api/logs
```

返回内容：

- `output/events.log` 的文本内容

当前实现里，HTTP 读取日志使用固定大小缓冲区，因此日志很长时不会返回完整历史。

## 5.4 实时视频流

请求：

```http
GET /stream.mjpg
```

说明：

- Content-Type 为 `multipart/x-mixed-replace`
- 仅在 `monitor` 模式下有实际意义
- 浏览器使用 `<img src="/stream.mjpg">` 即可显示

## 5.5 访问抓拍图片

请求：

```http
GET /output/<filename>.jpg
```

HTTP 服务会做基础路径安全检查，不允许带 `..` 的路径。

## 6. 驱动协议说明

统一字符设备：

```text
/dev/sensor_hub
```

## 6.1 `read/poll`

适合触发模式。

- `poll()`：等待有事件到来
- `read()`：读取一个或多个 `struct sh_event`

当前最重要的事件来源是：

- PIR 中断事件
- 蜂鸣器输出状态变化事件（`ALERT` / `OUTPUT_OFF`）

## 6.2 `ioctl`

主要命令如下：

- `SH_IOC_GET_INFO`
- `SH_IOC_GET_SNAPSHOT`
- `SH_IOC_CLR_EVENTS`
- `SH_IOC_FORCE_REFRESH`
- `SH_IOC_GET_SENSOR_CFG`
- `SH_IOC_SET_SENSOR_CFG`
- `SH_IOC_RUN_ACTION`

其中 `SH_IOC_RUN_ACTION` 当前已经用于蜂鸣器：

- `SH_ACTION_ALERT`
- `SH_ACTION_PULSE`
- `SH_ACTION_STOP`

## 6.3 端点 ID

当前已经实现：

- `SH_SENSOR_PIR = 1`
- `SH_SENSOR_SHT20 = 2`
- `SH_SENSOR_BUZZER = 6`

协议里还为将来的其他输入/输出端点留了扩展空间。

## 7. 输出文件说明

## 7.1 抓拍图片

命名格式：

```text
output/snap_YYYYMMDD_HHMMSS_1920x1080_nv12.jpg
```

中间会先生成 `.yuv`，转码成功后删除，只保留最终 JPG。

## 7.2 事件日志

路径：

```text
output/events.log
```

格式：

```text
时间 MODE=trigger PIR=1 BUZZER=1 TEMP=xx.xxC HUMI=xx.xx%RH IMG=output/xxx.jpg
```

示例：

```text
2026-04-15 10:25:23 MODE=trigger PIR=1 BUZZER=1 TEMP=23.55C HUMI=55.49%RH IMG=output/snap_20260415_102521_1920x1080_nv12.jpg
```

## 8. 调试建议

## 8.1 先验证驱动，再验证整机

推荐顺序：

1. 先看 `/dev/sensor_hub` 是否存在
2. 用 `hub_test` 验证 `GET_INFO` 和 `GET_SNAPSHOT`
3. 再验证 PIR 触发是否能收到事件
4. 最后再跑 `monitor_daemon`

这样比直接上整机更容易定位问题。

## 8.2 监控模式常见问题

### 现象：网页打不开实时画面

优先检查：

- 守护进程是否以 `monitor` 模式启动
- `/dev/video0` 是否可用
- `libturbojpeg` 是否可用
- 浏览器是否能访问 `http://<ip>:8080/stream.mjpg`

### 现象：模式切换后没有画面

优先检查：

- 是否看到控制台输出里的 mode switch 日志
- 摄像头是否被成功重开
- 是否发生了回滚

## 8.3 触发模式常见问题

### 现象：PIR 触发了但没有图片

优先检查：

- `events.log` 是否有记录
- 是否安装 `ffmpeg`
- `output/` 是否有写权限
- 摄像头抓帧是否成功

### 现象：蜂鸣器已经停了，但网页还显示在响；或者 PIR 已经回到 0，网页还停在 1

优先检查：

- 当前运行的 `monitor_daemon` 是否已经包含 trigger 模式每秒同步 snapshot 的修复
- `/api/status` 里的 `snapshot_seq` 是否在 `trigger` 模式下持续递增
- 是否仍在运行旧二进制

### 现象：温湿度总是 0

优先检查：

- SHT20 是否在 `i2c-2`
- 地址是否为 `0x40`
- `FORCE_REFRESH(SHT20)` 是否报错

## 8.4 路径问题

如果出现首页打不开或图片目录不对，首先确认程序是不是在 `user/` 目录下启动。因为当前代码使用的是相对路径，而不是绝对路径。

## 9. 演示时的推荐流程

如果你后面要把这个项目展示给老师、面试官或同学，建议演示顺序如下：

1. 展示 `hub_test` 能拿到 snapshot，说明内核驱动和用户态通信是通的
2. 启动 `monitor` 模式，展示网页实时视频和温湿度状态
3. 切到 `trigger` 模式，现场触发 PIR
4. 展示蜂鸣器提醒、PIR 状态自动回落，以及新生成的 JPG 和新增日志
5. 再解释为什么要做双模式，而不是只做一个模式

这个顺序最能体现项目的完整性。
