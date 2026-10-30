#ifndef REMOTESTORE_H
#define REMOTESTORE_H

#include "objectstore.h"

#include <QRecursiveMutex>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

struct RemoteStoreConfig
{
    QString host;
    int port = 22;
    QString user;
    QString password;
    QString keyPath;        // khoá riêng SSH (ưu tiên nếu có)
    QString rootPath;       // thư mục gốc của vault trên server
    QString knownHostsFile; // file known_hosts riêng của ứng dụng (host key đã được người dùng chấp nhận)
};

// IObjectStore trên SFTP bằng libssh2 (thay cho QProcess + sshpass):
// - mật khẩu không xuất hiện trên dòng lệnh, có kiểm tra host key (known_hosts)
// - đọc theo offset (getRange) để phát video theo chunk
// - một phiên blocking duy nhất, mọi thao tác được tuần tự hoá bằng mutex
class RemoteStore : public IObjectStore
{
public:
    enum class HostKeyStatus { Match, NotFound, Mismatch, Failure };

    explicit RemoteStore(const RemoteStoreConfig &config);
    ~RemoteStore() override;

    // acceptNewHostKey: người dùng đã xác nhận fingerprint của host key chưa từng thấy
    bool connectToServer(bool acceptNewHostKey, HostKeyStatus *status, QString *fingerprint);
    void disconnectFromServer();
    bool isConnected() const;
    const RemoteStoreConfig &config() const { return m_config; }

    bool get(const QString &path, QByteArray *data) override;
    bool getRange(const QString &path, qint64 offset, qint64 length, QByteArray *data) override;
    QList<QByteArray> getMany(const QStringList &paths, bool *ok) override;
    bool put(const QString &path, const QByteArray &data) override;
    bool putStream(const QString &path, const Producer &producer) override;
    bool putAtomic(const QString &path, const QByteArray &data) override;
    bool remove(const QString &path) override;
    bool exists(const QString &path) override;
    QList<StoreEntry> list(const QString &dir) override;
    bool removeDirectory(const QString &dir) override;
    QString lastError() const override;
    bool notFound() const override;

private:
    bool connectUnlocked(bool acceptNewHostKey, HostKeyStatus *status, QString *fingerprint);
    void closeUnlocked();
    bool ensureConnected();
    bool isSocketError() const;
    HostKeyStatus checkHostKey(const char *key, size_t keyLength, int keyType);
    bool rememberHostKey(const char *key, size_t keyLength, int keyType);
    bool authenticate();

    QString fullPath(const QString &relative) const;
    QByteArray encodedPath(const QString &relative) const;
    bool readUnlocked(const QString &path, qint64 offset, qint64 length, QByteArray *data);
    bool writeUnlocked(const QString &path, const Producer &producer);
    bool mkdirsUnlocked(const QString &relativeDir);
    bool removeUnlocked(const QString &path);
    bool renameUnlocked(const QString &from, const QString &to);
    QList<StoreEntry> listUnlocked(const QString &dir, bool *ok);
    bool removeTreeUnlocked(const QString &dir);
    void setError(const QString &message);
    QString sessionError() const;
    void report(const QString &op, const QString &path, qint64 in, qint64 out, qint64 elapsedUs, bool ok);

    RemoteStoreConfig m_config;
    mutable QRecursiveMutex m_mutex;
    int m_socket = -1;
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_SFTP *m_sftp = nullptr;
    QByteArray m_passwordBytes; // dùng cho keyboard-interactive callback
    QString m_lastError;
    bool m_notFound = false;
    bool m_hostTrusted = false;  // đã kết nối thành công ít nhất một lần -> cho phép tự nối lại
};

#endif // REMOTESTORE_H
