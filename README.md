# 含自定义应用层协议的 TCP 通信组件（服务端 / 客户端）

基于 **Qt 5.12.9 + C++11** 实现的 TCP 双端通信程序，包含服务端与客户端。
项目核心是**从零设计的一套应用层帧协议**，用于解决 TCP 面向字节流时必然出现的
**粘包 / 拆包**问题，并配套 **11 组单元测试**与**真实链路端到端测试**验证协议正确性。

> 这不是一个"能收发文字"的聊天 demo。真正的工作量在协议层：
> TCP 不保留消息边界，接收端一次 `readAll()` 可能读到半帧，也可能读到三帧粘在一起。
> 本项目用有状态拆包器解决它，并用 47 项断言 + 230 条消息端到端测试证明它是对的。

---

## 目录

- [功能特性](#功能特性)
- [为什么需要自定义协议](#为什么需要自定义协议)
- [协议设计](#协议设计)
- [项目结构](#项目结构)
- [编译与运行](#编译与运行)
- [测试](#测试)
- [关键实现说明](#关键实现说明)
- [已知限制](#已知限制)
- [环境](#环境)

---

## 功能特性

**服务端**
- 多客户端并发接入与管理，在线客户端列表动态刷新
- 按端口**定向单播** / 向所有在线客户端**群发**
- 显示客户端 IP、端口与接入 / 断开时间（日志分色显示）
- 停止监听前先向所有客户端广播系统通知，并 `flush()` + `waitForBytesWritten()` 确保送达
- 可主动断开指定的单个客户端（用 `disconnectFromHost()` 让排队数据先发完）
- 自定义 `MyComboBox` 重写 `mousePressEvent`，通过自定义信号实现下拉框展开即刷新

**客户端**
- 连接超时管理（5 秒），超时后正确回收 socket 并恢复 UI 可重试状态
- 心跳保活（10 秒 Ping，服务端回 Pong）
- 可指定本地源端口（`bind` 于 `connectToHost` 之前）
- 按错误类型给出针对性提示（主机不可达 / 连接被拒绝 / 对端主动关闭等）
- 连接耗时统计（`QElapsedTimer`）

**共同**
- 统一的应用层帧协议（8 字节帧头 + 变长正文）
- 粘包 / 拆包正确处理
- 脏数据自动重新同步（`resync`）
- 4 类消息类型：文本、心跳请求、心跳应答、系统通知

---

## 为什么需要自定义协议

TCP 是**面向字节流**的协议：它只保证字节按序到达，**不保留应用层的消息边界**。
因此接收端必然遇到两种现象：

```
发送方 write 3 次:  [消息A][消息B][消息C]

接收方一次 readyRead 可能拿到:
  · [消息A][消息B][消息C]        <- 粘包：3 条粘在一起
  · [消息A]                       <- 正常
  · [消息A][消息B的]              <- 拆包：一半
```

所以**绝不能把"一次 `readAll()` 的结果"当成"一条消息"**。
必须在应用层自己定义帧格式，让接收端能按帧边界切分。这就是本项目的协议层。

---

## 协议设计

### 帧格式（大端序 / 网络字节序）

```
 偏移:  0        2        3        4                        8            8+N
        +--------+--------+--------+------------------------+------------+
        | Magic  |  Ver   |  Type  |      BodyLength        |    Body    |
        | 2 Byte | 1 Byte | 1 Byte |       4 Byte           |   N Byte   |
        +--------+--------+--------+------------------------+------------+
        |<----------------- 帧头 Header 8 字节 ---------------->|
```

| 字段 | 长度 | 值 / 说明 |
|---|---|---|
| `Magic` | 2 字节 | 固定 `0xAA55`。作用：① 识别帧起始位置；② 字节流错位时靠它重新同步 |
| `Ver` | 1 字节 | 协议版本 `0x01`，为将来不兼容升级预留 |
| `Type` | 1 字节 | 消息类型，见下表 |
| `BodyLength` | 4 字节 | 正文字节数（不含帧头），大端序，上限 1 MB |
| `Body` | N 字节 | 正文，UTF-8 文本或任意二进制 |

### 消息类型

| 值 | 名称 | 方向 | 说明 |
|---|---|---|---|
| `0x01` | `MsgText` | 双向 | 普通文本消息 |
| `0x02` | `MsgPing` | 客户端 → 服务端 | 心跳请求 |
| `0x03` | `MsgPong` | 服务端 → 客户端 | 心跳应答 |
| `0x04` | `MsgSystem` | 服务端 → 客户端 | 系统通知（如"服务端即将停止监听"） |

### 常量定义

```cpp
const quint16 kMagic       = 0xAA55;          // 固定帧头
const quint8  kVersion     = 0x01;            // 协议版本
const int     kHeaderSize  = 8;               // 帧头长度 (2+1+1+4)
const quint32 kMaxBodySize = 1024u * 1024u;   // 单帧正文上限 1MB，防御性上限
```

### 主要接口

```cpp
namespace NetProto {

// 打包
QByteArray encode(quint8 type, const QByteArray &body);
QByteArray encodeText(const QString &text);
QByteArray encodePing();
QByteArray encodePong();
QByteArray encodeSystem(const QString &text);

struct Message {
    quint8     type;
    QByteArray body;
    QString text() const;      // 正文按 UTF-8 转字符串
};

// 拆包器：每个连接各持有一个
class MessageParser {
public:
    struct Result {
        QList<Message> messages;      // 本次解析出的完整消息（可能 0 条）
        bool           protocolError; // 是否发生协议错误（已自动丢弃脏数据）
        QString        errorText;
    };
    void   reset();                         // 新连接必须调用，清空残包
    Result append(const QByteArray &chunk); // 喂入新字节流
    int    bufferedBytes() const;           // 缓冲区内未凑成一帧的字节数
private:
    void resync();                          // 帧头校验失败时重新同步
    QByteArray m_buffer;
};

}
```

### 使用方式

**每个 TCP 连接必须持有独立的 `MessageParser`**，否则不同连接的残包会互相串扰。

```cpp
// 接收端
void onReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket*>(sender());
    const QByteArray chunk = sock->readAll();

    // 注意这里：不是把 chunk 当成一条消息，而是喂给拆包器
    MessageParser::Result r = m_parsers[sock].append(chunk);

    if (r.protocolError)
        qWarning() << "协议错误:" << r.errorText;

    for (const Message &msg : r.messages)
        handleMessage(sock, msg);     // 逐条处理完整消息
}

// 发送端
QByteArray frame = NetProto::encode(NetProto::MsgText, text.toUtf8());
sock->write(frame);      // 永远发完整帧
```

---

## 项目结构

```
.
├── common/                     # 服务端与客户端共享的协议模块
│   ├── protocol.h              # 协议定义、帧格式说明、MessageParser 声明
│   ├── protocol.cpp            # 编解码 + 拆包器实现
│   ├── protocol.pri            # qmake 包含文件，供两端引用
│   ├── test_protocol.cpp       # 单元测试（11 组 47 项断言）
│   ├── test_e2e_tcp.cpp        # 真实 TCP 链路端到端测试
│   ├── test_run.log            # 单元测试运行结果
│   └── e2e_run.log             # 端到端测试运行结果
├── server/                     # 服务端
│   ├── widget.h / widget.cpp   # 主窗口逻辑（多连接管理、收发）
│   ├── mycombobox.h / .cpp     # 自定义下拉框（展开时触发刷新）
│   ├── widget.ui
│   └── 109-TcpServer.pro
└── client/                     # 客户端
    ├── widget.h / widget.cpp   # 主窗口逻辑（连接、超时、心跳）
    ├── widget.ui
    └── 120-TCPClient.pro
```

**协议代码复用**：服务端与客户端通过 `protocol.pri` 引用**同一份源码**，
从工程结构上保证两端帧格式实现不会写歪。

```qmake
# 在 server/109-TcpServer.pro 与 client/120-TCPClient.pro 中
include(../common/protocol.pri)   # $$PWD 是绝对路径，不受影子构建目录影响
```

---

## 编译与运行

### 环境要求

| 项 | 版本 |
|---|---|
| Qt | 5.12.9（MinGW 7.3 32-bit） |
| 编译器 | MinGW g++（支持 C++11） |
| 构建工具 | qmake |
| 依赖模块 | `core` `gui` `widgets` `network` |

### 用 Qt Creator（推荐）

1. 用 Qt Creator 打开 `server/109-TcpServer.pro`
2. 选择套件 `Desktop Qt 5.12.9 MinGW 32-bit`
3. 构建并运行 → 得到服务端
4. 同样打开 `client/120-TCPClient.pro` 构建运行 → 得到客户端

### 用命令行编译测试程序

两个测试程序不依赖 GUI，可单独编译运行。以单元测试为例（PowerShell）：

```powershell
$qt  = 'D:\Qt\Qt5.12.9\5.12.9\mingw73_32'
$gxx = 'D:\Qt\Qt5.12.9\Tools\mingw730_32\bin\g++.exe'

Push-Location common

& $gxx -std=c++11 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qt\include" "-I$qt\include\QtCore" "." `
    test_protocol.cpp protocol.cpp "-L$qt\lib" -lQt5Core `
    -o test_protocol.exe

$env:PATH = "$qt\bin;$env:PATH"
.\test_protocol.exe

Pop-Location
```

端到端测试额外需要 QtNetwork：

```powershell
Push-Location common

& $gxx -std=c++11 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtNetwork" "." `
    test_e2e_tcp.cpp protocol.cpp "-L$qt\lib" -lQt5Network -lQt5Core `
    -o test_e2e_tcp.exe

.\test_e2e_tcp.exe

Pop-Location
```

### 联调步骤

1. 启动服务端，地址选 `127.0.0.1` 或本机 IP，端口填 `8888`，点「监听」
2. 启动客户端，服务端 IP 填 `127.0.0.1`，端口填 `8888`，点「连接」
3. 服务端会显示 `[127.0.0.1:xxxxx][新客户端接入]`
4. 双向发送文本即可；客户端下拉框选具体端口为点对点，选 `all` 为群发
5. 客户端每 10 秒自动发心跳，服务端回 Pong

---

## 测试

### 单元测试（`common/test_protocol.cpp`）

**不依赖 GUI、不依赖真实网络**的纯逻辑测试。把精心构造的"字节流片段"直接喂给
`MessageParser`，检查它是否总能还原出正确的消息边界。

运行结果：**11 组测试，47 项断言，全部通过**

| # | 测试项 | 验证内容 |
|---|---|---|
| 1 | 单帧基准 | 一条完整消息能正确解出，缓冲区清空 |
| 2 | 粘包 | 3 条消息挤在一次 `append` 里，全部解出且顺序正确 |
| 3 | 拆包 | 1 条消息切成 4 段到达（3/2/5/剩余字节），前 3 段不出消息，最终完整还原（含中文） |
| 4 | 逐字节投喂 | 20 条消息每次只喂 1 字节（最极端拆包），全部还原且无误报 |
| 5 | 脏数据重同步 | 帧头前混入 5 字节垃圾（不含 `0xAA55`），报协议错误后仍正确解出后续 2 条 |
| 6 | 长度字段伪造 | 把长度改成 `0x7FFFFFFF`，识别超限、不崩溃、丢弃后继续解析 |
| 7 | 空正文 + 类型保真 | 4 种类型（含 0 字节正文）全部解出，类型字段无损 |
| 8 | 混合压力 | 50 条消息 + 伪随机切片（1~37 字节）投喂，内容与顺序完全一致 |
| 9 | 多连接隔离 | 两个拆包器互不干扰，A 的残包不影响 B |
| 10 | `reset` 与空数据 | `reset()` 后残包清空，空数据不产生消息也不报错 |
| 11 | 帧格式字节级校验 | 逐字节核对 `AA 55 / 01 / 01 / 00 00 00 02 / "AB"` |

完整日志见 [`common/test_run.log`](common/test_run.log)。

### 端到端测试（`common/test_e2e_tcp.cpp`）

在**真实 TCP 链路**（`127.0.0.1`）上验证。刻意制造两种极端：

- **阶段 1「突发」**：200 帧拼成一大块，一次 `write` 发出 → 必然粘包
- **阶段 2「连发」**：一帧一个 `write`，连发 20 帧 → 真实场景
- **阶段 3「截断」**：每帧切成前后两半，中间隔 20ms 发出 → 必然拆包

运行结果：

```
期望消息数            : 230
实际还原消息数        : 230
内容与顺序            : 完全一致
客户端 write 次数     : 41
服务端 readyRead 次数 : 41   （远小于 230，证明发生了粘包且被正确处理）
协议错误次数          : 0
结论：真实 TCP 链路上 100% 还原了消息边界
```

> **41 次 `write` 对 230 条消息**是这套协议有效的直接证据：
> 接收端只触发 41 次 `readyRead`，却正确切分出了 230 条独立消息。

完整日志见 [`common/e2e_run.log`](common/e2e_run.log)。

---

## 关键实现说明

### 1. 拆包器的核心循环

```cpp
MessageParser::Result MessageParser::append(const QByteArray &chunk)
{
    Result result;
    if (!chunk.isEmpty())
        m_buffer.append(chunk);          // 新数据接在残包后面

    for (;;)
    {
        // (1) 连帧头都不够 —— 半包，保留缓冲区等下一批数据
        //     这是"拆包"能被正确还原的关键：绝不能把不足一帧的数据丢掉
        if (m_buffer.size() < kHeaderSize)
            break;

        // (2) 校验魔数，不匹配说明字节流错位，丢脏数据重新同步
        if (readU16BE(m_buffer.constData()) != kMagic) {
            result.protocolError = true;
            resync();
            continue;
        }

        const char   *p       = m_buffer.constData();
        const quint8  type    = quint8(p[3]);
        const quint32 bodyLen = readU32BE(p + 4);

        // (3) 长度超限：不可能是合法帧，按脏数据处理（防内存被撑爆）
        if (bodyLen > kMaxBodySize) {
            result.protocolError = true;
            m_buffer.remove(0, 1);       // 只丢 1 字节，下一轮继续找帧头
            continue;
        }

        // (4) 正文没收全 —— 依然是半包，保留等待
        if (quint32(m_buffer.size()) < quint32(kHeaderSize) + bodyLen)
            break;

        // (5) 凑齐一帧，切出来交给上层
        result.messages.append(Message(type, m_buffer.mid(kHeaderSize, int(bodyLen))));
        m_buffer.remove(0, kHeaderSize + int(bodyLen));

        // 注意不 break：缓冲区里可能还"粘"着下一帧甚至下下帧
    }
    return result;
}
```

**同一段循环同时解决了粘包（一次解出多条）与拆包（残包保留待补）。**

### 2. 脏数据重新同步（`resync`）

```cpp
void MessageParser::resync()
{
    // 从第 2 个字节开始找下一个魔数（跳过当前位置，避免原地死循环）
    const int idx = m_buffer.indexOf(QByteArray(kMagicBytes, 2), 1);

    if (idx < 0) {
        // 整段都没有帧头。只保留最后 1 个字节：
        // 它可能是被 TCP 从中间切断的帧头首字节 0xAA，下一批数据会补上 0x55
        m_buffer = m_buffer.right(1);
        if (!m_buffer.isEmpty() && quint8(m_buffer.at(0)) != 0xAA)
            m_buffer.clear();
    } else {
        m_buffer.remove(0, idx);    // 丢掉错位字节，让缓冲区正好从新帧头开始
    }
}
```

### 3. 每个连接独立的拆包器

拆包器是**有状态**的（内部维护残包缓冲区），所以**必须按连接隔离**：

```cpp
// 服务端：key 就是 socket 指针
QHash<QTcpSocket*, NetProto::MessageParser> m_parsers;

// 新连接接入时建立干净的拆包器
m_parsers.insert(client, NetProto::MessageParser());

// socket 销毁时顺手清掉，避免 map 里留下悬空 key
connect(client, &QObject::destroyed, this, [this, client]() {
    m_parsers.remove(client);
});
```

如果所有连接共用一个拆包器，A 的残包会串到 B 的消息里去。

### 4. 连接超时中的对象生命周期处理（客户端）

这是本项目最容易踩坑的地方：

```cpp
void Widget::onConnTimeout()
{
    if (client && client->state() == QAbstractSocket::ConnectingState)
    {
        QTcpSocket *sock = client;
        client = nullptr;    // ★ 先摘掉成员，避免回调访问到正在销毁的 socket

        // ★ 再摘掉这个 socket 到本窗口的全部信号连接：
        //   abort() 有可能触发 error() / disconnected()，
        //   而界面状态本函数已经处理完了，不需要（也不应该）再走一遍错误提示。
        disconnect(sock, nullptr, this, nullptr);

        sock->abort();       // 强制断开（立刻终止，不同于 close）
        sock->deleteLater();
        setUiConnected(false);
    }
}
```

**顺序很关键**：先摘成员指针 → 再摘信号连接 → 最后 `abort()` + `deleteLater()`。
否则 `abort()` 触发的回调可能访问到已销毁对象，直接崩溃。

### 5. 优雅关闭

```cpp
// 停止监听前，先广播通知，并确保数据真正写出去
sendToAll(NetProto::MsgSystem, QStringLiteral("服务端即将停止监听，连接将被断开").toUtf8());

for (QTcpSocket *tmp : tcpSocketClients) {
    if (tmp->state() == QAbstractSocket::ConnectedState) {
        tmp->flush();
        // 关键：不等数据真正写出去就 close()，通知会烂在发送缓冲区里
        tmp->waitForBytesWritten(200);
    }
}
```

### 6. 为什么本层不加 CRC

TCP 头部已有 16 位校验和，且本协议跑在同一台机器或局域网上，
应用层再算一遍 CRC 属于重复劳动。若将来要跑在不可靠信道上或需要防篡改，
在帧头追加 CRC32 字段并升 `Ver` 即可——现有设计已经为此预留了版本位。

---

## 已知限制

诚实列出，避免误导：

- **仅实现了 TCP**。UI 的"通信协议"下拉框里有 `UTP` 选项，但当前版本未实现 UDP，选中无效。
  协议层本身与传输无关，若要做 UDP 只需换掉 socket 与连接管理部分。
- **未做应用层 CRC 校验**（理由见上）。
- **无消息持久化**：收到的消息只显示在界面上，未落盘。
- **无自动重连**：客户端断开后需手动重新连接。
- **协议握手未做**：当前通过固定 Magic 识别，未做版本协商。`Ver` 字段已预留。
- **服务端未限制客户端数量**：如用于公网场景需增加连接数上限与限流。

**后续可做的方向**：UDP 支持、消息持久化与导出、自动重连与心跳超时判定、
协议版本协商握手、帧头追加 CRC32、消息加解密。

---

## 环境

- **Qt**：5.12.9（MinGW 7.3 32-bit）
- **语言标准**：C++11
- **平台**：Windows 10（代码本身跨平台，仅依赖 Qt）
- **IDE**：Qt Creator 4.12.2

---

## License

个人学习项目，可自由参考。
