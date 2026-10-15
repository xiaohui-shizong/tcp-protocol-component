// 我的代码
#include "widget.h"
#include "ui_widget.h"

#include <QMessageBox>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QToolTip>
#include <QTextCursor>
#include <QTextCharFormat>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);
    this->setLayout(ui->verticalLayout);    //窗口布局优化

    server = new QTcpServer(this);

    connect(ui->comboBoxChildren,&MyComboBox::on_ComboBox_clicked,this,&Widget::mComboBox_refresh);
    connect(server,SIGNAL(newConnection()),this,SLOT(on_newClient_connect()));

    ui->btnStop->setEnabled(false);
    ui->btnLineout->setEnabled(false);
    ui->btnSend->setEnabled(false);

    ui->comboBoxAddr->insertItem(0,"0.0.0.0");
    ui->comboBoxAddr->insertItem(1,"127.0.0.1");

    // 获取本机IPv4地址，填充地址下拉框
    //手册: QList<QHostAddress> QNetworkInterface::allAddresses()
    QList<QHostAddress> address = QNetworkInterface::allAddresses();
    for(QHostAddress tmp : address){
        if(tmp.protocol() == QAbstractSocket::IPv4Protocol){
            ui->comboBoxAddr->addItem(tmp.toString());
        }
    }
}

Widget::~Widget()
{
    delete ui;
}

// 有新客户端连接进入
void Widget::on_newClient_connect()
{
    qDebug() << "newClient In!";
    //手册: bool QTcpServer::hasPendingConnections() const
    //原来用 if 只处理一个；改成 while 更稳妥：排队等待接入的连接会被一次性接完
    while(server->hasPendingConnections()){
        //手册: QTcpSocket *QTcpServer::nextPendingConnection()
        QTcpSocket *client = server->nextPendingConnection();

        QString clientIp = client->peerAddress().toString();
        quint16 clientPort = client->peerPort();
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");

        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, QString("[%1:%2]").arg(clientIp).arg(int(clientPort)));
        mInsertColor(Qt::black, "[新客户端接入]\n");
        qDebug() << "客户端IP:" << clientIp << "客户端端口:" << clientPort;

        // ★ 关键：为该连接建立**独立**的拆包器，并保证是干净的（没有历史残包）
        m_parsers.insert(client, NetProto::MessageParser());

        connect(client,SIGNAL(readyRead()),this,SLOT(on_readyRead_handler()));  //收到信息
        connect(client,SIGNAL(disconnected()),this,SLOT(mdisconnected()));      //断开连接

        // socket 被销毁时顺手清掉它的拆包器，避免 map 里留下悬空 key
        connect(client,&QObject::destroyed,this,[this,client](){
            m_parsers.remove(client);
        });
    }

    mComboBox_refresh();
}


// 收到客户端数据
// ★ 注意：这里**不再**把 readAll() 的结果当成"一条消息"，而是喂给拆包器
void Widget::on_readyRead_handler()
{
    QTcpSocket *tmpSock = qobject_cast<QTcpSocket*>(sender());
    if(!tmpSock)
        return;

    // 这一次读到的字节：可能不足一帧（拆包），也可能含好几帧（粘包）
    const QByteArray revData = tmpSock->readAll();
    if(revData.isEmpty())
        return;

    NetProto::MessageParser &parser = m_parsers[tmpSock];
    const NetProto::MessageParser::Result res = parser.append(revData);

    if(res.protocolError)
        handleProtocolError(tmpSock, res.errorText);

    // 逐条处理「完整」消息——消息边界由协议保证，不再靠猜
    for(const NetProto::Message &msg : res.messages)
        handleMessage(tmpSock, msg);
}


