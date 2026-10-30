#ifndef VAULTCONTROLLER_H
#define VAULTCONTROLLER_H

#include "remotestore.h"
#include "vaultservice.h"
#include "vaulttypes.h"

#include <QFuture>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

class QIODevice;
class ServerLogModel;
class VideoProbe;

// Controller của kho mã hoá: nối View (VaultWidget) với lõi VaultService.
// Mọi thao tác chạm tới server chạy trên một QThreadPool 1 luồng (tuần tự, không
// chặn UI); kết quả quay về luồng UI qua QFuture::then(this, ...).
class VaultController : public QObject
{
    Q_OBJECT
public:
    explicit VaultController(QObject *parent = nullptr);
    ~VaultController() override;

    bool isOpen() const { return m_open; }
    bool isBusy() const { return m_busy > 0; }
    QString username() const { return m_username; }
    QString fingerprint() const { return m_fingerprint; }
    QString serverDescription() const;
    bool needsSshPassword() const;
    int bucketSize() const;
    ServerLogModel *serverLog() const { return m_log; }
    std::shared_ptr<VaultService> service() const { return m_service; }

    void openVault(const QString &username, const QString &passphrase, const QString &sshPassword);
    void confirmHostKey(bool accepted);
    void createPendingVault(const QString &confirmPassphrase);
    void cancelPending();
    void lock();

    void upload(const QString &localPath);
    void reloadLibrary();
    void rename(const VideoMeta &meta, const QString &newName);
    void remove(const VideoMeta &meta);
    void play(const VideoMeta &meta);
    void listServer();
    void reportStats(const QueryStats &stats) { emit statsUpdated(stats); }

    // Chạy work() trên luồng nền của kho rồi gọi done(kết quả) trên luồng UI
    template <typename Work, typename Done>
    void runTask(const QString &message, Work work, Done done)
    {
        using Result = std::invoke_result_t<Work>;
        setBusy(true, message);
        QtConcurrent::run(&m_pool, std::move(work)).then(this, [this, done = std::move(done)](const Result &result) {
            setBusy(false);
            done(result);
        });
    }

signals:
    void busyChanged(bool busy, const QString &message);
    void hostKeyConfirmationNeeded(const QString &host, const QString &fingerprint);
    void userNotFound(const QString &username);
    void opened(const QString &username, const QString &fingerprint);
    void aboutToLock();
    void locked();
    void failed(const QString &message);
    void libraryLoaded(const QList<VideoMeta> &items);
    void statsUpdated(const QueryStats &stats);
    void uploadProgress(qint64 done, qint64 total);
    void uploaded(const VideoMeta &meta);
    void changed();
    void videoReady(QIODevice *device, const QString &title);
    void serverListed(const QList<StoreEntry> &entries);

private:
    void setBusy(bool busy, const QString &message = QString());
    void startOpen(bool acceptHostKey, bool create);
    void startNextUpload();
    void clearPending();

    QThreadPool m_pool;
    std::shared_ptr<RemoteStore> m_store;
    std::shared_ptr<VaultService> m_service;
    ServerLogModel *m_log;
    VideoProbe *m_probe;
    QStringList m_uploadQueue;
    bool m_uploading = false;
    int m_busy = 0;
    bool m_open = false;
    QString m_username;
    QString m_fingerprint;
    QString m_pendingUser;
    QString m_pendingPassphrase;
};

#endif // VAULTCONTROLLER_H
