# 项目文档索引

这组文档的目标不是重复 `readme.md`，而是把这个项目拆开讲清楚，方便你：

- 系统化理解代码和运行链路
- 提炼可以写进简历的内容
- 为后续演示、答辩、面试做准备

如果你是第一次完整阅读这个项目，推荐顺序如下：

1. 先看 [01-project-overview.md](./01-project-overview.md)
2. 再看 [02-architecture-deep-dive.md](./02-architecture-deep-dive.md)
3. 然后看 [03-api-and-operations.md](./03-api-and-operations.md)
4. 最后看 [04-resume-and-interview-guide.md](./04-resume-and-interview-guide.md)
5. 想回到源码逐个吃透时，再看 [05-code-reading-roadmap.md](./05-code-reading-roadmap.md)

## 文档说明

### [01-project-overview.md](./01-project-overview.md)

项目总览。回答“这个项目是干什么的”“为什么要分成双模式”“整体数据怎么流动”。

### [02-architecture-deep-dive.md](./02-architecture-deep-dive.md)

架构深挖。重点解释内核驱动、用户态守护进程、HTTP 服务、Web 控制台，以及模块之间的通信关系。

### [03-api-and-operations.md](./03-api-and-operations.md)

接口与运维说明。包含构建、运行、调试、接口说明、目录输出、排障建议。

### [04-resume-and-interview-guide.md](./04-resume-and-interview-guide.md)

简历与面试指南。把当前实现转化成适合简历表述、项目介绍、技术亮点和面试问答素材。

### [05-code-reading-roadmap.md](./05-code-reading-roadmap.md)

代码阅读地图。告诉你应该按什么顺序看源码，每个文件重点看什么，以及读完之后应该能回答哪些问题。

## 一句话总结

这是一个运行在 ROCK 3A 上的嵌入式智能监控终端项目，内核态通过自定义 `sensor_hub` 驱动统一接入 PIR、SHT20 与 PWM 蜂鸣器，抽象出统一输入/输出端点；用户态守护进程同时负责 V4L2 摄像头采集、模式切换、事件提醒抓拍、日志记录和 HTTP 服务，浏览器侧则通过 Web 控制台查看状态、实时画面与抓拍结果。
