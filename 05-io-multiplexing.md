# 05 IO 多路复用：select 与 epoll

## 从阻塞到事件驱动

### 一连接一线程

结构直观，但大量连接会带来线程栈内存、调度和上下文切换成本。连接很多而活跃连接很少时，资源利用率较低。

### 非阻塞轮询

把 socket 设为非阻塞后，单个 recv 不会永久卡住，但如果程序不停遍历所有连接，就会产生大量无效系统调用和 CPU 空转。非阻塞只是基础，不等于高效事件通知。

### IO 多路复用

由内核等待多个文件描述符的就绪事件，应用程序只处理已经就绪的连接。

## select

核心接口和宏包括 fd_set、FD_ZERO、FD_SET、FD_ISSET 和 select。

select 返回后，程序通常仍要遍历 fd 集合判断哪些连接就绪。它有 fd 数量限制，并且每轮都要重建或复制集合，适合入门和连接规模较小的程序。

## epoll

- epoll_create1：创建 epoll 实例。
- epoll_ctl：添加、修改或删除监听项。
- epoll_wait：等待并返回就绪事件列表。

典型服务端循环：

~~~text
创建监听 socket → bind → listen
创建 epoll 实例
把监听 socket 注册到 epoll
循环调用 epoll_wait
  监听 socket 可读：accept 新连接并注册
  客户端 socket 可读：recv 数据
  客户端断开/出错：从 epoll 删除并 close
~~~

## select 与 epoll 对比

| 维度 | select | epoll |
| --- | --- | --- |
| 平台 | POSIX 常见平台 | Linux |
| 返回结果 | 返回后遍历集合 | 直接返回就绪事件 |
| fd 数量 | 受 FD_SETSIZE 限制 | 通常受系统资源限制 |
| 注册方式 | 每轮传递集合 | 使用 epoll_ctl 持久注册 |
| 适用场景 | 教学、小规模连接 | 大量连接、事件驱动服务 |

## 多人聊天室练习

- 新连接：accept、加入 epoll、发送历史记录并要求昵称。
- 首条消息：检查昵称是否重复并完成登录。
- 普通消息：添加时间戳、保存历史并广播给其他客户端。
- /quit 或断线：删除客户端、关闭 fd 并广播离线通知。

客户端主线程发送消息，接收线程持续打印服务端数据。

## 工程化补充

- TCP 没有消息边界，一次 recv 不保证得到一条完整消息。
- send 可能只写出部分数据；非阻塞模式还要处理 EAGAIN。
- epoll 默认是水平触发；使用边缘触发时应循环读到 EAGAIN。
- fd 数字会被复用，异步任务最好携带连接代次或会话对象。
- 慢客户端需要独立发送缓冲和容量上限，不能阻塞整个事件循环。
- 单事件循环中直接维护客户端表通常不需要锁；一旦加入工作线程，就要重新设计状态所有权。



## 完整 epoll 聊天室代码

原 PDF 包含 `server_mt.c` 和 `client_mt.c`，但分页导致部分 `bind`、`accept`、事件注册和清理代码缺失。下面的独立源文件保留聊天室、昵称、最近消息与退出功能，并补全了非阻塞 socket、按行拆包和断开清理：

- [服务端源码：examples/epoll_chat_server.c](examples/epoll_chat_server.c)
- [客户端源码：examples/epoll_chat_client.c](examples/epoll_chat_client.c)

服务端使用 level-triggered `epoll`；监听 socket 和客户端 socket 均设置为非阻塞。每次事件到达后持续读取到 `EAGAIN`，并为每个客户端保存未完成的一行，从而正确处理 TCP 粘包和半包。

```bash
gcc -std=c11 -O2 -Wall -Wextra examples/epoll_chat_server.c -o epoll_chat_server
gcc -std=c11 -O2 -Wall -Wextra -pthread examples/epoll_chat_client.c -o epoll_chat_client
```

启动服务端和两个客户端：

```bash
./epoll_chat_server 8888
./epoll_chat_client 127.0.0.1 8888
./epoll_chat_client 127.0.0.1 8888
```

客户端连接后先输入昵称，再输入聊天内容；输入 `/quit` 退出。新客户端会收到最近 10 条历史消息。
