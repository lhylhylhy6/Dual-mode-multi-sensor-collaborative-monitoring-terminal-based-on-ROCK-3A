# 架构深挖

## 1. 阅读这份文档前，先记住三个结论

### 1.1 这是一个“统一端点入口”的系统

用户态并不直接分别操作 GPIO、I2C 和 PWM，而是统一访问 `/dev/sensor_hub`。这是这个项目最核心的架构选择。

### 1.2 `monitor` 和 `trigger` 使用了两套不同的图像链路

- `monitor`：连续采集，内存编码，MJPEG 推流
- `trigger`：事件驱动，单帧抓拍，落地 JPG

这不是重复实现，而是有意分工。

### 1.3 模式切换不是前端直接控制摄像头，而是“先改状态，再由主线程完成真实切换”

前端只通过 `/api/mode` 改写应用状态，真正的摄像头关闭、重开、线程启动、失败回滚，都由 `main.c` 主线程负责。这使得状态流转更清晰。

## 2. 目录与职责映射

```text
kernel/
  overlay/
    sensor-hub-overlay.dts
  sensor_hub/
    sensor_hub_ioctl.h
    sensor_hub.h
    sensor_hub_core.c
    sensor_hub_pir.c
    sensor_hub_sht20.c
    sensor_hub_buzzer.c

user/
  daemon/
    include/
    sensor_client.c
    camera_v4l2.c
    app_state.c
    http_server.c
    hub_test.c
    main.c
  web/
    index.html
  output/
```

可以把它理解成下面这个分工：

- `sensor_hub_ioctl.h`：内核和用户态共享协议
- `sensor_hub_core.c`：驱动核心框架
- `sensor_hub_pir.c`：PIR 接入与中断事件
- `sensor_hub_sht20.c`：SHT20 接入与周期采样
- `sensor_hub_buzzer.c`：PWM 蜂鸣器接入与输出动作
- `sensor_client.c`：用户态访问驱动的薄封装
- `camera_v4l2.c`：摄像头生命周期与采集接口
- `app_state.c`：系统状态中心
- `http_server.c`：HTTP 接口与 MJPEG 输出
- `main.c`：主控逻辑、模式切换和业务编排

## 3. 内核态设计

## 3.1 设备树 overlay 的作用

`kernel/overlay/sensor-hub-overlay.dts` 做了两件事：

- 创建一个 `compatible = "poozoo,sensor-hub"` 的 platform device
- 把 PIR GPIO、SHT20 的 I2C 总线信息，以及蜂鸣器 PWM 和默认参数通过属性传给驱动

属性包括：

- `pir-gpios = <&gpio3 5 0>`
- `sht20-bus-num = <2>`
- `sht20-addr = <0x40>`
- `pwms = <&pwm9 0 416667 0>`
- `pwm-names = "buzzer"`

这里的蜂鸣器被固定接到 `PWM9_M0 / PIN18`。也就是说，驱动不是硬编码地去猜设备在哪，而是通过 DT 属性获取硬件描述。

## 3.2 `sensor_hub` 驱动核心

`sensor_hub_core.c` 定义了整个驱动的骨架。

### 3.2.1 对外暴露方式

驱动注册成了一个 `miscdevice`，设备节点名称为：

```text
/dev/sensor_hub
```

对用户态暴露四类能力：

- `read()`：读取一个或多个 `struct sh_event`
- `poll()`：等待事件到来
- `ioctl(GET_INFO)`：读取驱动元信息
- `ioctl(GET_SNAPSHOT)`：读取全量状态快照
- `ioctl(FORCE_REFRESH)`：强制刷新指定传感器
- `ioctl(GET/SET_SENSOR_CFG)`：读写单个传感器配置
- `ioctl(RUN_ACTION)`：对输出端点下发动作

### 3.2.2 两套数据模型

驱动内部同时维护：

- 事件队列 `sh_event_queue`
- 最新状态快照 `sh_snapshot`

这两个模型分别服务于不同业务：

- 事件队列服务 `trigger` 模式
- 快照服务 `monitor` 模式

### 3.2.3 为什么共享协议头设计得比较好

`sensor_hub_ioctl.h` 的设计是这个项目里非常值得讲的一部分。

优点包括：

- 有版本号 `SH_API_VERSION`
- 有统一的 `sensor id` / `sensor type` / `event code`
- 有统一的 `direction` / `caps`，同一套结构既能描述输入，也能描述输出
- 使用定点整数而不是浮点数，避免内核和用户态共享结构里使用 `float`
- 支持最多 16 个传感器和每个传感器最多 4 个数值
- 为将来扩展光照、烟雾、门磁等设备预留了 ID

例如：

- 温度以毫摄氏度保存
- 湿度以千分之一 `%RH` 保存

这样可以保证内核态和用户态的结构体 ABI 更稳定。