// 处理一条完整的应用层消息（按 Type 分发）
void Widget::handleMessage(QTcpSocket *sock, const NetProto::Message &msg)
{
    const QString clientIp   = sock->peerAddress().toString();
    const quint16 clientPort = sock->peerPort();
    const QString timeStr    = QTime::currentTime().toString("hh:mm:ss");

    switch(msg.type)
    {
    case NetProto::MsgText:
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::darkGreen, QString("[%1:%2]").arg(clientIp).arg(int(clientPort)));
        mInsertColor(Qt::darkGreen, QString("客户端: %1\n").arg(msg.text()));
        qDebug() << "来自客户端的信息: " << msg.text();
        break;

    case NetProto::MsgPing:
        // 心跳请求：立刻回一个 Pong。不打印日志，避免每 10 秒刷一片屏
        sendMessage(sock, NetProto::MsgPong, QByteArray());
        qDebug() << "收到心跳请求，已回复 Pong ->" << clientPort;
        break;

    case NetProto::MsgPong:
        // 服务端不主动发 Ping，收到 Pong 直接忽略
        break;

    case NetProto::MsgSystem:
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::darkYellow, QString("[%1:%2] [系统通知] %3\n")
                                         .arg(clientIp).arg(int(clientPort)).arg(msg.text()));
        break;

    default:
        mInsertColor(Qt::red, QString("[%1] 收到未知类型消息 0x%2（正文 %3 字节）\n")
                                  .arg(timeStr)
                                  .arg(int(msg.type), 2, 16, QChar('0'))
                                  .arg(msg.body.size()));
        break;
    }
}


// 打包成「一帧」再发送；返回是否成功
bool Widget::sendMessage(QTcpSocket *sock, quint8 type, const QByteArray &body)
{
    if(!sock || sock->state() != QAbstractSocket::ConnectedState)
        return false;

    // ★ 发出去的永远是带 8 字节帧头的完整帧，对方才能按帧边界还原
    const QByteArray frame = NetProto::encode(type, body);
    const qint64 ret = sock->write(frame);

    if(ret != frame.size()){
        mInsertColor(Qt::red, QString("[%1] 发送失败：%2\n")
                                  .arg(QTime::currentTime().toString("hh:mm:ss"))
                                  .arg(sock->errorString()));
        return false;
    }
    return true;
}


// 给所有在线客户端群发
void Widget::sendToAll(quint8 type, const QByteArray &body)
{
    const QList<QTcpSocket *> clients = server->findChildren<QTcpSocket*>();
    for(QTcpSocket *tmp : clients){
        if(tmp->state() == QAbstractSocket::ConnectedState)
            sendMessage(tmp, type, body);
    }
}


void Widget::handleProtocolError(QTcpSocket *sock, const QString &err)
{
    mInsertColor(Qt::red, QString("[%1][协议错误] %2（对端 %3:%4）\n")
                              .arg(QTime::currentTime().toString("hh:mm:ss"))
                              .arg(err)
                              .arg(sock->peerAddress().toString())
                              .arg(int(sock->peerPort())));
}


// 客户端断开连接
void Widget::mdisconnected()
{
    QTcpSocket *tmpSock = qobject_cast<QTcpSocket *>(sender());
    if(!tmpSock)
        return;

    QString clientIp = tmpSock->peerAddress().toString();
    quint16 clientPort = tmpSock->peerPort();
    QString timeStr = QTime::currentTime().toString("hh:mm:ss");

    mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
    mInsertColor(Qt::black, QString("[%1:%2]").arg(clientIp).arg(int(clientPort)));
    mInsertColor(Qt::black, "[客户端断开]\n");
    qDebug() << "信号解除";

    m_parsers.remove(tmpSock);      // 连同这个连接的拆包器一起销毁，残包不会串到下一个连接
    tmpSock->deleteLater();

    mComboBox_refresh();
}

