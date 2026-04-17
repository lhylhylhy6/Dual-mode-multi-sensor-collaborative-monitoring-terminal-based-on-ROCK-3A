# 代码阅读地图

## 1. 这份文档是干什么的

前面的文档已经把项目“讲明白”了，这一份文档的目标是帮助你把理解重新落回源码。

也就是说，它回答的是：

- 先看哪个文件最容易建立全局认知
- 每个文件里的重点结构和重点函数是什么
- 读完一轮后，你应该能回答哪些问题

## 2. 推荐阅读顺序

如果你准备认真把这个项目吃透，建议按下面顺序读：

1. `readme.md`
2. `kernel/sensor_hub/sensor_hub_ioctl.h`
3. `kernel/sensor_hub/sensor_hub.h`
4. `kernel/sensor_hub/sensor_hub_core.c`
5. `kernel/sensor_hub/sensor_hub_pir.c`
6. `kernel/sensor_hub/sensor_hub_sht20.c`
7. `kernel/sensor_hub/sensor_hub_buzzer.c`
8. `user/daemon/include/*.h`
9. `user/daemon/sensor_client.c`
10. `user/daemon/camera_v4l2.c`
11. `user/daemon/app_state.c`
12. `user/daemon/http_server.c`
13. `user/daemon/main.c`
14. `user/daemon/hub_test.c`
15. `user/web/index.html`

这个顺序的原则是：

- 先看协议，再看实现
- 先看底层边界，再看业务编排
- 先理解组件职责，再看主流程

## 3. 第一轮阅读：先建立协议意识

## 3.1 `sensor_hub_ioctl.h`

这是整个项目最重要的共享协议头。

你读这一份文件时，不要急着看代码细节，先回答下面几个问题：

1. 驱动向用户态暴露了哪些命令：`read / poll / ioctl`
2. 什么是 `sh_event`：`read/poll` 返回的事件，当前主要由 PIR 和蜂鸣器输出状态变化产生
3. 什么是 `sh_snapshot`：所有端点的最新值，统一保存在结构体里，通过 `ioctl` 返回给用户态
4. 为什么传感器值要用定点整数：内核共享 ABI 更稳定，也能避免浮点和精度陷阱
5. 现在已有哪些 `sensor id`：当前已经落地的是 `PIR / SHT20 / BUZZER`，另外还预留了 `LIGHT / SMOKE / DOOR` 等扩展 ID。

重点关注这些结构体：

- `struct sh_sensor_value`
- `struct sh_snapshot`
- `struct sh_event`
- `struct sh_sensor_cfg`
- `struct sh_hub_info`

读完这一份头文件后，你应该能用自己的话说出：

> 这个系统是怎么把“端点状态”“事件流”和“动作接口”分别建模的。

## 3.2 `sensor_hub.h`

这一份文件是驱动内部头文件，重点是理解驱动怎么组织自己。

重点看：

- `struct sh_endpoint_ops`
- `struct sh_endpoint`
- `struct sh_event_queue`
- `struct sh_core`

你要理解的关键点是：

- 一个端点在驱动里是怎样被抽象的：通过 `sh_endpoint` 抽象，包含 ID、类型、方向、能力、配置、值、操作等参数。
- `core` 为什么既要有 `lock` 又要有事件队列：前者保护注册表、配置和 snapshot，后者承担异步事件投递
- PIR、SHT20 和 BUZZER 为什么都能复用同一套框架

## 4. 第二轮阅读：先吃透内核驱动

## 4.1 `sensor_hub_core.c`

这个文件建议分四段看：

### 第一段：事件队列

重点函数：

- `sh_q_pop()`
- `sh_push_event()`

你要理解：

- 为什么这是一个环形队列
- 队列满了之后为什么丢最旧事件
- `wake_up_interruptible()` 在这里的作用是什么

### 第二段：传感器注册与快照

重点函数：

- `sh_find_sensor()`
- `sh_register_sensor()`
- `sh_update_sensor_value()`
- `sh_fill_snapshot()`

你要理解：

- 为什么驱动要维护一个 `registered sensor` 数组
- snapshot 是怎么拼出来的
- `snapshot_seq` 为什么每次更新值都要递增

### 第三段：字符设备接口

重点函数：

- `sh_open()`
- `sh_read()`
- `sh_poll()`
- `sh_ioctl()`

你要理解：