### 3.2.4 事件队列实现

驱动内部有一个长度为 `64` 的环形队列：

```text
SH_EVT_Q_SIZE = 64
```

入队逻辑：

- 队列满时，丢弃最旧事件
- `dropped` 计数加一
- 新事件写入队头
- 唤醒阻塞在 `poll/read` 上的用户态

这意味着它不是“阻塞生产者”的设计，而是“尽量保留最新事件”的设计，适合监控类场景。

### 3.2.5 snapshot 实现

`snapshot` 的生成方式是：

- 遍历所有已注册传感器
- 把每个 `sensor->value` 拷入 `sh_snapshot.items[]`
- 附带 `snapshot_seq`、时间戳、丢失事件数量

每次传感器值更新都会调用 `sh_update_sensor_value()`，从而推进 `snapshot_seq`。

## 3.3 PIR 子模块

`sensor_hub_pir.c` 的逻辑很清晰：

1. 从设备树中获取 `pir` GPIO
2. 把 GPIO 转成 IRQ
3. 注册线程化中断
4. 在中断线程里读取当前电平
5. 更新 PIR 当前值
6. 生成一条事件推送到队列

### 3.3.1 为什么用线程化中断

线程化中断更适合这里的场景，因为 GPIO 读取使用了 `gpiod_get_value_cansleep()`，这类接口允许睡眠，不适合在硬中断上下文里随意做更复杂的工作。

### 3.3.2 去抖动实现

PIR 模块维护了：

- `last_irq_ns`
- `debounce_ms`

如果本次 IRQ 与上次 IRQ 的时间差小于配置的去抖动时间，就直接忽略。这使得 PIR 的误触发概率更低。

### 3.3.3 事件语义

当电平为 `1` 时，事件码会写成：

- `SH_CODE_TRIGGER`

当电平为 `0` 时，事件码会写成：

- `SH_CODE_FALLING`

用户态在 `trigger` 模式下只关心“PIR 变为高电平”的情况。

## 3.4 SHT20 子模块

`sensor_hub_sht20.c` 负责 I2C 传感器的接入。

### 3.4.1 初始化过程

初始化时，驱动会：

1. 从设备树读取总线号和地址
2. 获取 `i2c_adapter`
3. 创建 `i2c_client`
4. 注册为 `SH_SENSOR_SHT20`
5. 做一次初始刷新
6. 启动 `delayed_work` 周期采样

### 3.4.2 采样过程

采样逻辑分两步：

- 发送 `0xF3`，读取温度
- 发送 `0xF5`，读取湿度

代码按 SHT20 数据手册常见公式完成换算：

- 温度：`-46.85 + 175.72 * raw / 65536`
- 湿度：`-6 + 125 * raw / 65536`

最后统一换算成毫单位整数写回 `snapshot`。

### 3.4.3 为什么 SHT20 不走事件队列

当前代码里，SHT20 刷新后只更新当前值，不主动推送事件。`sensor_hub_sht20.c` 里其实保留了一段被注释掉的“生成 sample 事件”代码。

这说明当前设计选择是：

- PIR 负责触发类事件
- SHT20 负责状态类采样

对于这个项目的双模式目标来说，这个取舍是合理的。

## 3.5 蜂鸣器子模块

`sensor_hub_buzzer.c` 负责 PWM 无源蜂鸣器接入。

### 3.5.1 初始化过程

初始化时，驱动会：

1. 通过 `devm_pwm_get(..., "buzzer")` 获取 PWM 控制器
2. 从设备树读取默认频率、占空比、时长
3. 注册为 `SH_SENSOR_BUZZER`
4. 把它声明成 `direction = SH_DIR_OUTPUT`
5. 在 snapshot 中暴露当前 `active/freq/duration/duty`

如果板端没有启用 `rk3568-pwm9-m0`，或者 overlay 里没有 `pwms`，这个模块会优雅跳过，不影响 PIR 和 SHT20 正常工作。

### 3.5.2 动作模型

蜂鸣器不走 `FORCE_REFRESH`，而是走统一动作接口：

- `SH_ACTION_ALERT`
- `SH_ACTION_PULSE`
- `SH_ACTION_STOP`

收到 `ALERT/PULSE` 后，驱动会配置 PWM、更新 snapshot，并在 `duration_ms` 到期后通过 `delayed_work` 自动关闭，同时补一条 `OUTPUT_OFF` 事件。

## 4. 用户态守护进程设计

## 4.1 `sensor_client.c`：驱动访问适配层

这个文件做的是很薄的一层封装，把底层 `open/ioctl/read/poll` 变成易读的函数：

- `sensor_client_get_info`
- `sensor_client_get_snapshot`
- `sensor_client_force_refresh`
- `sensor_client_wait_readable`
- `sensor_client_read_events`