// 开始监听按钮
void Widget::on_btnListen_clicked()
{
    bool ok = false;
    const int port = ui->lineEditPort->text().toInt(&ok);
    if(!ok || port < 1 || port > 65535){
        mInsertColor(Qt::red, "端口号非法！范围 1~65535\n");
        return;
    }

    QHostAddress addr = QHostAddress(ui->comboBoxAddr->currentText());
    //手册: bool QTcpServer::listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0)
    if (server->listen(addr, port))
    {
        qDebug() << "成功监听" << server->serverPort();
        ui->btnListen->setEnabled(false);
        ui->btnStop->setEnabled(true);

        mInsertColor(Qt::black, QString("开始监听 %1:%2 ｜ 协议版本 0x%3 ｜ 帧头 %4 字节\n")
                                     .arg(ui->comboBoxAddr->currentText())
                                     .arg(int(server->serverPort()))
                                     .arg(int(NetProto::kVersion), 2, 16, QChar('0'))
                                     .arg(NetProto::kHeaderSize));
    }
    else
    {
        qDebug() << "listenError  " << server->errorString();
        QMessageBox msgBox;
        msgBox.setWindowTitle("监听失败");
        //不再写死"端口号被占用"：地址不可用等其它原因也要如实告诉用户
        msgBox.setText("监听失败：" + server->errorString());
        msgBox.exec();
        return;
    }
}

// 停止监听按钮
void Widget::on_btnStop_clicked()
{
    const QList<QTcpSocket *> tcpSocketClients = server->findChildren<QTcpSocket*>();

    // 先给在线客户端发一条「系统通知」，让对方知道不是被静默掐断
    sendToAll(NetProto::MsgSystem, QStringLiteral("服务端即将停止监听，连接将被断开").toUtf8());

    for(QTcpSocket* tmp : tcpSocketClients)
    {
        if(tmp->state() == QAbstractSocket::ConnectedState)
        {
            tmp->flush();
            // 关键：不等数据真正写出去就 close()，通知会烂在发送缓冲区里
            tmp->waitForBytesWritten(200);
        }
    }

    for(QTcpSocket* tmp : tcpSocketClients)
    {
        tmp->close();
    }
    if(server->isListening())
    {
        server->close();
    }

    m_parsers.clear();      // 所有连接的拆包器一并清空

    ui->btnListen->setEnabled(true);
    ui->btnStop->setEnabled(false);
    qDebug() << "监听已断开";
    mComboBox_refresh();
}

// 断开按钮：只断开下拉框里选中的「那一个」客户端
// （原实现直接调用 on_btnStop_clicked()，等于停止整个监听把所有人都踢掉，语义不对）
void Widget::on_btnLineout_clicked()
{
    const QString selText = ui->comboBoxChildren->currentText();
    if(selText.isEmpty() || selText == "all"){
        mInsertColor(Qt::black, "请先在下方列表中选择要断开的具体客户端\n");
        return;
    }

    bool ok = false;
    const quint16 port = selText.toUShort(&ok);
    if(!ok)
        return;

    const QList<QTcpSocket *> tcpSocketClients = server->findChildren<QTcpSocket*>();
    for(QTcpSocket* tmp : tcpSocketClients)
    {
        if(tmp->state() == QAbstractSocket::ConnectedState && tmp->peerPort() == port)
        {
            tmp->write(NetProto::encodeSystem("服务端主动断开了你的连接"));
            tmp->flush();
            tmp->waitForBytesWritten(200);
            tmp->disconnectFromHost();   // 比 close() 温和：让排队中的数据先发完
            mInsertColor(Qt::black, QString("已断开客户端 %1:%2\n")
                                        .arg(tmp->peerAddress().toString()).arg(int(port)));
            break;
        }
    }
}

