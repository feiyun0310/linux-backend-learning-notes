# Linux C/C++ 后端学习笔记

这套笔记按照原始《整体计划》的学习顺序，把近期内容整理成一条完整路线：

> 线程与同步 → 并发模型 → 日志组件 → TCP → IO 多路复用 → 游戏聊天服务器

它不是对原文的简单复制，而是把思维导图、设计文档和代码练习重新组织为可复习、可检查、可继续扩展的知识体系。

## 学习路线

| 阶段 | 主题 | 要解决的问题 | 对应笔记 |
| --- | --- | --- | --- |
| 1 | Linux/C++ 工具链 | 能独立编译、调试和组织项目 | [学习路线](01-learning-roadmap.md) |
| 2 | 线程与同步 | 多个执行流如何安全访问共享状态 | [线程与同步](02-threads-and-synchronization.md) |
| 3 | 并发练习 | 如何把锁、条件变量和阻塞队列用于实际问题 | [并发练习](03-concurrency-practices.md) |
| 4 | 日志组件 | 如何让慢速磁盘 IO 不阻塞业务线程 | [日志系统](04-logging-system.md) |
| 5 | IO 多路复用 | 单个线程如何管理大量连接 | [select 与 epoll](05-io-multiplexing.md) |
| 6 | TCP | 如何建立可靠的客户端/服务端通信 | [TCP 编程](06-tcp-programming.md) |
| 7 | 综合项目 | 如何把前面的知识组合成聊天服务器 | [游戏聊天服务器](07-game-chat-server.md) |
| 8 | 工程化 | 如何从“能运行”推进到“可维护、可验证” | [复盘与下一步](08-review-and-next-steps.md) |

## 知识之间的关系

~~~mermaid
flowchart LR
    A[线程生命周期] --> B[互斥锁/条件变量/信号量]
    B --> C[有界阻塞队列]
    C --> D[生产者-消费者]
    D --> E[异步日志]
    A --> F[TCP Socket]
    F --> G[select/epoll]
    D --> H[网络线程与工作线程分离]
    G --> H
    E --> I[可观测性]
    H --> J[LogicServer/DataServer]
    I --> J
~~~

## 最重要的五个认识

1. 并发程序的第一目标是正确性，然后才是吞吐量。
2. 队列不仅传递数据，也定义背压、容量上限和停止语义。
3. TCP 是字节流，必须由应用层协议解决粘包、拆包和请求对应关系。
4. epoll 只告诉程序“哪些 fd 已就绪”，业务逻辑仍应与网络收发解耦。
5. 服务端不能只有正常流程，还必须设计断线、超时、重连、资源回收和监控。

## 相关项目

完整的 TCP、Protobuf、Redis 聊天服务器源码见：

- [tcp-game-chat-redis](https://github.com/feiyun0310/tcp-game-chat-redis)

## 说明

- 本仓库只整理学习总结，不上传原始 PDF。
- 原资料中的少量概念错误和代码风险已在对应章节中标出。
- 示例面向 Linux，主要使用 C/C++、pthread、Socket、select/epoll。
