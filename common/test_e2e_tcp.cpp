/**
 * 真实 TCP 链路上的端到端验证
 * ============================================================================
 * test_protocol.cpp 是纯逻辑测试（直接喂字节数组）。这个测试更进一步：
 * 真的开一个 QTcpServer + QTcpSocket，在 127.0.0.1 上收发，验证协议在
 * "真实 TCP 交付边界"下依然能 100% 还原消息。
 *
 * 三个阶段的发送计划，刻意制造两种极端：
 *   阶段1「突发」：把 200 帧拼成一大块，一次 write 发出  → 必然发生粘包
 *   阶段2「连发」：一帧一个 write，连续发 20 帧          → 真实场景
 *   阶段3「截断」：每帧切成前后两半，中间隔 20ms 发出     → 必然发生拆包
 *                  （20ms 远大于回环 RTT，服务端一定会分两次 readyRead）
 *
 * 编译（Qt 5.12.9 MinGW 32 位）：
 *   $qt = 'D:\Qt\Qt5.12.9\5.12.9\mingw73_32'
 *   $gxx = 'D:\Qt\Qt5.12.9\Tools\mingw730_32\bin\g++.exe'
 *   & $gxx -std=c++11 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
 *       "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtNetwork" "." `
 *       test_e2e_tcp.cpp protocol.cpp "-L$qt\lib" -lQt5Network -lQt5Core `
 *       -o test_e2e_tcp.exe
 * 运行：
 *   $env:PATH = "$qt\bin;$env:PATH"; .\test_e2e_tcp.exe
 */

#include "protocol.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

using namespace NetProto;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const int kBurst = 200;   // 阶段1：一次 write 塞进 200 帧
    const int kSeq   =  20;   // 阶段2：一帧一个 write，连发 20 帧
    const int kSplit =  10;   // 阶段3：每帧切成两半，中间隔 20ms

    const int kExpected = kBurst + kSeq + kSplit;

    /* ---- 按阶段编排"每一次 write 要写什么" ---- */
    QVector<QByteArray> steps;
    QStringList expected;

    {   // 阶段1：全部拼成一大块
        QByteArray burst;
        for (int i = 0; i < kBurst; ++i) {
            const QString s = QString("突发-%1").arg(i);
            expected << s;
            burst += encodeText(s);
        }
        steps.append(burst);
    }
    for (int i = 0; i < kSeq; ++i) {          // 阶段2：一帧一次 write
        const QString s = QString("连发-%1").arg(i);
        expected << s;
        steps.append(encodeText(s));
    }
    for (int i = 0; i < kSplit; ++i) {        // 阶段3：每帧拆成前后两半
        const QString s = QString("截断-%1").arg(i);
        expected << s;
        const QByteArray f = encodeText(s);
        steps.append(f.left(f.size() / 2));
        steps.append(f.mid(f.size() / 2));
    }

    qDebug().noquote() << "============ 真实 TCP 端到端协议验证 ============";
    qDebug().noquote() << QString("计划发送 %1 条消息，分 %2 次 write 发出")
                              .arg(kExpected).arg(steps.size());
    qDebug().noquote() << QString("  阶段1 突发 %1 帧 / 1 次 write（制造粘包）").arg(kBurst);
    qDebug().noquote() << QString("  阶段2 连发 %1 帧 / %1 次 write").arg(kSeq);
    qDebug().noquote() << QString("  阶段3 截断 %1 帧 / %2 次 write（制造拆包）")
                              .arg(kSplit).arg(kSplit * 2);

    /* ---- 服务端 ---- */
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        qDebug().noquote() << "监听失败:" << server.errorString();
        return 3;
    }
    const quint16 port = server.serverPort();

    MessageParser parser;          // 一条连接一个拆包器
    QStringList received;
    int readyReadCount = 0;
    int protocolErrors = 0;
    bool finished = false;

    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        QTcpSocket *conn = server.nextPendingConnection();
        QObject::connect(conn, &QTcpSocket::readyRead, [&, conn]() {
            ++readyReadCount;
            const QByteArray chunk = conn->readAll();      // 真实的 TCP 交付边界
            const MessageParser::Result r = parser.append(chunk);
            if (r.protocolError) ++protocolErrors;
            for (int i = 0; i < r.messages.size(); ++i)
                received << r.messages.at(i).text();

            if (!finished && received.size() >= kExpected) {
                finished = true;

                bool ok = (received.size() == kExpected);
                for (int i = 0; ok && i < expected.size(); ++i)
                    ok = (received.at(i) == expected.at(i));

                qDebug().noquote() << "";
                qDebug().noquote() << "-------------- 结果 --------------";
                qDebug().noquote() << QString("期望消息数            : %1").arg(kExpected);
                qDebug().noquote() << QString("实际还原消息数        : %1").arg(received.size());
                qDebug().noquote() << QString("内容与顺序            : %1").arg(ok ? "完全一致" : "★ 不一致 ★");
                qDebug().noquote() << QString("客户端 write 次数     : %1").arg(steps.size());
                qDebug().noquote() << QString("服务端 readyRead 次数 : %1   （远小于 %2 就证明发生了粘包）")
                                          .arg(readyReadCount).arg(kExpected);
                qDebug().noquote() << QString("协议错误次数          : %1").arg(protocolErrors);
                qDebug().noquote() << (ok && protocolErrors == 0
                    ? "结论：真实 TCP 链路上 100% 还原了消息边界 ✔"
                    : "结论：校验失败 ✘");
                app.exit((ok && protocolErrors == 0) ? 0 : 1);
            }
        });
    });

    /* ---- 客户端：按计划分步写入 ---- */
    QTcpSocket client;
    int stepIndex = 0;

    QTimer *pump = new QTimer(&client);
    pump->setInterval(20);        // 每步间隔 20ms，保证 TCP 把它们分成不同段

    QObject::connect(&client, &QTcpSocket::connected, [&]() { pump->start(); });

    QObject::connect(pump, &QTimer::timeout, [&]() {
        if (stepIndex >= steps.size()) { pump->stop(); return; }
        client.write(steps.at(stepIndex));
        client.flush();
        ++stepIndex;
    });

    client.connectToHost(QHostAddress::LocalHost, port);

    QTimer::singleShot(30000, &app, [&]() {
        qDebug().noquote() << QString("超时：只收到 %1 / %2 条").arg(received.size()).arg(kExpected);
        app.exit(2);
    });

    return app.exec();
}
