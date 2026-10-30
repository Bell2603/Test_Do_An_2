#include "vaultcontroller.h"
#include "appconfig.h"
#include "decryptingdevice.h"
#include "serverlogmodel.h"
#include "videoprobe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>

namespace {

struct OpenResult
{
    bool ok = false;
    bool hostKeyUnknown = false;
    bool userMissing = false;
    QString hostFingerprint;
    QString error;
    QString userFingerprint;
    std::shared_ptr<RemoteStore> store;
    std::shared_ptr<VaultService> service;
};

struct TaskResult
{
    bool ok = false;
    QString error;
};

struct UploadResult
{
    bool ok = false;
    QString error;
    VideoMeta meta;
};

struct PlayResult
{
    QIODevice *device = nullptr;
    QString error;
};

QString appDataPath(const QString &file)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + file;
}

} // namespace

VaultController::VaultController(QObject *parent)
    : QObject(parent)
    , m_log(new ServerLogModel(this))
    , m_probe(new VideoProbe(this))
{
    m_pool.setMaxThreadCount(1);
    connect(m_probe, &VideoProbe::finished, this, [this](const QString &path, qint64 durationMs) {
        auto service = m_service;
        if (!service) {
            m_uploading = false;
            return;
        }
        QPointer<VaultController> self(this);
        const QString name = QFileInfo(path).fileName();
        runTask(
            QStringLiteral("Đang mã hoá và upload %1…").arg(name),
            [service, path, durationMs, self]() {
                UploadResult result;
                result.ok = service->uploadFile(path, durationMs, &result.meta, &result.error,
                                                [self](qint64 done, qint64 total) {
                                                    QMetaObject::invokeMethod(
                                                        self.data(),
                                                        [self, done, total] {
                                                            if (self)
                                                                emit self->uploadProgress(done, total);
                                                        },
                                                        Qt::QueuedConnection);
                                                });
                return result;
            },
            [this](const UploadResult &result) {
                if (result.ok) {
                    emit uploaded(result.meta);
                    reloadLibrary();
                } else {
                    emit failed(result.error);
                }
                startNextUpload();
            });
    });
}

VaultController::~VaultController()
{
    m_pool.waitForDone();
    if (m_store)
        m_store->setObserver({});
    if (m_service)
        m_service->lock();
}

QString VaultController::serverDescription() const
{
    const auto &config = AppConfig::instance();
    return QStringLiteral("%1@%2:%3 — %4").arg(config.user(), config.host(), config.port(), config.vaultRemotePath());
}

bool VaultController::needsSshPassword() const
{
    const auto &config = AppConfig::instance();
    return config.password().isEmpty() && config.keyPath().isEmpty();
}

int VaultController::bucketSize() const
{
    return AppConfig::instance().bucketSize();
}

void VaultController::setBusy(bool busy, const QString &message)
{
    m_busy = qMax(0, m_busy + (busy ? 1 : -1));
    emit busyChanged(m_busy > 0, busy ? message : QString());
}

void VaultController::clearPending()
{
    m_pendingUser.clear();
    // QString không có wipe an toàn như QByteArray; ghi đè trước khi giải phóng
    m_pendingPassphrase.fill(QLatin1Char(' '));
    m_pendingPassphrase.clear();
}

void VaultController::openVault(const QString &username, const QString &passphrase, const QString &sshPassword)
{
    if (isBusy() || m_open)
        return;
    if (!sshPassword.isEmpty())
        AppConfig::instance().setSessionPassword(sshPassword);
    m_pendingUser = username.trimmed();
    m_pendingPassphrase = passphrase;
    startOpen(false, false);
}

void VaultController::confirmHostKey(bool accepted)
{
    if (!accepted) {
        clearPending();
        emit failed(QStringLiteral("Đã huỷ: host key chưa được tin cậy"));
        return;
    }
    startOpen(true, false);
}

