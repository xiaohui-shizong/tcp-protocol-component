/**
 * protocol.h / protocol.cpp 的独立单元测试
 * ============================================================================
 * 不依赖 GUI、不依赖真实网络，纯逻辑测试：
 * 把精心构造的"字节流片段"直接喂给 MessageParser，检查它是否总能还原出
 * 正确的消息边界。这样就能在没有两台机器的情况下，证明粘包/拆包真的被解决。
 *
 * 编译（Qt 5.12.9 MinGW 32 位，PowerShell）：
 *   $qt = 'D:\Qt\Qt5.12.9\5.12.9\mingw73_32'
 *   $gxx = 'D:\Qt\Qt5.12.9\Tools\mingw730_32\bin\g++.exe'
 *   & $gxx -std=c++11 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
 *       "-I$qt\include" "-I$qt\include\QtCore" "." `
 *       test_protocol.cpp protocol.cpp "-L$qt\lib" -lQt5Core `
 *       -o test_protocol.exe
 * 运行：
 *   $env:PATH = "$qt\bin;$env:PATH"; .\test_protocol.exe
 */

#include "protocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

using namespace NetProto;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const QString &what)
{
    if (cond) { ++g_pass; qDebug().noquote() << "   [PASS]" << what; }
    else      { ++g_fail; qDebug().noquote() << "   [FAIL]" << what; }
}

/** 把一段字节流按 chunkSize 切片投喂，返回解析出的所有消息 */
static QList<Message> feedInChunks(MessageParser &p, const QByteArray &stream,
                                   int chunkSize, int *errCount = nullptr)
{
    QList<Message> out;
    for (int i = 0; i < stream.size(); i += chunkSize) {
        MessageParser::Result r = p.append(stream.mid(i, chunkSize));
        out += r.messages;
        if (errCount && r.protocolError) ++(*errCount);
    }
    return out;
}

static QStringList textsOf(const QList<Message> &ms)
{
    QStringList l;
    for (int i = 0; i < ms.size(); ++i) l << ms.at(i).text();
    return l;
}

/* ======================================================================== */

static void test01_singleFrame()
{
    qDebug().noquote() << "\n[测试 1] 一条完整消息（基准）";
    MessageParser p;
    MessageParser::Result r = p.append(encodeText("hello"));

    check(r.messages.size() == 1,                          "解出 1 条消息");
    check(r.messages.value(0).text() == "hello",            "正文还原为 hello");
    check(r.messages.value(0).type == MsgText,              "类型为 MsgText");
    check(!r.protocolError,                                 "无协议错误");
    check(p.bufferedBytes() == 0,                           "缓冲区已清空");
}

static void test02_stickyPackets()
{
    qDebug().noquote() << "\n[测试 2] 粘包：3 条消息挤在一次 readAll 里";
    MessageParser p;
    const QByteArray stream = encodeText("AAA") + encodeText("BBB") + encodeText("CCC");

    MessageParser::Result r = p.append(stream);   // 模拟 TCP 把 3 帧合并交付

    check(r.messages.size() == 3, "一次 append 解出 3 条（旧代码这里只会得到 1 条）");
    check(textsOf(r.messages) == (QStringList() << "AAA" << "BBB" << "CCC"),
          "顺序与内容都正确");
    check(p.bufferedBytes() == 0, "缓冲区已清空");
}

static void test03_fragmented()
{
    qDebug().noquote() << "\n[测试 3] 拆包：1 条消息被切成 4 段到达";
    const QByteArray frame = encodeText("被切碎的报文");
    MessageParser p;

    QList<Message> got;
    const int cuts[] = { 3, 2, 5, -1 };   // -1 表示"剩下的全部"
    int fed = 0;
    for (int i = 0; i < 4; ++i) {
        int len = (cuts[i] < 0) ? (frame.size() - fed) : cuts[i];
        if (fed + len > frame.size()) len = frame.size() - fed;

        MessageParser::Result r = p.append(frame.mid(fed, len));
        fed += len;
        got += r.messages;

        if (i < 3)
            check(r.messages.isEmpty(),
                  QString("第 %1 段（%2 字节）不应解出消息，只能留残包").arg(i + 1).arg(len));
    }

    check(fed == frame.size(), "4 段正好拼成整帧");
    check(got.size() == 1, "最终解出 1 条消息");
    check(got.value(0).text() == "被切碎的报文", "正文完整还原，中文没乱码");
    check(p.bufferedBytes() == 0, "缓冲区已清空");
}