// 发送按钮
void Widget::on_btnSend_clicked()
{
    const QList<QTcpSocket *> tcpSocketClients = server->findChildren<QTcpSocket*>();

    if(tcpSocketClients.isEmpty()){
        qDebug() << "没有找到任何客户端socket！";
        QMessageBox msgBox;
        msgBox.setWindowTitle("发送错误");
        msgBox.setText("当前无连接");
        msgBox.exec();
        ui->btnSend->setEnabled(false);
        return;
    }

    const QString selText = ui->comboBoxChildren->currentText();
    const QString sendStr = ui->textEditSend->toPlainText();
    if(sendStr.isEmpty()){
        mInsertColor(Qt::black,"发送信息不能为空\n");
        return;
    }

    const QByteArray body   = sendStr.toUtf8();     // ★ 只取正文，帧头交给 encode() 去加
    const QString    timeStr = QTime::currentTime().toString("hh:mm:ss");

    if(selText != "all")    // 点对点模式
    {
        bool ok = false;
        const quint16 port = selText.toUShort(&ok);     //遍历找端口匹配的socket
        if(!ok)
            return;

        bool found = false;
        for(QTcpSocket* tmp : tcpSocketClients)         //每次循环把其中一个 socket 指针给到tmp
        {
            if(tmp->peerPort() == port && tmp->state() == QAbstractSocket::ConnectedState)
            {
                found = true;
                if(sendMessage(tmp, NetProto::MsgText, body)){
                    mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
                    mInsertColor(Qt::black, QString("[%1]->").arg(int(port)));
                    mInsertColor(Qt::blue, QString("我：%1\n").arg(sendStr));
                }
                break;
            }
        }
        if(!found)
            mInsertColor(Qt::red, QString("[%1] 未找到端口为 %2 的在线客户端，可能已断开\n")
                                      .arg(timeStr).arg(int(port)));
    }
    else    // 群发模式
    {
        int sentCount = 0;
        for(QTcpSocket* tmp : tcpSocketClients)
        {
            if(tmp->state() == QAbstractSocket::ConnectedState)
            {
                if(sendMessage(tmp, NetProto::MsgText, body)){
                    mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
                    mInsertColor(Qt::black, QString("[%1]->").arg(int(tmp->peerPort())));
                    mInsertColor(Qt::blue, QString("我(群发)：%1\n").arg(sendStr));
                    ++sentCount;
                }
            }
        }
        if(sentCount == 0)
            mInsertColor(Qt::red, QString("[%1] 群发失败：当前没有处于连接状态的客户端\n").arg(timeStr));
    }

    ui->textEditSend->clear();      // 发送后清空输入框
}


// 刷新客户端下拉列表
void Widget::mComboBox_refresh()
{
    ui->comboBoxChildren->clear();      //1.清空下拉框所有旧选项
    QList<QTcpSocket *> tcpSocketClients = server->findChildren<QTcpSocket*>();     //2.拿到当前所有存活的客户端socket
    int onlineCount = 0; //统计真正在线的客户端

    for(QTcpSocket* tmp : tcpSocketClients)
    {
        if(tmp != nullptr && tmp->state() == QAbstractSocket::ConnectedState){
            ui->comboBoxChildren->addItem(QString::number(tmp->peerPort()));    //3.把每个客户端的端口号加到下拉框
            onlineCount++;
        }
    }

    if(onlineCount >= 1)    //4.群发
    {
        ui->comboBoxChildren->addItem("all");
    }

    // 按钮状态跟着在线人数走，避免"一个客户端都没有却还能点发送/断开"
    ui->btnSend->setEnabled(onlineCount >= 1);
    ui->btnLineout->setEnabled(onlineCount >= 1);
}

void Widget::mInsertColor(Qt::GlobalColor color, QString str)       //改变文本颜色
{
    QTextCursor cursor = ui->textEditRev->textCursor();     //实例化光标对象
    cursor.movePosition(QTextCursor::End);      //光标移动到末尾
    QTextCharFormat format;         //定义一个 文字样式对象
    format.setForeground(QBrush(QColor(color)));    //设置字体前景色（文字颜色）
    cursor.setCharFormat(format);       //把这个格式交给光标，后续插入文字就套用这个样式
    cursor.insertText(str);     //在当前光标的位置，插入一段纯文本

    ui->textEditRev->setTextCursor(cursor);   //【把副本写回控件，更新界面光标】
    ui->textEditRev->ensureCursorVisible();   //【自动滚动，显示这一行】
}