void VaultController::createPendingVault(const QString &confirmPassphrase)
{
    if (confirmPassphrase != m_pendingPassphrase) {
        clearPending();
        emit failed(QStringLiteral("Passphrase nhập lại không khớp"));
        return;
    }
    startOpen(false, true);
}

void VaultController::cancelPending()
{
    clearPending();
}

void VaultController::startOpen(bool acceptHostKey, bool create)
{
    const auto &config = AppConfig::instance();
    RemoteStoreConfig remote;
    remote.host = config.host();
    remote.port = config.port().toInt() > 0 ? config.port().toInt() : 22;
    remote.user = config.user();
    remote.password = config.password();
    remote.keyPath = config.keyPath();
    remote.rootPath = config.vaultRemotePath();
    remote.knownHostsFile = appDataPath(QStringLiteral("known_hosts"));

    VaultSettings settings;
    settings.bucketSize = config.bucketSize();
    settings.chunkSize = config.chunkSizeKB() * 1024;
    settings.ramCacheBytes = qint64(config.cacheSizeMB()) * 1024 * 1024;
    settings.diskCachePath = appDataPath(QStringLiteral("cache.sqlite"));
    settings.cacheScope = QStringLiteral("%1:%2:%3").arg(remote.host).arg(remote.port).arg(remote.rootPath);

    if (remote.rootPath.isEmpty()) {
        clearPending();
        emit failed(QStringLiteral("Chưa cấu hình [Vault] remotePath trong config.ini"));
        return;
    }

    const QString username = m_pendingUser;
    const QString passphrase = m_pendingPassphrase;
    ServerLogModel *log = m_log;
    runTask(
        create ? QStringLiteral("Đang tạo kho (scrypt + X25519)…") : QStringLiteral("Đang kết nối và mở kho…"),
        [remote, settings, username, passphrase, acceptHostKey, create, log]() {
            OpenResult result;
            auto store = std::make_shared<RemoteStore>(remote);
            store->setObserver([log](const StoreOperation &op) {
                QMetaObject::invokeMethod(log, [log, op] { log->append(op); }, Qt::QueuedConnection);
            });
            RemoteStore::HostKeyStatus status = RemoteStore::HostKeyStatus::Failure;
            if (!store->connectToServer(acceptHostKey, &status, &result.hostFingerprint)) {
                result.hostKeyUnknown = status == RemoteStore::HostKeyStatus::NotFound && !acceptHostKey;
                result.error = store->lastError();
                store->setObserver({});
                return result;
            }
            auto service = std::make_shared<VaultService>(store, settings);
            QString error;
            if (create) {
                result.ok = service->createUser(username, passphrase, &error);
            } else if (!service->userExists(username, &error)) {
                result.userMissing = error.isEmpty();
                result.error = error;
            } else {
                result.ok = service->open(username, passphrase, &error);
            }
            if (!result.ok) {
                if (result.error.isEmpty())
                    result.error = error;
                store->disconnectFromServer();
                store->setObserver({});
                return result;
            }
            result.userFingerprint = service->fingerprint();
            result.store = store;
            result.service = service;
            return result;
        },
        [this, username, remote](const OpenResult &result) {
            if (result.hostKeyUnknown) {
                emit hostKeyConfirmationNeeded(remote.host, result.hostFingerprint);
                return;
            }
            if (result.userMissing) {
                emit userNotFound(username);
                return;
            }
            clearPending();
            if (!result.ok) {
                emit failed(result.error);
                return;
            }
            m_store = result.store;
            m_service = result.service;
            m_open = true;
            m_username = username;
            m_fingerprint = result.userFingerprint;
            emit opened(m_username, m_fingerprint);
            reloadLibrary();
        });
}

