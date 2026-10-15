#include "protocol.h"

namespace NetProto {

namespace {

/* ---- 大端序(Big-Endian / 网络字节序)读写小工具 ---- */

inline void appendU16BE(QByteArray &out, quint16 v)
{
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

inline void appendU32BE(QByteArray &out, quint32 v)
{
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >>  8) & 0xFF));
    out.append(char( v        & 0xFF));
}

inline quint16 readU16BE(const char *p)
{
    return quint16((quint16(quint8(p[0])) << 8) | quint16(quint8(p[1])));
}

inline quint32 readU32BE(const char *p)
{
    return (quint32(quint8(p[0])) << 24)
         | (quint32(quint8(p[1])) << 16)
         | (quint32(quint8(p[2])) <<  8)
         |  quint32(quint8(p[3]));
}

/* 帧头魔数的字节形态，用于 indexOf 查找 */
const char kMagicBytes[2] = { char(0xAA), char(0x55) };

} // namespace

/* ==================================================================== 打包 */

QByteArray encode(quint8 type, const QByteArray &body)
{
    QByteArray frame;
    frame.reserve(kHeaderSize + body.size());

    appendU16BE(frame, kMagic);                 // [0..1]  Magic
    frame.append(char(kVersion));               // [2]     Ver
    frame.append(char(type));                   // [3]     Type
    appendU32BE(frame, quint32(body.size()));   // [4..7]  BodyLength
    frame.append(body);                         // [8..]   Body

    return frame;
}

QByteArray encodeText(const QString &text)   { return encode(MsgText,   text.toUtf8()); }
QByteArray encodePing()                      { return encode(MsgPing,   QByteArray()); }
QByteArray encodePong()                      { return encode(MsgPong,   QByteArray()); }
QByteArray encodeSystem(const QString &text) { return encode(MsgSystem, text.toUtf8()); }

QString typeName(quint8 type)
{
    switch (type) {
    case MsgText:   return QStringLiteral("文本");
    case MsgPing:   return QStringLiteral("心跳请求");
    case MsgPong:   return QStringLiteral("心跳应答");
    case MsgSystem: return QStringLiteral("系统通知");
    default:        return QStringLiteral("未知类型(0x%1)")
                               .arg(int(type), 2, 16, QChar('0'));
    }
}

/* ================================================================== 拆包器 */

MessageParser::MessageParser()
{
}

void MessageParser::reset()
{
    m_buffer.clear();
}

MessageParser::Result MessageParser::append(const QByteArray &chunk)
{
    Result result;

    if (!chunk.isEmpty())
        m_buffer.append(chunk);     // 先把新数据接在残包后面

    /* 循环拆包：一次 append 可能解出 0 ~ N 条消息 */
    for (;;)
    {
        /* (1) 连帧头都不够 —— 半包，保留缓冲区，等下一批数据
         *     这里是"拆包"能被正确还原的关键：绝不能把不足一帧的数据丢掉 */
        if (m_buffer.size() < kHeaderSize)
            break;

        /* (2) 校验魔数：不匹配说明字节流已经错位，丢脏数据重新同步 */
        if (readU16BE(m_buffer.constData()) != kMagic)
        {
            result.protocolError = true;
            result.errorText = QStringLiteral("帧头校验失败，已丢弃脏数据并重新同步");
            resync();
            continue;
        }

        const char   *p       = m_buffer.constData();
        const quint8  type    = quint8(p[3]);
        const quint32 bodyLen = readU32BE(p + 4);

        /* (3) 长度超限：不可能是合法帧，按脏数据处理（防御内存被撑爆） */
        if (bodyLen > kMaxBodySize)
        {
            result.protocolError = true;
            result.errorText = QStringLiteral("正文长度 %1 超过上限 %2，疑似脏数据")
                                   .arg(bodyLen).arg(kMaxBodySize);
            m_buffer.remove(0, 1);      // 只丢 1 字节，下一轮继续找帧头
            continue;
        }

        /* (4) 正文还没收全 —— 依然是半包，必须保留缓冲区等待后续数据 */
        if (quint32(m_buffer.size()) < quint32(kHeaderSize) + bodyLen)
            break;

        /* (5) 凑齐一帧，切出来交给上层调用者 */
        const QByteArray body = m_buffer.mid(kHeaderSize, int(bodyLen));
        m_buffer.remove(0, kHeaderSize + int(bodyLen));
        result.messages.append(Message(type, body));

        /* 注意：这里不 break，要接着循环。
         * 因为缓冲区里很可能还"粘"着下一帧甚至下下帧（粘包）。 */
    }

    return result;
}

void MessageParser::resync()
{
    /* 从第 2 个字节开始找下一个魔数。
     * 不从 0 开始是因为当前位置已经确认不是帧头了，跳过它避免原地死循环。 */
    const int idx = m_buffer.indexOf(QByteArray(kMagicBytes, 2), 1);

    if (idx < 0)
    {
        /* 整段数据里都没有帧头。只保留最后 1 个字节：
         * 它可能是被 TCP 从中间切断的帧头首字节 0xAA，下一批数据会补上 0x55。 */
        m_buffer = m_buffer.right(1);
        if (!m_buffer.isEmpty() && quint8(m_buffer.at(0)) != 0xAA)
            m_buffer.clear();
    }
    else
    {
        /* 丢掉错位字节，让缓冲区正好从新的帧头开始 */
        m_buffer.remove(0, idx);
    }
}

} // namespace NetProto