好处是 `main.c` 不需要到处散落 `ioctl` 宏和结构体细节。

## 4.2 `camera_v4l2.c`：摄像头生命周期管理

这个模块是摄像头链路的底座，负责：

- 打开 `/dev/video0`
- 查询能力
- 设置分辨率、像素格式、帧率
- 申请 MMAP buffer
- `QBUF / DQBUF`
- `STREAMON / STREAMOFF`
- 单帧抓取并保存原始 NV12 数据

### 4.2.1 为什么这里值得讲

因为它不是简单调用现成 SDK，而是直接使用 V4L2 多平面采集接口。这本身就是比较典型的 Linux 多媒体栈工程能力。

### 4.2.2 两条使用路径

它同时服务两类场景：

- `monitor` 模式：持续 `dequeue -> 编码 -> queue`
- `trigger` 模式：跳过前几帧后抓取一帧原始图像

代码里 `CAPTURE_SKIP_FRAMES = 5`，用于避免刚切流或刚进入采集状态时抓到不稳定帧。

## 4.3 `app_state.c`：状态中心

`app_state_t` 是整个用户态的共享状态对象，里面维护：

- 当前模式
- 最近一次传感器快照
- 最近一张图像路径
- 最近事件时间
- 最近抓拍是否成功

多个线程都会访问这份状态，所以通过 `pthread_mutex_t` 做互斥保护。

## 4.4 `main.c`：真正的业务编排中心

如果只看一个文件来理解整个系统，最值得看的就是 `main.c`。

它承担了以下职责：

- 启动时初始化输出目录
- 打开 `/dev/sensor_hub`
- 下发 PIR/SHT20/蜂鸣器默认配置
- 初始化应用状态
- 启动 HTTP 服务
- 根据模式打开摄像头
- 在模式切换时关闭/重开摄像头
- 在 `monitor` 模式启动编码线程
- 在 `trigger` 模式等待 PIR 事件并执行抓拍流程

### 4.4.1 启动默认行为

程序接受一个命令行参数：

```bash
./monitor_daemon monitor
./monitor_daemon trigger
```

如果不传参数，默认进入：

```text
trigger
```

### 4.4.2 默认配置

启动后会通过 `SET_SENSOR_CFG` 下发：

- PIR：`enabled=1`，`debounce_ms=200`
- SHT20：`enabled=1`，`period_ms=1000`
- BUZZER：`enabled=1`，默认 `2400Hz / 50% duty / 180ms`

这说明用户态对驱动并不是“只读访问”，而是会主动配置底层行为。

### 4.4.3 模式切换机制

模式切换的真实流程是：

```text
前端调用 /api/mode
  -> app_state.mode 被改写
  -> main 循环发现 requested_mode != active_mode
  -> 如当前是 monitor，则先停 monitor worker
  -> 关闭当前 camera
  -> 按新模式参数重新打开 camera
  -> 如新模式是 monitor，则重新启动 monitor worker
  -> 如果重开失败，则尝试回滚到旧模式
```

这里最有工程味道的一点是“失败回滚”：

- 切换失败不会直接让系统停在半初始化状态
- 主线程会尝试恢复旧模式

这在 demo 项目里并不常见，写进项目说明会加分。

### 4.4.4 `trigger` 模式业务流程

收到 PIR 事件后，`handle_trigger()` 会执行：

1. `RUN_ACTION(BUZZER, ALERT)`
2. `FORCE_REFRESH(SHT20)`
3. `GET_SNAPSHOT`
4. 更新应用状态
5. 生成带时间戳的输出文件名
6. 从摄像头抓取一帧原始 NV12 数据
7. 调用 `ffmpeg` 转成 JPG
8. 删除中间 `.yuv`
9. 更新最近抓拍状态
10. 追加写入 `events.log`

这个流程把“传感器上下文”和“图像证据”绑定在一起，是一个很完整的事件链。

### 4.4.5 `monitor` 模式业务流程

`monitor` 模式下主线程本身不做图像编码，而是：

- 每秒获取一次传感器快照，更新状态
- 让专门的 monitor worker 线程持续处理视频帧

这样做的好处是职责清晰：

- 主线程负责状态和模式调度
- worker 线程负责高频图像处理

另外，`trigger` 模式现在也会每秒刷新一次 snapshot。这样网页上的 `pir` 和 `buzzer_active` 会在硬件状态恢复后自动回落，不会只在“下一次 PIR 触发”时才更新。

## 5. MJPEG 推流链路

这个项目在 `monitor` 模式下最有代表性的实现就是 MJPEG 推流。

完整路径如下：

```text
V4L2 获取 NV12 帧
  -> 转换成 I420
  -> TurboJPEG 压缩为 JPEG
  -> 发布到 g_mjpeg 全局缓存
  -> HTTP 客户端等待条件变量唤醒
  -> 以 multipart/x-mixed-replace 输出
  -> 浏览器 <img src="/stream.mjpg"> 实时显示
```

