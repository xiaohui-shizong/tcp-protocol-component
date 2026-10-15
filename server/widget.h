#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QTime>

#include "mycombobox.h"
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

    QTcpServer* server;

public slots:
    void on_newClient_connect();
    void on_readyRead_handler();
    void mdisconnected();
    void mComboBox_refresh();

private slots:
    void on_btnListen_clicked();

    void on_btnSend_clicked();

    void on_btnStop_clicked();

    void on_btnLineout_clicked();

private:
    Ui::Widget *ui;

    // 拆包器是按「连接」隔离的：每个 socket 一个，key 就是 socket 指针
    QHash<QTcpSocket*, NetProto::MessageParser> m_parsers;

    void mInsertColor(Qt::GlobalColor color, QString str);

    /* ---------------------------- 协议相关 ---------------------------- */
    void handleMessage(QTcpSocket *sock, const NetProto::Message &msg);       // 处理一条完整消息
    bool sendMessage(QTcpSocket *sock, quint8 type, const QByteArray &body);  // 打包成帧并发送
    void sendToAll(quint8 type, const QByteArray &body);                     // 给所有在线客户端群发
    void handleProtocolError(QTcpSocket *sock, const QString &err);           // 协议错误日志
};
#endif // WIDGET_H
