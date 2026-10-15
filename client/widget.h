#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QTime>
#include <QTimer>

#include "protocol.h"      // 自定义协议模块（common/protocol.h）


QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

private slots:
    void on_btnConnect_clicked();
    void mRead_Data_From_Server();

    void on_btnSend_clicked();
    void mConnect_err(QAbstractSocket::SocketError err);
    void on_btndisConnect_clicked();

    void onConnTimeout();       // 连接超时槽函数
    void onHeartbeatTimeout();  // 心跳定时槽函数

private:
    Ui::Widget *ui;
    QTcpSocket *client = nullptr;   //网络对象
    QElapsedTimer m_timer;          //计时器
    QTimer *m_connTimer;            //连接超时定时器
    QTimer *m_heartbeatTimer;       //心跳定时器

    // ★ 本连接专用的拆包器：TCP 是字节流，收到的数据要靠它切成一条条消息
    NetProto::MessageParser m_parser;

    void mInsertColor(Qt::GlobalColor color, QString str);

    void handleMessage(const NetProto::Message &msg);              // 处理一条完整消息
    void sendMessage(quint8 type, const QByteArray &body);         // 打包成帧并发送
    void setUiConnected(bool connected);                          // 统一管理控件可用状态
};
#endif // WIDGET_H