- `read()` 为什么要求长度必须是 `struct sh_event` 的整数倍
- `poll()` 和 `read()` 是怎样配合的
- `GET_INFO / GET_SNAPSHOT / FORCE_REFRESH` 各自走了什么路径

### 第四段：平台驱动生命周期

重点函数：

- `sh_probe()`
- `sh_remove()`

你要理解：

- 为什么先注册 `miscdevice` 再注册子传感器
- 为什么 `remove()` 里还会遍历所有 sensor 调 `remove`

## 4.2 `sensor_hub_pir.c`

这份文件建议从“事件是怎么来的”这个角度看。

重点函数：

- `sh_pir_irq_thread()`
- `sh_pir_refresh()`
- `sh_pir_apply_cfg()`
- `sh_pir_register()`

阅读时重点问自己：

- PIR 事件从硬件到用户态经历了哪些步骤
- 去抖动逻辑在哪里做的
- 为什么 rising/falling 都会申请 IRQ，但用户态只关心高电平触发

如果你能把 `sh_pir_irq_thread()` 讲清楚，说明你已经真正理解了触发链路的起点。

## 4.3 `sensor_hub_sht20.c`

这份文件建议从“状态是怎么刷新的”这个角度看。

重点函数：

- `sh_sht20_do_sample()`
- `sh_sht20_refresh()`
- `sh_sht20_workfn()`
- `sh_sht20_apply_cfg()`
- `sh_sht20_register()`

阅读时重点问自己：

- 为什么初始化时既要“马上 refresh 一次”，又要“启动 delayed_work”
- `FORCE_REFRESH` 和周期采样分别适用于什么场景
- 为什么当前 SHT20 只更新 snapshot，不往事件队列里推 sample 事件

## 4.4 `sensor_hub_buzzer.c`

这份文件建议从“输出端点是怎么被统一纳入框架的”这个角度看。

重点函数：

- `sh_buzzer_apply_cfg()`
- `sh_buzzer_dispatch()`
- `sh_buzzer_stop_workfn()`
- `sh_buzzer_register()`

阅读时重点问自己：

- `RUN_ACTION` 是怎么一路落到 PWM 配置上的
- 为什么输出端点也要维护 snapshot
- `ALERT/PULSE/STOP` 的事件语义是什么

## 5. 第三轮阅读：吃透用户态基础模块

## 5.1 `user/daemon/include/*.h`

先看头文件的好处是，能快速知道每个 C 文件对外暴露了什么。

重点包括：

- `camera_v4l2.h`
- `sensor_client.h`
- `app_state.h`
- `http_server.h`

你要回答：

- 哪些模块是纯封装层
- 哪些模块是状态中心
- 哪些模块负责线程和服务

## 5.2 `sensor_client.c`

这份文件比较简单，但非常关键，因为它把用户态与驱动的交互抽象干净了。

重点函数：

- `sensor_client_open()`
- `sensor_client_get_snapshot()`
- `sensor_client_force_refresh()`
- `sensor_client_wait_readable()`
- `sensor_client_read_events()`
- `sensor_client_find_value()`

读完后你应该能说清：

- 用户态触发模式为什么是 `poll + read`
- monitor 模式为什么主要用 `GET_SNAPSHOT`

## 5.3 `camera_v4l2.c`

这是用户态里最底层、也最值得啃的一份文件。

建议按下面的逻辑顺序读：

1. `camera_init()`
2. `camera_set_format()`
3. `camera_reqbufs_and_mmap()`
4. `camera_start()`
5. `camera_dequeue()`
6. `camera_queue()`
7. `camera_capture_one()`
8. `camera_stop()`
9. `camera_deinit()`

你要真正看懂下面这些概念：

- 为什么先 `REQBUFS` 再 `QUERYBUF` 再 `mmap`
- 为什么连续采集是 `DQBUF -> 处理 -> QBUF`
- 为什么 `camera_capture_one()` 要跳过前几帧

如果这一份文件你能吃透，后面面试聊 V4L2 时会更有底气。

## 5.4 `app_state.c`

这份文件代码很短，但它承载了整个系统的状态边界。

重点函数：

- `app_state_set_mode()`
- `app_state_get_mode()`
- `app_state_update_snapshot()`
- `app_state_update_capture()`
- `app_state_copy()`

阅读时思考：

- 为什么 HTTP 层不直接读底层硬件，而要读 `app_state`
- 为什么模式切换不是 HTTP 线程直接做，而是主线程通过状态变化感知

## 5.5 `http_server.c`

建议把它拆成三块读：

