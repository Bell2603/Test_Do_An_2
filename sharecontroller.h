#ifndef SHARECONTROLLER_H
#define SHARECONTROLLER_H

#include "vaulttypes.h"

#include <QObject>

class VaultController;

// Controller chia sẻ video (Mục III, bản đơn giản): bọc lại CEK bằng khoá công khai
// X25519 của người nhận. Thu hồi = xoá mục inbox của người nhận.
class ShareController : public QObject
{
    Q_OBJECT
public:
    explicit ShareController(VaultController *vault, QObject *parent = nullptr);

    void lookup(const QString &recipient);
    void share(const VideoMeta &meta, const QString &recipient);
    void revoke(const QString &shareId);
    void loadInbox();
    void loadOutgoing();

signals:
    void recipientLookedUp(const QString &recipient, const QString &fingerprint, const QString &error);
    void shared(const QString &videoName, const QString &recipient);
    void revoked();
    void inboxLoaded(const QList<VideoMeta> &items);
    void outgoingLoaded(const QList<ShareRecord> &records);
    void failed(const QString &message);

private:
    VaultController *m_vault;
};

#endif // SHARECONTROLLER_H
