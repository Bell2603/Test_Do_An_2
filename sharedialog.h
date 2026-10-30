#ifndef SHAREDIALOG_H
#define SHAREDIALOG_H

#include <QDialog>

namespace Ui {
class ShareDialog;
}

// Hộp thoại chia sẻ: người dùng phải tra và xác nhận fingerprint khoá công khai
// của người nhận trước khi CEK được bọc cho họ (chống server tráo khoá công khai).
class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShareDialog(const QString &videoName, QWidget *parent = nullptr);
    ~ShareDialog() override;

    QString recipient() const;
    void setLookupResult(const QString &recipient, const QString &fingerprint, const QString &error);

signals:
    void lookupRequested(const QString &recipient);

private:
    void updateOkButton();

    Ui::ShareDialog *ui;
    QString m_verifiedRecipient;
};

#endif // SHAREDIALOG_H
