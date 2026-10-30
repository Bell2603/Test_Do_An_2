#ifndef SFTPSESSION_H
#define SFTPSESSION_H

#include <QObject>
#include <QProcess>

// Model: bọc QProcess chạy sftp (qua sshpass, hoặc bằng khoá SSH), và parse output thô (text)
// thành dữ liệu có cấu trúc (tên file + có phải thư mục hay không).
// Đây chính là phần logic của onReadyRead() cũ trong MainWindow - việc parse
// dữ liệu là việc của Model, không phải của View.
class SftpSession : public QObject
{
    Q_OBJECT
public:
    explicit SftpSession(QObject *parent = nullptr);

    bool isRunning() const;
    // true khi sftp đã đăng nhập xong (không chỉ là tiến trình đã khởi động)
    bool isConnected() const { return m_connected; }
    // keyPath khác rỗng -> dùng khoá SSH, không cần sshpass
    void connectToServer(const QString &host, const QString &port,
                          const QString &user, const QString &password,
                          const QString &keyPath = QString());
    void disconnectFromServer();
    void sendCommand(const QString &cmd);
    // Gửi lệnh kèm một lệnh đánh dấu; commandFinished(token) phát ra khi sftp đã chạy XONG lệnh
    int sendTrackedCommand(const QString &cmd);

signals:
    void entryParsed(const QString &name, bool isDir);
    void startFailed(const QString &reason);
    void started();
    void commandFinished(int token);
    void disconnected();

private slots:
    void onReadyRead();
    void onReadyReadError();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void handleLine(const QString &line);
    void markConnected();

    QProcess *m_process;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QString m_lastErrorLine;
    bool m_connected = false;
    bool m_usingSshpass = false;
    bool m_stopRequested = false;
    int m_nextToken = 1;
};

#endif // SFTPSESSION_H
