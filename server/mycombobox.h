#ifndef MYCOMBOBOX_H
#define MYCOMBOBOX_H

#include <QWidget>
#include <QComboBox>

class MyComboBox : public QComboBox
{

    Q_OBJECT
public:
    MyComboBox(QWidget *parent);

private:
    void mousePressEvent(QMouseEvent *e) ;

signals:
    void on_ComboBox_clicked();

};

#endif // MYCOMBOBOX_H
