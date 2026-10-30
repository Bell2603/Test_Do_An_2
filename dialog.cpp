#include "dialog.h"
#include "ui_dialog.h"

#include <QFileInfo>

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Dialog)
{
    ui->setupUi(this);
}

Dialog::~Dialog()
{
    delete ui;
}

QString Dialog::NewName_Test()
{
    return ui->lineEdit_Name->text().trimmed();
}

void Dialog::setInitialName(const QString &name)
{
    ui->lineEdit_Name->setText(QFileInfo(name).completeBaseName());
    ui->lineEdit_Name->selectAll();
}

void Dialog::on_pushButton_OK_clicked()
{
    accept();
}

void Dialog::on_pushButton_Cancel_clicked()
{
    reject();
}
