#include "widget.h"
#include "ui_widget.h"

#include <QMessageBox>
#include <QTcpSocket>
#include <QHostAddress>
#include <QTimer>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);
    this->setLayout(ui->verticalLayout);

    // 连接超时定时器
    m_connTimer = new QTimer(this);
    m_connTimer->setSingleShot(true); //只触发一次
    m_connTimer->setInterval(5000);   //5000ms = 5秒
    connect(m_connTimer, &QTimer::timeout, this, &Widget::onConnTimeout);

    // 心跳定时器：连上以后每 10 秒发一个 Ping，用来验证链路还活着
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(10000);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &Widget::onHeartbeatTimeout);

    setUiConnected(false);      // 初始为"未连接"状态
}

Widget::~Widget()
{
    delete ui;
}

// 统一管理"已连接 / 未连接"时各控件是否可用
// 顺带把"没连接却点了发送"这条崩溃路径从界面上堵死（同时函数内还有判空兜底）
void Widget::setUiConnected(bool connected)
{
    ui->btnConnect->setEnabled(!connected);
    ui->lineEditAddr->setEnabled(!connected);
    ui->lineEditPort->setEnabled(!connected);
    ui->lineEditLocalPort->setEnabled(!connected);
    ui->btndisConnect->setEnabled(connected);
    ui->btnSend->setEnabled(connected);
}


void Widget::on_btnConnect_clicked()        //连接按钮
{
    // 0) 防重复点击
    if(client != nullptr && (client->state() == QAbstractSocket::ConnectingState
                          || client->state() == QAbstractSocket::ConnectedState))
    {
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray,QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "正在连接中，请勿重复点击！\n");
        return;
    }

    // 1) 先把所有输入校验完，避免创建了 socket 又要回收
    bool ok = false;
    const quint16 serverPort = ui->lineEditPort->text().toUShort(&ok);
    if(!ok || serverPort < 1){
        mInsertColor(Qt::red, "服务端端口非法！范围 1~65535\n");
        return;
    }

    const QString localPortStr = ui->lineEditLocalPort->text().trimmed();
    quint16 localPort = 0;
    if(!localPortStr.isEmpty()){
        localPort = localPortStr.toUShort(&ok);
        if(!ok || localPort < 1){
            QString timeStr = QTime::currentTime().toString("hh:mm:ss");
            mInsertColor(Qt::gray,QString("[%1]->").arg(timeStr));
            mInsertColor(Qt::black, "本地端口输入非法！范围1~65535\n");
            return;
        }
    }

    // 2) 每次点击连接，新建QTcpSocket
    client = new QTcpSocket(this);

    // 3) 需要的话绑定本地源端口（bind 必须放在 connectToHost 前面）
    if(localPort != 0){
        if(!client->bind(QHostAddress::Any, localPort)){
            mInsertColor(Qt::black, QString("绑定本地端口%1失败，端口被占用！\n").arg(int(localPort)));
            qDebug() << "bind error:" << client->errorString();
            client->deleteLater();
            client = nullptr;
            return;
        }
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, QString("已指定客户端本地端口：%1\n").arg(int(localPort)));
    }

    m_parser.reset();           // ★ 新连接必须清空拆包缓冲区，不能让上一个连接的残包串进来
    m_timer.start();            // 启动计时！
    m_connTimer->start(5000);   // 5秒超时定时器

    QTcpSocket *sock = client;  // 用局部变量给 lambda 捕获，避免误操作到"后来的"client

    connect(sock,SIGNAL(readyRead()),this,SLOT(mRead_Data_From_Server()));

    connect(sock,&QTcpSocket::connected,this,[=](){
        m_connTimer->stop();            //连接成功，停止超时定时器
        m_heartbeatTimer->start();      //开始心跳
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "!!客户端: 连接服务端成功!!\n");
        setUiConnected(true);
    });

    connect(sock,&QTcpSocket::disconnected,this,[=](){
        m_connTimer->stop();            //断开，停止超时定时器
        m_heartbeatTimer->stop();       //停止心跳
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "??客户端: 与服务端断开连接??\n");
        setUiConnected(false);
        if(client == sock)              // 只有当前连接才置空，避免误清掉新建的连接
            client = nullptr;
        sock->deleteLater();
    });

    connect(sock,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(mConnect_err(QAbstractSocket::SocketError)));

    // 函数原型 发起连接
    //void QTcpSocket::connectToHost(const QHostAddress &address, quint16 port);
    sock->connectToHost(ui->lineEditAddr->text(), serverPort);
}


// 读取数据：★ 同样不再把一次 readAll 当成一条消息
void Widget::mRead_Data_From_Server()
{
    QTcpSocket *tmpSock = qobject_cast<QTcpSocket*>(sender());
    if(!tmpSock)
        return;

    const QByteArray revData = tmpSock->readAll();
    if(revData.isEmpty())
        return;

    const NetProto::MessageParser::Result res = m_parser.append(revData);

    if(res.protocolError)
        mInsertColor(Qt::red, QString("[协议错误] %1\n").arg(res.errorText));

    for(const NetProto::Message &msg : res.messages)
        handleMessage(msg);
}