static void test04_byteByByte()
{
    qDebug().noquote() << "\n[测试 4] 逐字节投喂（最极端的拆包）";
    const int N = 20;
    QByteArray stream;
    for (int i = 0; i < N; ++i)
        stream += encodeText(QString("消息%1-中文测试").arg(i));

    MessageParser p;
    int errCount = 0;
    const QList<Message> got = feedInChunks(p, stream, 1, &errCount);  // 每次只给 1 字节

    check(got.size() == N, QString("%1 条消息全部还原（实际 %2 条）").arg(N).arg(got.size()));

    bool allOk = (got.size() == N);
    for (int i = 0; i < got.size() && allOk; ++i)
        allOk = (got.at(i).text() == QString("消息%1-中文测试").arg(i));
    check(allOk, "每一条的内容与顺序都完全正确（含中文多字节）");

    check(errCount == 0, "全程没有误报协议错误");
    check(p.bufferedBytes() == 0, "缓冲区已清空");
}

static void test05_dirtyDataResync()
{
    qDebug().noquote() << "\n[测试 5] 帧头前混入脏数据 —— 自动重新同步";
    QByteArray dirty;
    dirty.append(char(0x00)).append(char(0x11)).append(char(0x22))
         .append(char(0x33)).append(char(0x7F));      // 5 字节垃圾，且不含 0xAA55

    MessageParser p;
    MessageParser::Result r =
        p.append(dirty + encodeText("脏数据之后的第一条") + encodeText("第二条"));

    check(r.protocolError, "检测到帧头错位并报协议错误");
    check(r.messages.size() == 2, "重新同步后仍正确解出 2 条消息");
    check(textsOf(r.messages) == (QStringList() << "脏数据之后的第一条" << "第二条"),
          "内容正确");
}

static void test06_oversizeBody()
{
    qDebug().noquote() << "\n[测试 6] 长度字段被伪造/损坏 —— 不崩溃、能自恢复";
    QByteArray bad = encode(MsgText, QByteArray());   // 先造一个合法帧头
    bad[4] = char(0x7F); bad[5] = char(0xFF);
    bad[6] = char(0xFF); bad[7] = char(0xFF);         // 把长度改成 0x7FFFFFFF

    MessageParser p;
    MessageParser::Result r = p.append(bad + encodeText("超限帧之后的正常消息"));

    check(r.protocolError, "识别出长度超限并报协议错误");
    check(r.messages.size() == 1, "只解出后面那条正常消息");
    check(r.messages.value(0).text() == "超限帧之后的正常消息", "内容正确");
    check(p.bufferedBytes() == 0, "缓冲区已清空（没有把脏数据一直留着）");
}

static void test07_emptyBodyAndTypes()
{
    qDebug().noquote() << "\n[测试 7] 空正文帧 + 类型字段保真";
    MessageParser p;
    MessageParser::Result r =
        p.append(encodePing() + encodePong() + encodeSystem("系统通知") + encodeText(""));

    check(r.messages.size() == 4, "4 帧全部解出（含 0 字节正文的帧）");
    check(r.messages.value(0).type == MsgPing && r.messages.value(0).body.isEmpty(),
          "Ping：类型正确、正文为空");
    check(r.messages.value(1).type == MsgPong && r.messages.value(1).body.isEmpty(),
          "Pong：类型正确、正文为空");
    check(r.messages.value(2).type == MsgSystem && r.messages.value(2).text() == "系统通知",
          "System：类型与正文都正确");
    check(r.messages.value(3).type == MsgText && r.messages.value(3).body.isEmpty(),
          "空文本帧：长度字段为 0 也能正确切分");
}

static void test08_mixedRandomSplit()
{
    qDebug().noquote() << "\n[测试 8] 混合压力：50 条消息 + 伪随机切片投喂";
    const int N = 50;
    QByteArray stream;
    QStringList expect;
    for (int i = 0; i < N; ++i) {
        const QString s = QString("第%1条-mixed-测试数据").arg(i);
        expect << s;
        stream += encodeText(s);
    }

    MessageParser p;
    quint32 seed = 20261013u;      // 固定种子，保证结果可复现
    QList<Message> got;
    int pos = 0;
    while (pos < stream.size()) {
        seed = seed * 1103515245u + 12345u;              // 简易 LCG
        const int chunk = 1 + int((seed >> 16) % 37);    // 每片 1~37 字节
        got += p.append(stream.mid(pos, chunk)).messages;
        pos += chunk;
    }

    check(got.size() == N, QString("%1 条全部还原（实际 %2 条）").arg(N).arg(got.size()));
    check(textsOf(got) == expect, "内容与顺序完全一致");
    check(p.bufferedBytes() == 0, "缓冲区已清空");
}

