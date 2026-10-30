#ifndef VAULTSERVICE_H
#define VAULTSERVICE_H

#include "bucketcache.h"
#include "encryptedbtree.h"
#include "keymanager.h"
#include "objectstore.h"
#include "vaulttypes.h"

#include <QHash>
#include <QMutex>

#include <functional>
#include <memory>

class DecryptingDevice;
class QIODevice;

// Lõi của kho video mã hoá đầu-cuối theo mô hình ZeroDB. Không có UI, không biết
// server là SFTP hay bộ nhớ (IObjectStore) nên kiểm thử được trọn vẹn.
// Mọi hàm đều chặn (blocking) và an toàn luồng; controller gọi chúng qua QtConcurrent.
//
// Bố cục trên server (gốc vault):
//   obj/<oid>                  video mã hoá theo chunk
//   b/<id>                     bucket B-tree mã hoá (dùng chung cho mọi chỉ mục)
//   users/<uid>/salt|verify|pub|identity|root
//   users/<uid>/meta/<oid>     metadata mã hoá
//   users/<uid>/keyring/<oid>  CEK đã bọc
//   users/<uid>/inbox/<sid>    chia sẻ đến (niêm phong X25519)
//   users/<uid>/outbox/<sid>   lượt đã chia sẻ đi (để thu hồi)
class VaultService
{
public:
    static const QStringList IndexNames;
    using Progress = std::function<void(qint64 done, qint64 total)>;

    VaultService(std::shared_ptr<IObjectStore> store, const VaultSettings &settings);
    ~VaultService();

    static QString userIdFor(const QString &username);

    bool userExists(const QString &username, QString *error);
    bool createUser(const QString &username, const QString &passphrase, QString *error);
    bool open(const QString &username, const QString &passphrase, QString *error);
    void lock();
    bool isOpen() const;
    QString username() const;
    QString fingerprint() const;
    qint64 videoCount() const;

    bool upload(QIODevice *source, const QString &name, qint64 durationMs, const QString &mime,
                VideoMeta *result, QString *error, const Progress &progress = Progress());
    bool uploadFile(const QString &path, qint64 durationMs, VideoMeta *result, QString *error,
                    const Progress &progress = Progress());
    bool rename(const QByteArray &oid, const QString &newName, QString *error);
    bool remove(const QByteArray &oid, QString *error);

    // Tải toàn bộ chỉ mục ngày theo tầng (Fig. 4a) rồi lấy metadata
    QueryResult library();
    QueryResult query(const VaultQuery &query);

    bool lookupUser(const QString &username, QByteArray *publicKey, QString *error);
    bool share(const QByteArray &oid, const QString &recipient, QString *shareId, QString *error);
    bool revoke(const QString &shareId, QString *error);
    QList<ShareRecord> outgoingShares(QString *error);
    QList<VideoMeta> inbox(QString *error);

    // Trả thiết bị giải mã đã mở (người gọi sở hữu). Không ghi plaintext xuống đĩa.
    DecryptingDevice *openVideo(const VideoMeta &meta, QString *error);

    std::shared_ptr<IObjectStore> store() const { return m_store; }
    const VaultSettings &settings() const { return m_settings; }

private:
    struct RootPointer
    {
        quint64 version = 0;
        qint64 count = 0;
        QHash<QString, QByteArray> roots;
        QHash<QString, qint64> entries;  // số phần tử của từng chỉ mục (cho ước lượng contour)
    };
    struct IndexChange
    {
        QString index;
        BTreeEntry entry;
    };

    QString userPath(const QString &relative) const;
    bool isOpenUnlocked() const { return m_keys.isReady() && !m_uid.isEmpty(); }
    bool loadRoot(RootPointer *root, QString *error);
    bool saveRoot(const RootPointer &root, QString *error);
    bool applyIndexChanges(const QList<IndexChange> &removals, const QList<IndexChange> &insertions,
                           qint64 countDelta, QueryStats *stats, QString *error);
    static QList<IndexChange> indexEntriesFor(const VideoMeta &meta);
    static QList<IndexChange> nameEntriesFor(const VideoMeta &meta);

    bool readMeta(const QByteArray &oid, VideoMeta *meta, QString *error);
    bool writeMeta(const VideoMeta &meta, QString *error);
    QList<VideoMeta> fetchMetas(const QList<QByteArray> &oids, QueryStats *stats, QString *error);
    bool readCek(const QByteArray &oid, QByteArray *cek, QString *error);

    bool runRange(const QString &index, const QByteArray &lo, const QByteArray &hi, int limit,
                  QList<BTreeEntry> *out, QueryStats *stats, QString *error);
    static bool matches(const VideoMeta &meta, const VaultQuery &query);
    static bool conditionRange(const VaultQuery &query, QString *index, QByteArray *lo, QByteArray *hi);
    qint64 indexSize(const QString &index) const;

    std::shared_ptr<IObjectStore> m_store;
    VaultSettings m_settings;
    std::unique_ptr<BucketCache> m_cache;
    std::unique_ptr<EncryptedBTree> m_tree;
    KeyManager m_keys;
    QString m_username;
    QString m_uid;
    RootPointer m_root;
    QHash<QByteArray, VideoMeta> m_metaCache;
    QHash<QString, QByteArray> m_sharedKeys; // shareId -> CEK của video được chia sẻ
    QHash<QString, VideoMeta> m_sharedMetas;
    mutable QRecursiveMutex m_mutex;
};

#endif // VAULTSERVICE_H