// 处理一条完整的应用层消息（按 Type 分发）
void Widget::handleMessage(const NetProto::Message &msg)
{
    const QString timeStr = QTime::currentTime().toString("hh:mm:ss");

    switch(msg.type)
    {
    case NetProto::MsgText:
        qDebug() << "RevData: " << msg.text();
        mInsertColor(Qt::gray,QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::darkGreen,QString("服务端：%1\n").arg(msg.text()));
        break;

    case NetProto::MsgPong:
        // 心跳应答：说明链路还活着。用暗灰色，避免每 10 秒刷一片屏
        mInsertColor(Qt::darkGray, QString("[心跳] 服务端应答正常（%1）\n").arg(timeStr));
        break;

    case NetProto::MsgPing:
        // 正常情况下服务端不发 Ping，真收到就回一个 Pong
        sendMessage(NetProto::MsgPong, QByteArray());
        break;

    case NetProto::MsgSystem:
        mInsertColor(Qt::darkYellow, QString("[%1] [系统通知] %2\n").arg(timeStr).arg(msg.text()));
        break;

    default:
        mInsertColor(Qt::red, QString("[%1] 收到未知类型消息 0x%2（正文 %3 字节）\n")
                                  .arg(timeStr)
                                  .arg(int(msg.type), 2, 16, QChar('0'))
                                  .arg(msg.body.size()));
        break;
    }
}


// 打包成「一帧」再发送
void Widget::sendMessage(quint8 type, const QByteArray &body)
{
    if(!client || client->state() != QAbstractSocket::ConnectedState){
        mInsertColor(Qt::black, "当前未连接，无法发送!\n");
        return;
    }

    const QByteArray frame = NetProto::encode(type, body);   // ★ 统一加帧头
    const qint64 ret = client->write(frame);
    if(ret != frame.size()){
        mInsertColor(Qt::red, QString("发送失败：%1\n").arg(client->errorString()));
        return;
    }

    if(type == NetProto::MsgText){    // 心跳帧不打印，避免刷屏
        mInsertColor(Qt::gray,QString("[%1]->").arg(QTime::currentTime().toString("hh:mm:ss")));
        mInsertColor(Qt::blue,QString("我：%1\n").arg(QString::fromUtf8(body)));
    }
}


void Widget::on_btnSend_clicked()       //发送按钮
{
    if(!client || client->state() != QAbstractSocket::ConnectedState){
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray,QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "当前未连接，无法发送!\n");
        return;
    }

    const QString sendStr = ui->textEditSend->toPlainText();
    if(sendStr.isEmpty()){
        mInsertColor(Qt::black, "发送信息不能为空\n");
        return;
    }

    sendMessage(NetProto::MsgText, sendStr.toUtf8());
    ui->textEditSend->clear();      // 发送后清空输入框
}

void Widget::mConnect_err(QAbstractSocket::SocketError err)     //报错提示
{
    m_connTimer->stop();        //连接出错，关闭超时定时器
    m_heartbeatTimer->stop();

    QTcpSocket *sock = qobject_cast<QTcpSocket*>(sender());
    if(!sock)
        return;

    const qint64 ms = m_timer.elapsed();    //获取从start到现在的毫秒
    const QString errMsg = sock->errorString();
    qDebug() << "连接耗时(ms):" << ms << " 错误文本:" << errMsg;

    QMessageBox msgBox;
    if(err == QAbstractSocket::RemoteHostClosedError){
        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray,QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "服务端主动关闭连接\n");
    }
    else if(err == QAbstractSocket::HostNotFoundError)
    {
        msgBox.setWindowTitle("连接失败");
        msgBox.setText("找不到这个IP地址，请检查IP！");
        msgBox.exec();
    }
    else if(err == QAbstractSocket::ConnectionRefusedError)
    {
        msgBox.setWindowTitle("连接失败");
        msgBox.setText("连接被拒绝，请确认服务端已开启监听、端口号正确！");
        msgBox.exec();
    }
    else
    {
        msgBox.setWindowTitle("连接失败");
        msgBox.setText("网络错误：" + errMsg);
        msgBox.exec();
    }

    // 连接失败后 socket 已经不可用：回收它，并把界面恢复成可以重试
    if(sock->state() == QAbstractSocket::UnconnectedState){
        setUiConnected(false);
        if(client == sock){
            client = nullptr;
            sock->deleteLater();
        }
    }
}

void Widget::on_btndisConnect_clicked()     //断开按钮
{
    if(!client){                            // 防御：未连接时点击不再崩溃
        mInsertColor(Qt::black, "当前未连接\n");
        return;
    }
    m_heartbeatTimer->stop();
    client->disconnectFromHost();           // 比 close() 温和：让排队中的数据先发完
}

void Widget::onConnTimeout()
{
    // 超时触发：还在正在连接状态
    if(client && client->state() == QAbstractSocket::ConnectingState)
    {
        QTcpSocket *sock = client;
        client = nullptr;       // ★ 先摘掉成员，避免回调访问到正在销毁的 socket

        // ★ 再摘掉这个 socket 到本窗口的全部信号连接：
        //   abort() 有可能触发 error() / disconnected()，而界面状态本函数已经处理完了，
        //   不需要（也不应该）再走一遍错误提示。直接断开信号比判断具体错误码更稳。
        disconnect(sock, nullptr, this, nullptr);

        sock->abort();          // 强制断开连接，和close不一样，abort立刻终止
        sock->deleteLater();

        QString timeStr = QTime::currentTime().toString("hh:mm:ss");
        mInsertColor(Qt::gray, QString("[%1]->").arg(timeStr));
        mInsertColor(Qt::black, "连接超时！5s未能连上服务端\n");

        // 恢复UI到初始状态
        setUiConnected(false);
    }
}

void Widget::onHeartbeatTimeout()
{
    if(client && client->state() == QAbstractSocket::ConnectedState)
        sendMessage(NetProto::MsgPing, QByteArray());
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
