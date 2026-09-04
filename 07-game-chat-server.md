# 07 TCP 游戏聊天服务器

## 总体结构

系统由三个角色组成：

- Client：接收用户输入、显示聊天消息并维护心跳。
- LogicServer：管理在线用户、处理业务逻辑并广播消息。
- DataServer：保存聊天记录并提供历史查询。

~~~mermaid
sequenceDiagram
    participant C as Client
    participant L as LogicServer
    participant D as DataServer
    C->>L: 登录/发送消息请求
    L->>D: 查询历史或写入消息
    D-->>L: 数据/写入确认
    L->>L: 执行业务逻辑
    L-->>C: 响应或频道广播
~~~

设计强调“先存储成功，再广播消息”，这样客户端看到的消息原则上已经进入数据层。

## Client

- 从参数或配置读取服务器地址、端口、昵称和频道。
- 连接 LogicServer。
- 发送聊天内容并接收广播。
- 定期发送心跳。
- 保存有限条显示历史。
- 输入 /quit 时优雅退出。

客户端还需要处理重连、重复登录、服务端主动断开和协议错误。

## LogicServer

LogicServer 是连接层和业务层的边界：

- 监听客户端连接。
- 维护 client_id 到会话，以及 channel_id 到成员集合的索引。
- 网络线程解析请求并生成任务。
- 工作线程从阻塞队列取任务，执行登录、进频道、发消息和退出。
- 连接 DataServer，查询历史或提交写入。
- 收到存储确认后向同频道成员广播。
- 清理超时或断开的连接。
- 输出在线数、队列积压和错误统计。

网络线程应尽量只负责收包、拆包和排队，避免被数据库或复杂业务阻塞。

## DataServer

原设计中的 DataServer：

- 接收 LogicServer 的查询和写入请求。
- 网络线程解析请求并放入写任务队列。
- 工作线程执行数据更新。
- 维护每频道最近约 20 条消息的内存历史。
- 追加写入本地日志文件。
- 写入成功后返回 ACK。

后续可以把本地容器和日志文件替换为 Redis 或数据库，但对外协议和职责边界应保持稳定。

## 线程与队列

~~~text
网络线程（生产者）
    ↓ 解析并创建任务
有界阻塞队列
    ↓
工作线程（消费者）
    ↓
业务、存储、响应
~~~

队列必须有容量、关闭语义和过载策略，否则后端变慢时内存会持续增长。

## 设计中需要统一的地方

### 端口

设计文档一处写 LogicServer 监听 8888、DataServer 监听 9999，而后续实现常使用 LogicServer 9999、DataServer 8888。端口只是配置，但必须集中定义并保持一致，不能散落在代码中。

### 协议

cmd:content 便于早期练习，但内容包含冒号或出现半包时很脆弱。正式实现应使用长度前缀配合 JSON/Protobuf，并包含协议版本、请求 ID、消息类型、负载长度和错误码。

### 请求对应关系

LogicServer 到 DataServer 如果允许多个并发请求，不能用“发送后立即阻塞等待下一条响应”的隐含顺序。应使用请求 ID 关联响应，或为每个连接设计严格串行队列。

### 状态所有权

在线用户属于 LogicServer 的运行时状态，聊天记录属于 DataServer 的数据状态。多实例部署后，单机在线表无法代表全局，需要会话路由、共享状态或明确的粘性连接策略。

## 故障处理

- DataServer 断开：重连并对失败请求返回明确错误。
- Client 断开：注销会话并广播离线事件。
- 写入超时：不要把未确认消息当成成功。
- 重试写入：需要请求 ID 或幂等键避免重复消息。
- 慢客户端：限制输出缓冲，超过阈值时断开或降级。
- 服务停止：停止接收、关闭队列、处理剩余任务、回收线程和 fd。



## DataServer 与 LogicServer 参考实现

这两份 PDF 是类、字段、接口和线程流程的结构图，并不包含可以逐行复制的源代码。下面两个文件按照结构图中的职责与命令格式还原为可运行的 C++17 学习实现：

- [DataServer 源码：examples/chat_dataserver.cpp](examples/chat_dataserver.cpp)
- [LogicServer 源码：examples/chat_logicserver.cpp](examples/chat_logicserver.cpp)

DataServer 维护 `ChannelManager -> SingleChannel -> Message` 数据结构，支持：

- `get_channel:<channel_id>`：拉取频道历史消息。
- `send_message:<channel_id>:<sender_id>:<content>`：保存消息。

LogicServer 保存在线客户端与当前频道，向客户端提供：

- `login:<client_id>`
- `join_channel:<channel_id>`
- `send_message:<content>`
- `quit`

结构图原方案是 `epoll + 任务队列 + 工作线程`。这份代码为了突出 LogicServer/DataServer 的职责划分，网络接入层使用 `accept + 工作线程池`；完整 epoll 事件循环可参考 05 章的聊天室服务端。

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread examples/chat_dataserver.cpp -o chat_dataserver
g++ -std=c++17 -O2 -Wall -Wextra -pthread examples/chat_logicserver.cpp -o chat_logicserver
```

按 DataServer、LogicServer、客户端的顺序启动：

```bash
./chat_dataserver 9001
./chat_logicserver 9000 127.0.0.1 9001
nc 127.0.0.1 9000
```

连接后可以依次输入：

```text
login:alice
join_channel:room1
send_message:hello
quit
```

当前 DataServer 使用内存保存最近 100 条频道消息，重启后数据会清空；它适合解释原图中的接口协作，不应当被误认为具备持久化、鉴权、限流和生产级故障恢复。
