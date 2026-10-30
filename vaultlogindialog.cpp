#include "vaultlogindialog.h"
#include "ui_vaultlogindialog.h"

#include <QMessageBox>

VaultLoginDialog::VaultLoginDialog(const QString &serverText, bool askSshPassword, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::VaultLoginDialog)
{
    ui->setupUi(this);
    ui->label_Server->setText(QStringLiteral("SFTP: %1").arg(serverText));
    ui->label_SshPassword->setVisible(askSshPassword);
    ui->lineEdit_SshPassword->setVisible(askSshPassword);
    ui->lineEdit_User->setFocus();
}

VaultLoginDialog::~VaultLoginDialog()
{
    delete ui;
}

QString VaultLoginDialog::username() const
{
    return ui->lineEdit_User->text().trimmed();
}

QString VaultLoginDialog::passphrase() const
{
    return ui->lineEdit_Passphrase->text();
}

QString VaultLoginDialog::sshPassword() const
{
    return ui->lineEdit_SshPassword->text();
}

void VaultLoginDialog::accept()
{
    if (username().isEmpty() || passphrase().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Hãy nhập người dùng và passphrase."));
        return;
    }
    if (ui->lineEdit_SshPassword->isVisible() && sshPassword().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Hãy nhập mật khẩu SSH của server."));
        return;
    }
    QDialog::accept();
}