### 5.1 为什么先转 I420

TurboJPEG 的 `tjCompressFromYUVPlanes()` 接口要求传入 YUV 平面，因此代码先把 NV12 拆成 I420 的三平面布局，再交给 TurboJPEG 压缩。

### 5.2 为什么这个实现比“先落盘再轮询”更合理

这个项目明确避开了下面这类低效方案：

```text
抓图 -> 写 live.jpg -> HTTP 每次读文件 -> 浏览器轮询刷新
```

当前做法的优势是：

- 避免频繁磁盘 I/O
- 减少无意义的 HTTP 重复请求
- 更适合连续画面预览
- 前后端链路更接近真实监控系统

## 6. HTTP 服务与前端控制台

## 6.1 HTTP 服务

`http_server.c` 没有引入第三方 Web 框架，而是手写了一个轻量级 HTTP 服务器。

提供的主要接口有：

- `GET /`：返回首页
- `GET /api/status`：返回当前状态 JSON
- `POST /api/mode?value=monitor|trigger`：切换模式
- `GET /api/logs`：读取事件日志
- `GET /stream.mjpg`：输出实时 MJPEG
- `GET /output/*.jpg`：访问抓拍图片

### 6.1.1 `app_state` 与 HTTP 的关系

HTTP 层不直接读取硬件，而是读取 `app_state`。这意味着：

- 前端只依赖统一状态中心
- HTTP 层和底层驱动、摄像头解耦
- 后续如果换成别的前端，也可以继续复用同一套状态接口

### 6.1.2 MJPEG 缓存设计

HTTP 服务内部维护了一个全局缓存结构：

- `buf`
- `len`
- `seq`
- `valid`
- `mutex + cond`

monitor worker 发布新 JPEG 后会：

- 拷贝进缓存
- `seq++`
- `pthread_cond_broadcast()`

每个 MJPEG 客户端则等待 `seq` 变化，再拿到最新帧。这种设计实现简单，但已经具备“多客户端共享同一份最新帧”的思路。

## 6.2 Web 控制台

前端页面功能比较克制，但很完整：

- 切换 `monitor / trigger`
- 立即刷新
- 查看当前模式
- 查看 PIR 状态
- 查看蜂鸣器状态
- 查看温度、湿度
- 查看最近事件时间
- 查看抓拍状态
- 显示实时视频或最近图片
- 查看日志

页面逻辑是：

- 每 1 秒刷新 `/api/status`
- 每 3 秒刷新 `/api/logs`
- 根据当前模式决定图片组件显示 `/stream.mjpg` 还是最近抓拍图

## 7. 并发模型

项目里至少存在以下线程角色：

- 主线程：模式调度、状态更新、触发处理
- HTTP 监听线程：`accept` 客户端连接
- HTTP 客户端线程：每个连接一个线程
- monitor worker：图像采集和 JPEG 编码

同步手段包括：

- `app_state.lock`：保护共享状态
- `g_mjpeg.lock + cond`：保护最新视频帧缓存
- 驱动内部 `mutex + spinlock + waitqueue`

这使得项目虽然是 C 写的，但并不是“单线程大循环”，而是有比较明确的线程分工。

## 8. 当前实现的优点与边界

## 8.1 优点

- 从硬件到网页形成闭环
- 驱动层和用户态有清晰协议边界
- 触发链路与监控链路分开设计
- 支持模式切换与失败回滚
- 已经具备一定扩展性

## 8.2 当前边界

下面这些内容在代码中还属于“已预留或可优化”，不建议在简历里夸大成“已经完成”：

- 已接入单路 PWM 蜂鸣器，但当前告警策略仍然比较简单，主要是固定时长/固定节奏提醒
- SHT20 阈值字段已保留，但尚未形成阈值告警逻辑
- HTTP 服务是最小实现，没有认证、鉴权和完整协议解析
- 日志接口当前只读取固定大小缓冲区
- 没有自动化测试体系，更多依赖板端实测

## 9. 这套架构最值得你自己真正吃透的点

如果你准备把这个项目写进简历，建议你至少把下面几件事讲到顺：

1. 为什么要做统一的 `sensor_hub` 字符设备，而不是让用户态直接分散访问硬件
2. 为什么 `monitor` 和 `trigger` 需要不同分辨率和不同处理链路
3. PIR 事件、SHT20 快照、摄像头抓拍是如何在一次触发中组合起来的
4. MJPEG 为什么选择“内存缓存 + multipart 推流”
5. 模式切换为什么由主线程完成，而不是 HTTP 请求线程直接改摄像头

如果这五点你能讲明白，这个项目就已经不只是“vibe coding 拼起来的代码”，而是你真正理解过的一套系统。
