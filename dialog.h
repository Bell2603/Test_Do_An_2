#ifndef DIALOG_H
#define DIALOG_H

#include <QDialog>

namespace Ui {
class Dialog;
}

class Dialog : public QDialog
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    ~Dialog();

    QString NewName_Test();
    // Điền sẵn tên cũ (không kèm đuôi) để người dùng sửa
    void setInitialName(const QString &name);

private slots:
    void on_pushButton_OK_clicked();

    void on_pushButton_Cancel_clicked();

private:
    Ui::Dialog *ui;
};

#endif // DIALOG_H