### 第一块：基础 HTTP 响应

重点函数：

- `send_all()`
- `send_response_header()`
- `send_json()`
- `send_file_path()`

### 第二块：业务接口

重点函数：

- `handle_status()`
- `handle_mode()`
- `handle_logs()`
- `handle_mjpeg_stream()`
- `handle_client()`

### 第三块：服务线程与缓存

重点函数：

- `http_thread_main()`
- `http_server_publish_jpeg()`
- `http_server_clear_mjpeg()`
- `http_server_start()`
- `http_server_stop()`

你要理解的核心问题是：

- HTTP 服务为什么不直接采集摄像头，而是等 monitor worker 发布 JPEG
- MJPEG 客户端为什么要等待 `seq` 变化
- 为什么状态接口和视频接口分开

## 6. 第四轮阅读：最后再看主流程

## 6.1 `main.c`

很多人一上来就看 `main.c`，其实不太容易看懂。更好的方式是前面都铺垫完，再回来看它。

建议重点盯住这些函数：

- `apply_default_cfg()`
- `handle_trigger()`
- `camera_open_for_mode()`
- `camera_reopen_with_retry()`
- `nv12_to_i420()`
- `encode_nv12_frame_to_jpeg()`
- `monitor_thread_main()`
- `start_monitor_worker()`
- `stop_monitor_worker()`
- `main()`

## 6.2 阅读 `main()` 时的正确姿势

不要从第一行一路机械往下看，而是带着下面四个问题去读：

1. 程序启动时依次初始化了哪些资源
2. 模式切换时哪些资源会被关闭和重建
3. `monitor` 模式和 `trigger` 模式的主循环分别在做什么
4. 程序退出时如何清理资源

## 6.3 最值得讲的三段逻辑

### 第一段：`handle_trigger()`

这就是事件模式最核心的业务闭环：

```text
RUN_ACTION(BUZZER, ALERT)
  -> FORCE_REFRESH(SHT20)
  -> GET_SNAPSHOT
  -> camera_capture_one()
  -> ffmpeg 转 JPG
  -> 更新状态
  -> 追加日志
```

### 第二段：monitor worker

这里体现的是“高频视频处理”和“主线程控制逻辑”分离：

```text
camera_dequeue()
  -> NV12 转 I420
  -> TurboJPEG 压缩
  -> publish_jpeg()
  -> camera_queue()
```

### 第三段：模式切换与失败回滚

这里体现的是工程稳定性意识：

```text
发现模式变化
  -> 停旧 worker
  -> 关 camera
  -> 重新打开新模式 camera
  -> 必要时启动新 worker
  -> 失败则尝试回滚
```

## 7. 最后一轮：看辅助文件

## 7.1 `hub_test.c`

它的价值不只是测试，还能帮你反向理解协议怎么用。

建议你带着这个问题看：

- 如果我不用整个 `monitor_daemon`，怎样最小化地访问 `/dev/sensor_hub`

## 7.2 `user/web/index.html`

这份文件主要帮助你理解前端如何消费后端接口。

重点看：

- `refreshStatus()`
- `refreshLogs()`
- `setMode()`
- `updatePreviewByMode()`

你要理解：

- 前端是怎么根据 `mode` 决定显示实时流还是静态图片的
- 为什么状态和日志使用轮询

## 8. 阅读完成后的自测题

如果你读完一轮源码，建议你尝试不看文档回答下面这些问题：

1. `/dev/sensor_hub` 为什么同时提供 `event` 和 `snapshot`
2. PIR 触发后，SHT20 数据和蜂鸣器状态是怎么和抓拍结果绑定到一起的
3. monitor 模式为什么不直接把 NV12 文件落盘给网页读
4. 为什么模式切换必须重开摄像头
5. `FORCE_REFRESH(SHT20)`、蜂鸣器动作以及周期采样分别解决什么问题
6. 如果以后新增一个烟雾传感器，大概应该接到哪一层

如果这些问题你能顺畅回答，说明你已经从“知道代码在哪”进入到“真正理解系统设计”了。

## 9. 建议你亲手做的三个小练习

为了把理解变成自己的东西，建议你后续做这三个小练习：

1. 画一张自己的系统时序图，覆盖 PIR 触发到 JPG 落盘的全过程
2. 自己手写一版 `/api/status` 的字段说明，不参考现成文档
3. 尝试给项目加一个新传感器占位设计，只写协议和框架，不必真的接硬件

这三个练习非常适合准备面试。