static void test09_independentParsers()
{
    qDebug().noquote() << "\n[测试 9] 多连接隔离：两个拆包器互不干扰";
    const QByteArray fa = encodeText("连接A的消息");
    const QByteArray fb = encodeText("连接B的消息");
    const QByteArray fa1 = fa.left(5);       // 故意切在帧头中间
    const QByteArray fa2 = fa.mid(5);

    MessageParser a, b;
    a.append(fa1);                            // A 先来半个包
    MessageParser::Result rb = b.append(fb);  // B 的整包插进来
    MessageParser::Result ra = a.append(fa2); // A 的另外半个包

    check(rb.messages.size() == 1 && rb.messages.value(0).text() == "连接B的消息",
          "B 的完整消息不受 A 的残包影响");
    check(ra.messages.size() == 1 && ra.messages.value(0).text() == "连接A的消息",
          "A 的残包被独立保存并正确拼接");
    check(a.bufferedBytes() == 0 && b.bufferedBytes() == 0, "两个缓冲区都已清空");
}

static void test10_resetAndEmptyChunk()
{
    qDebug().noquote() << "\n[测试 10] reset 与空数据（连接复用场景）";
    const QByteArray fa = encodeText("第一条");

    MessageParser p;
    p.append(fa.left(4));
    check(p.bufferedBytes() == 4, "半个包残留在缓冲区");

    p.reset();
    check(p.bufferedBytes() == 0, "reset 后缓冲区被清空（新连接不会串到旧残包）");

    MessageParser::Result r = p.append(QByteArray());
    check(r.messages.isEmpty() && !r.protocolError, "空数据不产生消息、也不报错");

    r = p.append(fa);
    check(r.messages.size() == 1 && r.messages.value(0).text() == "第一条",
          "reset 之后仍能正常解析新连接的帧");
}

static void test11_wireFormat()
{
    qDebug().noquote() << "\n[测试 11] 帧格式字节级校验（确保协议就是文档里写的那样）";
    const QByteArray f = encodeText("AB");

    check(f.size() == 10, "帧长 = 8 字节帧头 + 2 字节正文");
    check(quint8(f.at(0)) == 0xAA && quint8(f.at(1)) == 0x55, "Magic = AA 55");
    check(quint8(f.at(2)) == 0x01, "Ver = 01");
    check(quint8(f.at(3)) == 0x01, "Type = 01 (MsgText)");
    check(quint8(f.at(4)) == 0x00 && quint8(f.at(5)) == 0x00
          && quint8(f.at(6)) == 0x00 && quint8(f.at(7)) == 0x02,
          "BodyLength = 00 00 00 02（大端序）");
    check(f.mid(8) == QByteArray("AB"), "正文 = AB");
}

/* ======================================================================== */

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    qDebug().noquote() << "============== 自定义应用层协议 · 拆包器单元测试 ==============";
    qDebug().noquote() << QString("帧头 %1 字节 | 魔数 0x%2 | 版本 0x%3 | 正文上限 %4 字节")
                              .arg(kHeaderSize)
                              .arg(kMagic, 4, 16, QChar('0'))
                              .arg(int(kVersion), 2, 16, QChar('0'))
                              .arg(kMaxBodySize);

    test01_singleFrame();
    test02_stickyPackets();
    test03_fragmented();
    test04_byteByByte();
    test05_dirtyDataResync();
    test06_oversizeBody();
    test07_emptyBodyAndTypes();
    test08_mixedRandomSplit();
    test09_independentParsers();
    test10_resetAndEmptyChunk();
    test11_wireFormat();

    qDebug().noquote() << "\n======================== 测试结果 ========================";
    qDebug().noquote() << QString("通过 %1 项，失败 %2 项").arg(g_pass).arg(g_fail);
    qDebug().noquote() << (g_fail == 0 ? "全部通过 —— 粘包 / 拆包 / 脏数据恢复均已验证"
                                       : "存在失败项，请检查实现");
    return g_fail == 0 ? 0 : 1;
}
