#ifndef VAULTLOGINDIALOG_H
#define VAULTLOGINDIALOG_H

#include <QDialog>

namespace Ui {
class VaultLoginDialog;
}

class VaultLoginDialog : public QDialog
{
    Q_OBJECT

public:
    VaultLoginDialog(const QString &serverText, bool askSshPassword, QWidget *parent = nullptr);
    ~VaultLoginDialog() override;

    QString username() const;
    QString passphrase() const;
    QString sshPassword() const;

public slots:
    void accept() override;

private:
    Ui::VaultLoginDialog *ui;
};

#endif // VAULTLOGINDIALOG_H
