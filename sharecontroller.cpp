#include "sharecontroller.h"
#include "cryptoservice.h"
#include "vaultcontroller.h"

namespace {

struct LookupResult
{
    QString fingerprint;
    QString error;
};

struct ShareResult
{
    bool ok = false;
    QString error;
};

struct InboxResult
{
    QList<VideoMeta> items;
    QString error;
};

struct OutgoingResult
{
    QList<ShareRecord> records;
    QString error;
};

} // namespace

ShareController::ShareController(VaultController *vault, QObject *parent)
    : QObject(parent)
    , m_vault(vault)
{
}

void ShareController::lookup(const QString &recipient)
{
    auto service = m_vault->service();
    if (!service)
        return;
    m_vault->runTask(
        QStringLiteral("Đang tra khoá công khai của %1…").arg(recipient),
        [service, recipient]() {
            LookupResult result;
            QByteArray publicKey;
            if (service->lookupUser(recipient, &publicKey, &result.error))
                result.fingerprint = CryptoService::fingerprint(publicKey);
            return result;
        },
        [this, recipient](const LookupResult &result) {
            emit recipientLookedUp(recipient, result.fingerprint, result.error);
        });
}

void ShareController::share(const VideoMeta &meta, const QString &recipient)
{
    auto service = m_vault->service();
    if (!service)
        return;
    m_vault->runTask(
        QStringLiteral("Đang bọc khoá video cho %1…").arg(recipient),
        [service, meta, recipient]() {
            ShareResult result;
            result.ok = service->share(meta.oid, recipient, nullptr, &result.error);
            return result;
        },
        [this, meta, recipient](const ShareResult &result) {
            if (result.ok) {
                emit shared(meta.name, recipient);
                loadOutgoing();
            } else {
                emit failed(result.error);
            }
        });
}

void ShareController::revoke(const QString &shareId)
{
    auto service = m_vault->service();
    if (!service)
        return;
    m_vault->runTask(
        QStringLiteral("Đang thu hồi chia sẻ…"),
        [service, shareId]() {
            ShareResult result;
            result.ok = service->revoke(shareId, &result.error);
            return result;
        },
        [this](const ShareResult &result) {
            if (result.ok)
                emit revoked();
            else
                emit failed(result.error);
            loadOutgoing();
        });
}

void ShareController::loadInbox()
{
    auto service = m_vault->service();
    if (!service)
        return;
    m_vault->runTask(
        QStringLiteral("Đang mở các video được chia sẻ…"),
        [service]() {
            InboxResult result;
            result.items = service->inbox(&result.error);
            return result;
        },
        [this](const InboxResult &result) {
            if (!result.error.isEmpty())
                emit failed(result.error);
            emit inboxLoaded(result.items);
        });
}

void ShareController::loadOutgoing()
{
    auto service = m_vault->service();
    if (!service)
        return;
    m_vault->runTask(
        QStringLiteral("Đang tải danh sách đã chia sẻ…"),
        [service]() {
            OutgoingResult result;
            result.records = service->outgoingShares(&result.error);
            return result;
        },
        [this](const OutgoingResult &result) {
            if (!result.error.isEmpty())
                emit failed(result.error);
            emit outgoingLoaded(result.records);
        });
}
