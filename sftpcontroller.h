#ifndef SFTPCONTROLLER_H
#define SFTPCONTROLLER_H

#include <QHash>
#include <QObject>

#include "sftpsession.h"
#include "sftpfilemodel.h"

// Controller: nơi DUY NHẤT "biết" cả SftpSession/SftpFileModel (Model)
// lẫn phát tín hiệu cho MainWindow (View) cập nhật giao diện.
// Chứa toàn bộ business logic đã nằm rải rác trong MainWindow trước đây:
// - quyết định double-click vào file thì play luôn hay phải tải về trước
//   (logic cũ trong on_treeView_doubleClicked)
// - duyệt thư mục, rename / download / refresh
class SftpController : public QObject
{
    Q_OBJECT
public:
    enum class EntryKind { File, Directory, Parent };

    explicit SftpController(QObject *parent = nullptr);

    QAbstractItemModel *fileModel() const { return m_fileModel->proxyModel(); }
    bool isConnected() const { return m_session->isConnected(); }
    bool isRunning() const { return m_session->isRunning(); }
    QString currentPath() const { return m_currentPath; }

    void toggleConnection();
    void refresh();

    // name: tên mục được double-click trong treeView remote.
    // - Thư mục / "..": đổi thư mục hiện tại rồi liệt kê lại.
    // - File đã có ở local hoặc thư mục tạm -> videoReadyToPlay() phát ngay.
    // - File chưa có -> "get" về thư mục tạm; khi sftp báo xong mới phát videoReadyToPlay().
    void openEntry(const QString &name, EntryKind kind);

    void downloadToLocal(const QString &fileName);
    void renameRemote(const QString &oldName, const QString &newName);

signals:
    void connectionStateChanged(bool connected);
    void connectionFailed(const QString &reason);
    void videoReadyToPlay(const QString &localPath);
    void downloadFinished(const QString &localPath);
    void downloadFailed(const QString &fileName);

private slots:
    void handleStarted();
    void handleEntryParsed(const QString &name, bool isDir);
    void handleCommandFinished(int token);
    void handleDisconnected();

private:
    struct PendingCommand
    {
        enum Type { Download, Refresh } type = Refresh;
        QString fileName;
        QString partPath;   // file tải dở (.part), chỉ đổi tên khi sftp báo xong
        QString finalPath;
        bool playWhenDone = false;
    };

    void listCurrentDirectory();
    void startDownload(const QString &fileName, const QString &finalPath, bool playWhenDone);
    QString remotePath(const QString &name) const;
    static QString joinPath(const QString &dir, const QString &name);
    static QString quoted(const QString &path);

    SftpSession *m_session;
    SftpFileModel *m_fileModel;
    QString m_rootPath;     // remote_defaultPath trong config
    QString m_currentPath;  // thư mục đang xem
    QHash<int, PendingCommand> m_pending;
};

#endif // SFTPCONTROLLER_H