void VaultController::lock()
{
    if (!m_open)
        return;
    if (isBusy()) {
        emit failed(QStringLiteral("Đang có thao tác chạy, hãy chờ xong rồi khoá kho"));
        return;
    }
    emit aboutToLock();
    m_uploadQueue.clear();
    m_service->lock();
    m_store->disconnectFromServer();
    m_store->setObserver({});
    m_service.reset();
    m_store.reset();
    m_open = false;
    m_username.clear();
    m_fingerprint.clear();
    emit locked();
}

void VaultController::upload(const QString &localPath)
{
    if (!m_open) {
        emit failed(QStringLiteral("Hãy mở kho trước khi upload"));
        return;
    }
    m_uploadQueue.append(localPath);
    if (!m_uploading)
        startNextUpload();
}

void VaultController::startNextUpload()
{
    if (m_uploadQueue.isEmpty() || !m_open) {
        m_uploading = false;
        return;
    }
    m_uploading = true;
    // Đọc thời lượng trước (luồng UI), việc mã hoá + upload chạy nền sau đó
    m_probe->probe(m_uploadQueue.takeFirst());
}

void VaultController::reloadLibrary()
{
    auto service = m_service;
    if (!service)
        return;
    runTask(
        QStringLiteral("Đang tải thư viện từ chỉ mục mã hoá…"),
        [service]() { return service->library(); },
        [this](const QueryResult &result) {
            if (!result.ok())
                emit failed(result.error);
            emit libraryLoaded(result.items);
            emit statsUpdated(result.stats);
        });
}

void VaultController::rename(const VideoMeta &meta, const QString &newName)
{
    auto service = m_service;
    if (!service)
        return;
    runTask(
        QStringLiteral("Đang đổi tên (cập nhật chỉ mục)…"),
        [service, meta, newName]() {
            TaskResult result;
            result.ok = service->rename(meta.oid, newName, &result.error);
            return result;
        },
        [this](const TaskResult &result) {
            if (!result.ok)
                emit failed(result.error);
            emit changed();
            reloadLibrary();
        });
}

void VaultController::remove(const VideoMeta &meta)
{
    auto service = m_service;
    if (!service)
        return;
    runTask(
        QStringLiteral("Đang xoá video khỏi kho…"),
        [service, meta]() {
            TaskResult result;
            result.ok = service->remove(meta.oid, &result.error);
            return result;
        },
        [this](const TaskResult &result) {
            if (!result.ok)
                emit failed(result.error);
            emit changed();
            reloadLibrary();
        });
}

void VaultController::play(const VideoMeta &meta)
{
    auto service = m_service;
    if (!service)
        return;
    runTask(
        QStringLiteral("Đang mở video mã hoá…"),
        [service, meta]() {
            PlayResult result;
            DecryptingDevice *device = service->openVideo(meta, &result.error);
            if (device) {
                // Thiết bị được tạo ở luồng nền -> chuyển về luồng UI cho QMediaPlayer
                device->moveToThread(QCoreApplication::instance()->thread());
                result.device = device;
            }
            return result;
        },
        [this, meta](const PlayResult &result) {
            if (!result.device) {
                emit failed(result.error);
                return;
            }
            emit videoReady(result.device, meta.name);
        });
}

void VaultController::listServer()
{
    auto store = m_store;
    if (!store) {
        emit failed(QStringLiteral("Hãy mở kho để liệt kê thư mục trên server"));
        return;
    }
    runTask(
        QStringLiteral("Đang liệt kê thư mục vault trên server…"),
        [store]() {
            QList<StoreEntry> result;
            QStringList pending{QString()};
            while (!pending.isEmpty() && result.size() < 5000) {
                const QString dir = pending.takeFirst();
                for (const StoreEntry &entry : store->list(dir)) {
                    StoreEntry item = entry;
                    item.name = dir.isEmpty() ? entry.name : dir + QLatin1Char('/') + entry.name;
                    if (entry.isDir)
                        pending.append(item.name);
                    result.append(item);
                }
            }
            return result;
        },
        [this](const QList<StoreEntry> &entries) { emit serverListed(entries); });
}
