#ifndef PROTOCOL_H
#define PROTOCOL_H

/**
 * 自定义应用层协议 —— 编解码 + 粘包/拆包处理
 * ============================================================================
 *
 * 【帧格式】大端序 Big-Endian，8 字节帧头 + N 字节正文
 *
 *   偏移:  0        2        3        4                        8            8+N
 *          +--------+--------+--------+------------------------+------------+
 *          | Magic  |  Ver   |  Type  |      BodyLength        |    Body    |
 *          | 2 Byte | 1 Byte | 1 Byte |       4 Byte           |   N Byte   |
 *          +--------+--------+--------+------------------------+------------+
 *          |<----------------- 帧头 Header 8 字节 ------------->|
 *
 *   Magic      = 0xAA55  固定帧头。作用：① 识别帧的起始位置；
 *                         ② 字节流一旦错位，靠它重新同步（resync）
 *   Ver        = 0x01    协议版本，为将来不兼容升级预留
 *   Type       = 消息类型，见 MsgType（文本 / 心跳请求 / 心跳应答 / 系统通知）
 *   BodyLength = 正文字节数（不含帧头），大端序，上限 kMaxBodySize
 *   Body       = 正文，UTF-8 文本或任意二进制
 *
 * 【为什么需要这套协议】
 *   TCP 是「面向字节流」的，它只保证字节按序到达，不保留应用层的消息边界。
 *   于是接收端会遇到两种必然现象：
 *     · 粘包：发送方 write 了 3 条消息，接收方一次 readyRead 就读到全部 3 条；
 *     · 拆包：发送方 write 了 1 条消息，接收方分多次 readyRead 才收全。
 *   所以接收端不能把"一次 readAll 的结果"当成"一条消息"，必须自己维护
 *   缓冲区，按帧头里的长度字段切分。本模块的 MessageParser 就是做这件事。
 *
 * 【用法】每个 TCP 连接（每个 socket）各持有一个 MessageParser 实例，
 *        在 readyRead 里 append(socket->readAll())，再把解析出的消息逐条处理。
 *
 * 【为什么不加 CRC 校验】TCP 头部已有 16 位校验和，且本协议跑在同一台机器
 *        或局域网上，应用层再算一遍 CRC 属于重复劳动。若将来要跑在不可靠
 *        信道上或需要防篡改，再在帧头追加 CRC32 字段并升 Ver 即可。
 */

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace NetProto {

/* ------------------------------------------------------------------ 协议常量 */
const quint16 kMagic       = 0xAA55;              // 固定帧头
const quint8  kVersion     = 0x01;                // 协议版本
const int     kHeaderSize  = 8;                   // 帧头长度(2+1+1+4)
const quint32 kMaxBodySize = 1024u * 1024u;       // 单帧正文上限 1MB，防御性上限

/* -------------------------------------------------------------- 消息类型定义 */
enum MsgType {
    MsgText   = 0x01,   // 普通文本消息
    MsgPing   = 0x02,   // 心跳请求（客户端 -> 服务端）
    MsgPong   = 0x03,   // 心跳应答（服务端 -> 客户端）
    MsgSystem = 0x04    // 系统通知（服务端 -> 客户端，如"服务端即将关闭"）
};

/* ------------------------------------------------------------------ 消息实体 */
struct Message
{
    quint8     type;    // 消息类型
    QByteArray body;    // 正文原始字节

    Message() : type(0) {}
    Message(quint8 t, const QByteArray &b) : type(t), body(b) {}

    QString text() const { return QString::fromUtf8(body); }
};

/* ------------------------------------------------------------------ 打包接口 */
QByteArray encode(quint8 type, const QByteArray &body);
QByteArray encodeText(const QString &text);
QByteArray encodePing();
QByteArray encodePong();
QByteArray encodeSystem(const QString &text);

/* ------------------------------------------------------------------ 辅助函数 */
QString typeName(quint8 type);      // 类型名，用于日志打印

/**
 * 拆包器（Sticky-packet / Fragmentation Parser）
 * ------------------------------------------------------------
 * 有状态：内部维护一个接收缓冲区。把"新收到的字节"喂给它，
 * 它返回"本次能凑出来的所有完整消息"，并把不足一帧的残包留在缓冲区里。
 *
 * 这一个循环同时解决了两个问题：
 *   · 一次 append 解出多条  -> 粘包
 *   · 一条消息跨多次 append -> 拆包
 */
class MessageParser
{
public:
    struct Result
    {
        QList<Message> messages;        // 本次解析出的完整消息（可能 0 条）
        bool           protocolError;   // 是否发生协议错误（已自动丢弃脏数据）
        QString        errorText;       // 错误描述

        Result() : protocolError(false) {}
    };

    MessageParser();

    /** 清空缓冲区。每次新建连接时必须调用，避免上一个连接的残包串到新连接 */
    void reset();

    /** 追加新收到的字节流，返回本次解析结果 */
    Result append(const QByteArray &chunk);

    /** 当前缓冲区内尚未凑成一帧的字节数（调试用） */
    int bufferedBytes() const { return m_buffer.size(); }

private:
    /** 帧头校验失败时，丢弃脏数据、重新寻找下一个 0xAA55 */
    void resync();

    QByteArray m_buffer;
};

} // namespace NetProto

#endif // PROTOCOL_H
