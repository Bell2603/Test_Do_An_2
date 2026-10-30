#include "vaultservice.h"
#include "cryptoservice.h"
#include "decryptingdevice.h"

#include <QCborMap>
#include <QCborValue>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <limits>

const QStringList VaultService::IndexNames = {
    QStringLiteral("name"),
    QStringLiteral("word"),
    QStringLiteral("date"),
    QStringLiteral("duration"),
    QStringLiteral("size"),
    QStringLiteral("word_duration"),
};

namespace {

const QByteArray kVerifyPlain("SETP-VAULT-v1");

QString hex(const QByteArray &raw)
{
    return QString::fromLatin1(raw.toHex());
}

QCborMap metaToCbor(const VideoMeta &meta)
{
    QCborMap map;
    map.insert(QStringLiteral("n"), meta.name);
    map.insert(QStringLiteral("t"), meta.uploadedAt);
    map.insert(QStringLiteral("d"), meta.durationMs);
    map.insert(QStringLiteral("s"), meta.size);
    map.insert(QStringLiteral("m"), meta.mime);
    return map;
}

void metaFromCbor(const QCborMap &map, VideoMeta *meta)
{
    meta->name = map.value(QStringLiteral("n")).toString();
    meta->uploadedAt = map.value(QStringLiteral("t")).toInteger();
    meta->durationMs = map.value(QStringLiteral("d")).toInteger();
    meta->size = map.value(QStringLiteral("s")).toInteger();
    meta->mime = map.value(QStringLiteral("m")).toString();
}

bool parseMap(const QByteArray &cbor, QCborMap *map)
{
    QCborParserError error;
    const QCborValue value = QCborValue::fromCbor(cbor, &error);
    if (error.error != QCborError::NoError || !value.isMap())
        return false;
    *map = value.toMap();
    return true;
}

QString keepSuffix(const QString &oldName, const QString &newName)
{
    const QString suffix = QFileInfo(oldName).suffix();
    if (suffix.isEmpty() || newName.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive))
        return newName;
    return newName + QLatin1Char('.') + suffix;
}

} // namespace

VaultService::VaultService(std::shared_ptr<IObjectStore> store, const VaultSettings &settings)
    : m_store(std::move(store))
    , m_settings(settings)
    , m_cache(std::make_unique<BucketCache>(settings.ramCacheBytes, settings.diskCachePath, settings.cacheScope))
{
}

VaultService::~VaultService()
{
    lock();
}

QString VaultService::userIdFor(const QString &username)
{
    const QByteArray normalized = username.trimmed().toCaseFolded().toUtf8();
    return hex(CryptoService::sha256("setp-user:" + normalized)).left(32);
}

QString VaultService::userPath(const QString &relative) const
{
    return QStringLiteral("users/") + m_uid + QLatin1Char('/') + relative;
}

bool VaultService::userExists(const QString &username, QString *error)
{
    QMutexLocker lock(&m_mutex);
    const QString path = QStringLiteral("users/%1/salt").arg(userIdFor(username));
    if (m_store->exists(path))
        return true;
    if (error)
        *error = m_store->lastError();
    return false;
}

bool VaultService::createUser(const QString &username, const QString &passphrase, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (username.trimmed().isEmpty()) {
        *error = QStringLiteral("Tên người dùng không được để trống");
        return false;
    }
    if (passphrase.size() < 8) {
        *error = QStringLiteral("Passphrase cần tối thiểu 8 ký tự");
        return false;
    }
    const QString uid = userIdFor(username);
    if (m_store->exists(QStringLiteral("users/%1/salt").arg(uid))) {
        *error = QStringLiteral("Người dùng '%1' đã có kho").arg(username);
        return false;
    }

    this->lock();
    const QByteArray salt = CryptoService::randomBytes(16);
    if (!m_keys.derive(passphrase, salt)) {
        *error = QStringLiteral("Không dẫn xuất được khoá (scrypt)");
        return false;
    }
    QByteArray privateKey;
    QByteArray publicKey;
    if (!CryptoService::generateX25519(&privateKey, &publicKey)) {
        m_keys.wipe();
        *error = QStringLiteral("Không tạo được cặp khoá X25519");
        return false;
    }
    m_keys.setIdentity(privateKey, publicKey);
    m_uid = uid;
    m_username = username.trimmed();

    const QByteArray verify = CryptoService::encrypt(m_keys.verifyKey(), kVerifyPlain, "verify:" + uid.toLatin1());
    const QByteArray identity = CryptoService::encrypt(m_keys.identityKey(), privateKey, "identity:" + uid.toLatin1());
    CryptoService::wipe(privateKey);

    m_tree = std::make_unique<EncryptedBTree>(m_store.get(), m_cache.get(), m_keys.indexKey(), m_settings.bucketSize);
    m_root = RootPointer();

    // Salt được ghi sau cùng: nếu lỗi giữa chừng thì người dùng coi như chưa tồn tại
    if (!m_store->put(userPath(QStringLiteral("verify")), verify)
        || !m_store->put(userPath(QStringLiteral("pub")), publicKey)
        || !m_store->put(userPath(QStringLiteral("identity")), identity)
        || !saveRoot(m_root, error)
        || !m_store->put(userPath(QStringLiteral("salt")), salt)) {
        if (error->isEmpty())
            *error = QStringLiteral("Không tạo được kho: %1").arg(m_store->lastError());
        this->lock();
        return false;
    }
    return true;
}

bool VaultService::open(const QString &username, const QString &passphrase, QString *error)
{
    QMutexLocker lock(&m_mutex);
    this->lock();
    const QString uid = userIdFor(username);
    const QString base = QStringLiteral("users/%1/").arg(uid);

    QByteArray salt;
    if (!m_store->get(base + QStringLiteral("salt"), &salt)) {
        *error = m_store->notFound() ? QStringLiteral("Người dùng '%1' chưa có kho").arg(username)
                                     : m_store->lastError();
        return false;
    }
    if (!m_keys.derive(passphrase, salt)) {
        *error = QStringLiteral("Không dẫn xuất được khoá (scrypt)");
        return false;
    }

    QByteArray verify;
    QByteArray plain;
    if (!m_store->get(base + QStringLiteral("verify"), &verify)) {
        *error = m_store->lastError();
        m_keys.wipe();
        return false;
    }
    if (!CryptoService::decrypt(m_keys.verifyKey(), verify, "verify:" + uid.toLatin1(), &plain)
        || plain != kVerifyPlain) {
        *error = QStringLiteral("Sai passphrase");
        m_keys.wipe();
        return false;
    }

    QByteArray identity;
    QByteArray publicKey;
    QByteArray privateKey;
    if (!m_store->get(base + QStringLiteral("identity"), &identity)
        || !m_store->get(base + QStringLiteral("pub"), &publicKey)
        || !CryptoService::decrypt(m_keys.identityKey(), identity, "identity:" + uid.toLatin1(), &privateKey)) {
        *error = QStringLiteral("Không đọc được khoá định danh của người dùng");
        m_keys.wipe();
        return false;
    }
    m_keys.setIdentity(privateKey, publicKey);
    CryptoService::wipe(privateKey);

    m_uid = uid;
    m_username = username.trimmed();
    m_tree = std::make_unique<EncryptedBTree>(m_store.get(), m_cache.get(), m_keys.indexKey(), m_settings.bucketSize);
    if (!loadRoot(&m_root, error)) {
        this->lock();
        return false;
    }
    return true;
}

void VaultService::lock()
{
    QMutexLocker lock(&m_mutex);
    m_keys.wipe();
    m_uid.clear();
    m_username.clear();
    m_root = RootPointer();
    m_metaCache.clear();
    for (auto it = m_sharedKeys.begin(); it != m_sharedKeys.end(); ++it)
        CryptoService::wipe(it.value());
    m_sharedKeys.clear();
    m_sharedMetas.clear();
    m_cache->clearMemory();
    m_tree.reset();
}

bool VaultService::isOpen() const
{
    QMutexLocker lock(&m_mutex);
    return isOpenUnlocked();
}

QString VaultService::username() const
{
    QMutexLocker lock(&m_mutex);
    return m_username;
}

QString VaultService::fingerprint() const
{
    QMutexLocker lock(&m_mutex);
    return m_keys.publicKey().isEmpty() ? QString() : CryptoService::fingerprint(m_keys.publicKey());
}

qint64 VaultService::videoCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_root.count;
}

// ---------------- Root pointer ----------------

bool VaultService::loadRoot(RootPointer *root, QString *error)
{
    QByteArray blob;
    if (!m_store->get(userPath(QStringLiteral("root")), &blob)) {
        *error = QStringLiteral("Không đọc được root pointer: %1").arg(m_store->lastError());
        return false;
    }
    QByteArray plain;
    QCborMap map;
    if (!CryptoService::decrypt(m_keys.metaKey(), blob, "root:" + m_uid.toLatin1(), &plain) || !parseMap(plain, &map)) {
        *error = QStringLiteral("Root pointer không hợp lệ (sai khoá hoặc bị sửa trên server)");
        return false;
    }
    root->version = quint64(map.value(QStringLiteral("v")).toInteger());
    root->count = map.value(QStringLiteral("n")).toInteger();
    root->roots.clear();
    const QCborMap roots = map.value(QStringLiteral("r")).toMap();
    for (auto it = roots.constBegin(); it != roots.constEnd(); ++it)
        root->roots.insert(it.key().toString(), it.value().toByteArray());
    root->entries.clear();
    const QCborMap entries = map.value(QStringLiteral("c")).toMap();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        root->entries.insert(it.key().toString(), it.value().toInteger());
    return true;
}

bool VaultService::saveRoot(const RootPointer &root, QString *error)
{
    QCborMap roots;
    for (auto it = root.roots.constBegin(); it != root.roots.constEnd(); ++it) {
        if (!it.value().isEmpty())
            roots.insert(it.key(), it.value());
    }
    QCborMap entries;
    for (auto it = root.entries.constBegin(); it != root.entries.constEnd(); ++it)
        entries.insert(it.key(), it.value());
    QCborMap map;
    map.insert(QStringLiteral("v"), qint64(root.version));
    map.insert(QStringLiteral("n"), root.count);
    map.insert(QStringLiteral("r"), roots);
    map.insert(QStringLiteral("c"), entries);
    const QByteArray blob = CryptoService::encrypt(m_keys.metaKey(), QCborValue(map).toCbor(), "root:" + m_uid.toLatin1());
    if (blob.isEmpty() || !m_store->putAtomic(userPath(QStringLiteral("root")), blob)) {
        *error = QStringLiteral("Không ghi được root pointer: %1").arg(m_store->lastError());
        return false;
    }
    return true;
}

// ---------------- Chỉ mục ----------------

QList<VaultService::IndexChange> VaultService::nameEntriesFor(const VideoMeta &meta)
{
    QList<IndexChange> changes;
    changes.append({QStringLiteral("name"), {IndexKey::fromText(meta.name), meta.oid}});
    for (const QString &word : IndexKey::words(meta.name)) {
        const QByteArray wordKey = IndexKey::fromText(word);
        changes.append({QStringLiteral("word"), {wordKey, meta.oid}});
        changes.append({QStringLiteral("word_duration"), {IndexKey::composite(wordKey, meta.durationMs), meta.oid}});
    }
    return changes;
}

QList<VaultService::IndexChange> VaultService::indexEntriesFor(const VideoMeta &meta)
{
    QList<IndexChange> changes = nameEntriesFor(meta);
    changes.append({QStringLiteral("date"), {IndexKey::fromInt(meta.uploadedAt), meta.oid}});
    changes.append({QStringLiteral("duration"), {IndexKey::fromInt(meta.durationMs), meta.oid}});
    changes.append({QStringLiteral("size"), {IndexKey::fromInt(meta.size), meta.oid}});
    return changes;
}

bool VaultService::applyIndexChanges(const QList<IndexChange> &removals, const QList<IndexChange> &insertions,
                                     qint64 countDelta, QueryStats *stats, QString *error)
{
    // Luôn dựa trên root mới nhất trên server, rồi kiểm tra lại version trước khi ghi
    RootPointer fresh;
    ++stats->roundTrips;
    if (!loadRoot(&fresh, error))
        return false;

    // Duyệt song song mọi cây liên quan (Fig. 4b): mỗi tầng một vòng cho tất cả
    QHash<QString, QList<BTreeEntry>> touched;
    for (const IndexChange &c : removals)
        touched[c.index].append(c.entry);
    for (const IndexChange &c : insertions)
        touched[c.index].append(c.entry);
    QList<QPair<QByteArray, QList<BTreeEntry>>> work;
    for (auto it = touched.constBegin(); it != touched.constEnd(); ++it)
        work.append({fresh.roots.value(it.key()), it.value()});
    if (!m_tree->prefetchPaths(work, stats, error))
        return false;

    QHash<QString, QByteArray> newRoots = fresh.roots;
    QList<QByteArray> obsolete;
    QList<QByteArray> written;
    auto discardWritten = [&]() {
        for (const QByteArray &id : std::as_const(written))
            m_store->remove(EncryptedBTree::bucketPath(id));
    };

    for (const QString &index : IndexNames) {
        if (!touched.contains(index))
            continue;
        BTreeTransaction tx(m_tree.get(), fresh.roots.value(index));
        bool ok = true;
        for (const IndexChange &c : removals) {
            if (c.index == index && !tx.remove(c.entry, error))
                ok = false;
        }
        for (const IndexChange &c : insertions) {
            if (ok && c.index == index && !tx.insert(c.entry, error))
                ok = false;
        }
        QByteArray newRoot;
        if (ok)
            ok = tx.commit(&newRoot, error);
        stats->add(tx.stats);
        written += tx.writtenIds();
        if (!ok) {
            discardWritten();
            return false;
        }
        obsolete += tx.obsoleteIds();
        newRoots.insert(index, newRoot);
        fresh.entries[index] = qMax<qint64>(0, fresh.entries.value(index) + tx.delta());
    }

    // Khoá lạc quan: nếu client khác đã commit trong lúc này thì huỷ
    RootPointer check;
    ++stats->roundTrips;
    if (!loadRoot(&check, error) || check.version != fresh.version) {
        if (error->isEmpty())
            *error = QStringLiteral("Kho vừa được client khác thay đổi. Hãy thử lại.");
        discardWritten();
        return false;
    }

    fresh.roots = newRoots;
    fresh.version += 1;
    fresh.count = qMax<qint64>(0, fresh.count + countDelta);
    ++stats->roundTrips;
    if (!saveRoot(fresh, error)) {
        discardWritten();
        return false;
    }
    m_root = fresh;

    // Bucket cũ không còn được tham chiếu
    for (const QByteArray &id : std::as_const(obsolete))
        m_store->remove(EncryptedBTree::bucketPath(id));
    return true;
}

// ---------------- Metadata & khoá ----------------

bool VaultService::readMeta(const QByteArray &oid, VideoMeta *meta, QString *error)
{
    auto cached = m_metaCache.constFind(oid);
    if (cached != m_metaCache.constEnd()) {
        *meta = cached.value();
        return true;
    }
    QByteArray blob;
    if (!m_store->get(userPath(QStringLiteral("meta/") + hex(oid)), &blob)) {
        *error = QStringLiteral("Không đọc được metadata: %1").arg(m_store->lastError());
        return false;
    }
    QByteArray plain;
    QCborMap map;
    if (!CryptoService::decrypt(m_keys.metaKey(), blob, ("meta:" + m_uid + ":" + hex(oid)).toLatin1(), &plain)
        || !parseMap(plain, &map)) {
        *error = QStringLiteral("Metadata không giải mã được");
        return false;
    }
    meta->oid = oid;
    metaFromCbor(map, meta);
    m_metaCache.insert(oid, *meta);
    return true;
}

bool VaultService::writeMeta(const VideoMeta &meta, QString *error)
{
    const QByteArray blob = CryptoService::encrypt(m_keys.metaKey(), QCborValue(metaToCbor(meta)).toCbor(),
                                                   ("meta:" + m_uid + ":" + meta.oidHex()).toLatin1());
    if (blob.isEmpty() || !m_store->putAtomic(userPath(QStringLiteral("meta/") + meta.oidHex()), blob)) {
        *error = QStringLiteral("Không ghi được metadata: %1").arg(m_store->lastError());
        return false;
    }
    m_metaCache.insert(meta.oid, meta);
    return true;
}

QList<VideoMeta> VaultService::fetchMetas(const QList<QByteArray> &oids, QueryStats *stats, QString *error)
{
    QStringList paths;
    QList<QByteArray> missing;
    for (const QByteArray &oid : oids) {
        if (!m_metaCache.contains(oid)) {
            missing.append(oid);
            paths.append(userPath(QStringLiteral("meta/") + hex(oid)));
        }
    }
    if (!paths.isEmpty()) {
        // Bulk-fetch metadata của mọi kết quả trong một vòng
        ++stats->roundTrips;
        bool ok = true;
        const QList<QByteArray> blobs = m_store->getMany(paths, &ok);
        if (!ok)
            *error = QStringLiteral("Không tải được metadata: %1").arg(m_store->lastError());
        for (int i = 0; i < blobs.size() && i < missing.size(); ++i) {
            if (blobs.at(i).isEmpty())
                continue;
            ++stats->objectsFetched;
            stats->bytesIn += blobs.at(i).size();
            QByteArray plain;
            QCborMap map;
            if (!CryptoService::decrypt(m_keys.metaKey(), blobs.at(i),
                                        ("meta:" + m_uid + ":" + hex(missing.at(i))).toLatin1(), &plain)
                || !parseMap(plain, &map))
                continue;
            VideoMeta meta;
            meta.oid = missing.at(i);
            metaFromCbor(map, &meta);
            m_metaCache.insert(meta.oid, meta);
        }
    }

    QList<VideoMeta> result;
    for (const QByteArray &oid : oids) {
        auto it = m_metaCache.constFind(oid);
        if (it != m_metaCache.constEnd())
            result.append(it.value());
    }
    return result;
}

bool VaultService::readCek(const QByteArray &oid, QByteArray *cek, QString *error)
{
    const QString path = userPath(QStringLiteral("keyring/") + hex(oid));
    QByteArray wrapped;
    if (!m_cache->blob(path, &wrapped)) {
        if (!m_store->get(path, &wrapped)) {
            *error = QStringLiteral("Không đọc được khoá của video: %1").arg(m_store->lastError());
            return false;
        }
        m_cache->insertBlob(path, wrapped);
    }
    if (!m_keys.unwrapCek(oid, wrapped, cek)) {
        *error = QStringLiteral("Không mở được khoá của video (keyring bị sửa?)");
        return false;
    }
    return true;
}

// ---------------- Upload / đổi tên / xoá ----------------

bool VaultService::upload(QIODevice *source, const QString &name, qint64 durationMs, const QString &mime,
                          VideoMeta *result, QString *error, const Progress &progress)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return false;
    }
    if (!source || !source->isReadable()) {
        *error = QStringLiteral("Không đọc được file nguồn");
        return false;
    }
    const qint64 size = source->size();
    if (size <= 0) {
        *error = QStringLiteral("File rỗng");
        return false;
    }

    const QByteArray oid = CryptoService::randomBytes(16);
    QByteArray cek = CryptoService::randomBytes(CryptoService::KeySize);
    const VideoHeader header = VideoHeader::forSize(quint64(size), quint32(m_settings.chunkSize));
    const QByteArray headerBytes = header.serialize();
    const QString objectPath = QStringLiteral("obj/") + hex(oid);

    bool headerSent = false;
    quint32 index = 0;
    qint64 done = 0;
    QString sourceError;
    auto producer = [&](QByteArray *piece) -> bool {
        if (!headerSent) {
            headerSent = true;
            *piece = headerBytes;
            return true;
        }
        if (index >= header.chunkCount) {
            piece->clear();
            return true;
        }
        const qint64 expected = header.chunkPlainLength(index);
        QByteArray plain;
        plain.reserve(expected);
        while (plain.size() < expected) {
            const QByteArray part = source->read(expected - plain.size());
            if (part.isEmpty())
                break;
            plain.append(part);
        }
        if (plain.size() != expected) {
            sourceError = QStringLiteral("Đọc file nguồn thất bại ở chunk %1").arg(index);
            return false;
        }
        const bool last = index + 1 == header.chunkCount;
        *piece = CryptoService::encrypt(cek, plain, VideoHeader::chunkAad(headerBytes, oid, index, last));
        CryptoService::wipe(plain);
        if (piece->isEmpty()) {
            sourceError = QStringLiteral("Mã hoá thất bại");
            return false;
        }
        ++index;
        done += expected;
        if (progress)
            progress(done, size);
        return true;
    };

    if (!m_store->putStream(objectPath, producer)) {
        *error = sourceError.isEmpty() ? QStringLiteral("Không upload được video: %1").arg(m_store->lastError())
                                       : sourceError;
        m_store->remove(objectPath);
        CryptoService::wipe(cek);
        return false;
    }

    const QString keyringPath = userPath(QStringLiteral("keyring/") + hex(oid));
    const QByteArray wrapped = m_keys.wrapCek(oid, cek);
    CryptoService::wipe(cek);
    if (wrapped.isEmpty() || !m_store->put(keyringPath, wrapped)) {
        *error = QStringLiteral("Không ghi được keyring: %1").arg(m_store->lastError());
        m_store->remove(objectPath);
        return false;
    }

    VideoMeta meta;
    meta.oid = oid;
    meta.name = name;
    meta.uploadedAt = QDateTime::currentMSecsSinceEpoch();
    meta.durationMs = qMax<qint64>(0, durationMs);
    meta.size = size;
    meta.mime = mime;

    QueryStats stats;
    if (!writeMeta(meta, error) || !applyIndexChanges({}, indexEntriesFor(meta), 1, &stats, error)) {
        m_store->remove(objectPath);
        m_store->remove(keyringPath);
        m_store->remove(userPath(QStringLiteral("meta/") + hex(oid)));
        m_metaCache.remove(oid);
        return false;
    }
    if (result)
        *result = meta;
    return true;
}

bool VaultService::uploadFile(const QString &path, qint64 durationMs, VideoMeta *result, QString *error,
                              const Progress &progress)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Không mở được file: %1").arg(file.errorString());
        return false;
    }
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
    return upload(&file, QFileInfo(path).fileName(), durationMs, mime, result, error, progress);
}

bool VaultService::rename(const QByteArray &oid, const QString &newName, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return false;
    }
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('/'))) {
        *error = QStringLiteral("Tên mới không hợp lệ");
        return false;
    }
    VideoMeta old;
    if (!readMeta(oid, &old, error))
        return false;
    VideoMeta renamed = old;
    renamed.name = keepSuffix(old.name, trimmed);
    if (renamed.name == old.name)
        return true;

    // Chỉ sửa chỉ mục + metadata; object video trên server giữ nguyên tên ngẫu nhiên
    QueryStats stats;
    if (!applyIndexChanges(nameEntriesFor(old), nameEntriesFor(renamed), 0, &stats, error))
        return false;
    return writeMeta(renamed, error);
}

bool VaultService::remove(const QByteArray &oid, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return false;
    }
    VideoMeta meta;
    if (!readMeta(oid, &meta, error))
        return false;

    QueryStats stats;
    if (!applyIndexChanges(indexEntriesFor(meta), {}, -1, &stats, error))
        return false;

    // Thu hồi mọi lượt chia sẻ video này
    QString shareError;
    const QList<ShareRecord> shares = outgoingShares(&shareError);
    for (const ShareRecord &share : shares) {
        if (share.oid == oid)
            revoke(share.shareId, &shareError);
    }

    m_store->remove(QStringLiteral("obj/") + hex(oid));
    m_store->remove(userPath(QStringLiteral("meta/") + hex(oid)));
    m_store->remove(userPath(QStringLiteral("keyring/") + hex(oid)));
    m_metaCache.remove(oid);
    return true;
}

// ---------------- Truy vấn ----------------

QueryResult VaultService::library()
{
    QMutexLocker lock(&m_mutex);
    QueryResult result;
    QElapsedTimer timer;
    timer.start();
    if (!isOpenUnlocked()) {
        result.error = QStringLiteral("Kho chưa được mở");
        return result;
    }

    QList<BTreeEntry> entries;
    QString error;
    if (!m_tree->rangeAll(m_root.roots.value(QStringLiteral("date")), {}, {}, &entries, &result.stats, &error)) {
        result.error = error;
        return result;
    }
    QList<QByteArray> oids;
    for (auto it = entries.crbegin(); it != entries.crend(); ++it)
        oids.append(it->oid);  // mới nhất trước
    result.items = fetchMetas(oids, &result.stats, &error);
    result.error = error;
    result.stats.elapsedMs = timer.elapsed();
    result.stats.note = QStringLiteral("Bulk-fetch cây chỉ mục ngày theo từng tầng (Fig. 4a): cây cao %1, "
                                       "server biết tổng số video (%2) nhưng không biết tên hay ngày.")
                            .arg(result.stats.treeHeight)
                            .arg(entries.size());
    return result;
}

bool VaultService::conditionRange(const VaultQuery &query, QString *index, QByteArray *lo, QByteArray *hi)
{
    switch (query.field) {
    case QueryField::NameExact:
        *index = QStringLiteral("name");
        *lo = IndexKey::fromText(query.text.trimmed());
        *hi = IndexKey::exactUpperBound(*lo);
        return !lo->isEmpty();
    case QueryField::NamePrefix:
        *index = QStringLiteral("name");
        *lo = IndexKey::fromText(query.text.trimmed());
        *hi = IndexKey::prefixUpperBound(*lo);
        return !lo->isEmpty();
    case QueryField::Word: {
        const QString word = IndexKey::words(query.text).value(0);
        *index = QStringLiteral("word");
        *lo = IndexKey::fromText(word);
        *hi = IndexKey::exactUpperBound(*lo);
        return !word.isEmpty();
    }
    case QueryField::UploadDate:
        *index = QStringLiteral("date");
        break;
    case QueryField::Duration:
        *index = QStringLiteral("duration");
        break;
    case QueryField::Size:
        *index = QStringLiteral("size");
        break;
    }
    if (query.to < query.from)
        return false;
    *lo = IndexKey::fromInt(query.from);
    *hi = IndexKey::intUpperBound(query.to);
    return true;
}

bool VaultService::matches(const VideoMeta &meta, const VaultQuery &query)
{
    bool first = true;
    switch (query.field) {
    case QueryField::NameExact:
        first = IndexKey::fromText(meta.name) == IndexKey::fromText(query.text.trimmed());
        break;
    case QueryField::NamePrefix:
        first = IndexKey::fromText(meta.name).startsWith(IndexKey::fromText(query.text.trimmed()));
        break;
    case QueryField::Word:
        first = IndexKey::words(meta.name).contains(IndexKey::words(query.text).value(0));
        break;
    case QueryField::UploadDate:
        first = meta.uploadedAt >= query.from && meta.uploadedAt <= query.to;
        break;
    case QueryField::Duration:
        first = meta.durationMs >= query.from && meta.durationMs <= query.to;
        break;
    case QueryField::Size:
        first = meta.size >= query.from && meta.size <= query.to;
        break;
    }
    if (!first)
        return false;
    if (query.withDuration)
        return meta.durationMs >= query.durationFrom && meta.durationMs <= query.durationTo;
    return true;
}

qint64 VaultService::indexSize(const QString &index) const
{
    auto it = m_root.entries.constFind(index);
    if (it != m_root.entries.constEnd())
        return it.value();
    // Các chỉ mục một-phần-tử-mỗi-video
    if (index == QLatin1String("name") || index == QLatin1String("date")
        || index == QLatin1String("duration") || index == QLatin1String("size"))
        return m_root.count;
    return 0;
}

bool VaultService::runRange(const QString &index, const QByteArray &lo, const QByteArray &hi, int limit,
                            QList<BTreeEntry> *out, QueryStats *stats, QString *error)
{
    const QByteArray root = m_root.roots.value(index);
    if (limit > 0)
        return m_tree->rangeLimit(root, lo, hi, limit, out, stats, error);
    return m_tree->rangeAll(root, lo, hi, out, stats, error);
}

QueryResult VaultService::query(const VaultQuery &query)
{
    QMutexLocker lock(&m_mutex);
    QueryResult result;
    QElapsedTimer timer;
    timer.start();
    if (!isOpenUnlocked()) {
        result.error = QStringLiteral("Kho chưa được mở");
        return result;
    }

    QString index;
    QByteArray lo;
    QByteArray hi;
    if (!conditionRange(query, &index, &lo, &hi)) {
        result.error = QStringLiteral("Giá trị tìm kiếm không hợp lệ");
        return result;
    }
    if (query.withDuration && query.durationTo < query.durationFrom) {
        result.error = QStringLiteral("Khoảng thời lượng không hợp lệ");
        return result;
    }

    QString error;
    QList<BTreeEntry> entries;
    const bool twoConditions = query.withDuration && query.field != QueryField::Duration;

    if (twoConditions && query.strategy == QueryStrategy::Preorder && query.field == QueryField::Word) {
        // Preorder (Mục II.C.2): chỉ mục ghép BTree((từ khoá, thời lượng) -> oid)
        const QByteArray compositeLo = IndexKey::composite(lo, query.durationFrom);
        const QByteArray compositeHi = query.durationTo == std::numeric_limits<qint64>::max()
                                           ? IndexKey::prefixUpperBound(lo + '\0')
                                           : IndexKey::composite(lo, query.durationTo + 1);
        if (!runRange(QStringLiteral("word_duration"), compositeLo, compositeHi, query.limit,
                      &entries, &result.stats, &error)) {
            result.error = error;
            return result;
        }
        result.stats.note = QStringLiteral("Preorder (Mục II.C.2): duyệt lười chỉ mục ghép (từ khoá, thời lượng) "
                                           "theo thứ tự thời lượng. Server không biết mỗi điều kiện khớp bao nhiêu video.");
    } else if (twoConditions) {
        // Prefetch (Mục II.C.1): ước lượng bằng contour, tải trọn tập nhỏ hơn rồi lọc ở client
        const QByteArray durationLo = IndexKey::fromInt(query.durationFrom);
        const QByteArray durationHi = IndexKey::intUpperBound(query.durationTo);
        qint64 firstEstimate = 0;
        qint64 durationEstimate = 0;
        if (!m_tree->estimateCount(m_root.roots.value(index), lo, hi, indexSize(index),
                                   &firstEstimate, &result.stats, &error)
            || !m_tree->estimateCount(m_root.roots.value(QStringLiteral("duration")), durationLo, durationHi,
                                      indexSize(QStringLiteral("duration")), &durationEstimate, &result.stats, &error)) {
            result.error = error;
            return result;
        }
        const bool useFirst = firstEstimate <= durationEstimate;
        QList<BTreeEntry> candidates;
        if (!(useFirst ? m_tree->rangeAll(m_root.roots.value(index), lo, hi, &candidates, &result.stats, &error)
                       : m_tree->rangeAll(m_root.roots.value(QStringLiteral("duration")), durationLo, durationHi,
                                          &candidates, &result.stats, &error))) {
            result.error = error;
            return result;
        }
        QList<QByteArray> oids;
        for (const BTreeEntry &e : std::as_const(candidates))
            oids.append(e.oid);
        const QList<VideoMeta> metas = fetchMetas(oids, &result.stats, &error);
        for (const VideoMeta &meta : metas) {
            if (matches(meta, query)) {
                result.items.append(meta);
                if (query.limit > 0 && result.items.size() >= query.limit)
                    break;
            }
        }
        QString prefix;
        if (query.strategy == QueryStrategy::Preorder)
            prefix = QStringLiteral("Chỉ mục ghép chỉ có cho (từ khoá, thời lượng) nên chuyển sang Prefetch. ");
        result.stats.note = prefix
                            + QStringLiteral("Prefetch (Mục II.C.1): ước lượng bằng contour — điều kiện 1 ≈ %1, "
                                             "thời lượng ≈ %2. Đã tải trọn %3 oid của điều kiện %4 rồi lọc ở client; "
                                             "server thấy được con số %3.")
                                  .arg(firstEstimate)
                                  .arg(durationEstimate)
                                  .arg(candidates.size())
                                  .arg(useFirst ? QStringLiteral("1") : QStringLiteral("thời lượng"));
        result.stats.elapsedMs = timer.elapsed();
        result.error = error;
        return result;
    } else {
        if (!runRange(index, lo, hi, query.limit, &entries, &result.stats, &error)) {
            result.error = error;
            return result;
        }
        switch (query.field) {
        case QueryField::NameExact:
        case QueryField::Word:
            result.stats.note = QStringLiteral("Tìm chính xác: duyệt cây từng vòng như Fig. 2b "
                                               "(cây cao %1). Server chỉ thấy các bucket id ngẫu nhiên.")
                                    .arg(result.stats.treeHeight);
            break;
        case QueryField::NamePrefix:
            result.stats.note = QStringLiteral("Tìm theo tiền tố tên: một khoảng liên tục trên chỉ mục tên.");
            break;
        default:
            result.stats.note = query.limit > 0
                                    ? QStringLiteral("Truy vấn khoảng có limit (Fig. 3a): xuống lá đầu khoảng rồi đi dần sang phải.")
                                    : QStringLiteral("Lấy toàn bộ khoảng theo từng tầng (Fig. 3b); server thấy xấp xỉ số phần tử trong khoảng.");
            break;
        }
    }

    QList<QByteArray> oids;
    QSet<QByteArray> seen;
    for (const BTreeEntry &e : std::as_const(entries)) {
        if (!seen.contains(e.oid)) {
            seen.insert(e.oid);
            oids.append(e.oid);
        }
    }
    result.items = fetchMetas(oids, &result.stats, &error);
    result.error = error;
    result.stats.elapsedMs = timer.elapsed();
    return result;
}

// ---------------- Chia sẻ ----------------

bool VaultService::lookupUser(const QString &username, QByteArray *publicKey, QString *error)
{
    QMutexLocker lock(&m_mutex);
    const QString path = QStringLiteral("users/%1/pub").arg(userIdFor(username));
    if (!m_store->get(path, publicKey) || publicKey->size() != 32) {
        *error = QStringLiteral("Người dùng '%1' chưa có kho trên server").arg(username);
        return false;
    }
    return true;
}

bool VaultService::share(const QByteArray &oid, const QString &recipient, QString *shareId, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return false;
    }
    if (userIdFor(recipient) == m_uid) {
        *error = QStringLiteral("Không thể chia sẻ cho chính mình");
        return false;
    }
    QByteArray recipientKey;
    if (!lookupUser(recipient, &recipientKey, error))
        return false;
    VideoMeta meta;
    if (!readMeta(oid, &meta, error))
        return false;
    QByteArray cek;
    if (!readCek(oid, &cek, error))
        return false;

    const QString recipientUid = userIdFor(recipient);
    const QString sid = hex(CryptoService::randomBytes(16));
    QCborMap record = metaToCbor(meta);
    record.insert(QStringLiteral("from"), m_username);
    record.insert(QStringLiteral("fp"), CryptoService::fingerprint(m_keys.publicKey()));
    record.insert(QStringLiteral("o"), oid);
    record.insert(QStringLiteral("k"), cek);
    QByteArray plain = QCborValue(record).toCbor();
    CryptoService::wipe(cek);
    // Bọc lại CEK bằng khoá công khai X25519 của người nhận (bản đơn giản của Mục III)
    const QByteArray sealed = CryptoService::seal(recipientKey, plain, ("share:" + recipientUid + ":" + sid).toLatin1());
    CryptoService::wipe(plain);
    if (sealed.isEmpty()
        || !m_store->put(QStringLiteral("users/%1/inbox/%2").arg(recipientUid, sid), sealed)) {
        *error = QStringLiteral("Không ghi được lượt chia sẻ: %1").arg(m_store->lastError());
        return false;
    }

    QCborMap outbox;
    outbox.insert(QStringLiteral("to"), recipient.trimmed());
    outbox.insert(QStringLiteral("ruid"), recipientUid);
    outbox.insert(QStringLiteral("o"), oid);
    outbox.insert(QStringLiteral("n"), meta.name);
    outbox.insert(QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch());
    const QByteArray outboxBlob = CryptoService::encrypt(m_keys.metaKey(), QCborValue(outbox).toCbor(),
                                                         ("outbox:" + m_uid + ":" + sid).toLatin1());
    if (!m_store->put(userPath(QStringLiteral("outbox/") + sid), outboxBlob)) {
        m_store->remove(QStringLiteral("users/%1/inbox/%2").arg(recipientUid, sid));
        *error = QStringLiteral("Không ghi được outbox: %1").arg(m_store->lastError());
        return false;
    }
    if (shareId)
        *shareId = sid;
    return true;
}

QList<ShareRecord> VaultService::outgoingShares(QString *error)
{
    QMutexLocker lock(&m_mutex);
    QList<ShareRecord> records;
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return records;
    }
    QStringList paths;
    QStringList ids;
    for (const StoreEntry &entry : m_store->list(userPath(QStringLiteral("outbox")))) {
        if (entry.isDir || entry.name.contains(QLatin1String(".tmp")))
            continue;
        ids.append(entry.name);
        paths.append(userPath(QStringLiteral("outbox/") + entry.name));
    }
    bool ok = true;
    const QList<QByteArray> blobs = m_store->getMany(paths, &ok);
    for (int i = 0; i < blobs.size(); ++i) {
        QByteArray plain;
        QCborMap map;
        if (blobs.at(i).isEmpty()
            || !CryptoService::decrypt(m_keys.metaKey(), blobs.at(i), ("outbox:" + m_uid + ":" + ids.at(i)).toLatin1(), &plain)
            || !parseMap(plain, &map))
            continue;
        ShareRecord record;
        record.shareId = ids.at(i);
        record.recipient = map.value(QStringLiteral("to")).toString();
        record.recipientUid = map.value(QStringLiteral("ruid")).toString();
        record.oid = map.value(QStringLiteral("o")).toByteArray();
        record.videoName = map.value(QStringLiteral("n")).toString();
        record.sharedAt = map.value(QStringLiteral("at")).toInteger();
        records.append(record);
    }
    std::sort(records.begin(), records.end(), [](const ShareRecord &a, const ShareRecord &b) {
        return a.sharedAt > b.sharedAt;
    });
    if (!ok)
        *error = m_store->lastError();
    return records;
}

bool VaultService::revoke(const QString &shareId, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return false;
    }
    QByteArray blob;
    QByteArray plain;
    QCborMap map;
    if (!m_store->get(userPath(QStringLiteral("outbox/") + shareId), &blob)
        || !CryptoService::decrypt(m_keys.metaKey(), blob, ("outbox:" + m_uid + ":" + shareId).toLatin1(), &plain)
        || !parseMap(plain, &map)) {
        *error = QStringLiteral("Không tìm thấy lượt chia sẻ này");
        return false;
    }
    const QString recipientUid = map.value(QStringLiteral("ruid")).toString();
    // Chỉ ngăn người nhận lấy CEK từ nay về sau; không thu hồi được CEK họ đã mở
    m_store->remove(QStringLiteral("users/%1/inbox/%2").arg(recipientUid, shareId));
    m_store->remove(userPath(QStringLiteral("outbox/") + shareId));
    return true;
}

QList<VideoMeta> VaultService::inbox(QString *error)
{
    QMutexLocker lock(&m_mutex);
    QList<VideoMeta> items;
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return items;
    }
    for (auto it = m_sharedKeys.begin(); it != m_sharedKeys.end(); ++it)
        CryptoService::wipe(it.value());
    m_sharedKeys.clear();
    m_sharedMetas.clear();

    QStringList paths;
    QStringList ids;
    for (const StoreEntry &entry : m_store->list(userPath(QStringLiteral("inbox")))) {
        if (entry.isDir || entry.name.contains(QLatin1String(".tmp")))
            continue;
        ids.append(entry.name);
        paths.append(userPath(QStringLiteral("inbox/") + entry.name));
    }
    bool ok = true;
    const QList<QByteArray> blobs = m_store->getMany(paths, &ok);
    for (int i = 0; i < blobs.size(); ++i) {
        QByteArray plain;
        QCborMap map;
        if (blobs.at(i).isEmpty()
            || !m_keys.openSealed(blobs.at(i), ("share:" + m_uid + ":" + ids.at(i)).toLatin1(), &plain)
            || !parseMap(plain, &map))
            continue;
        VideoMeta meta;
        metaFromCbor(map, &meta);
        meta.oid = map.value(QStringLiteral("o")).toByteArray();
        meta.sharedBy = QStringLiteral("%1 (%2)").arg(map.value(QStringLiteral("from")).toString(),
                                                     map.value(QStringLiteral("fp")).toString());
        meta.shareId = ids.at(i);
        m_sharedKeys.insert(meta.shareId, map.value(QStringLiteral("k")).toByteArray());
        m_sharedMetas.insert(meta.shareId, meta);
        CryptoService::wipe(plain);
        items.append(meta);
    }
    if (!ok)
        *error = m_store->lastError();
    return items;
}

// ---------------- Phát video ----------------

DecryptingDevice *VaultService::openVideo(const VideoMeta &meta, QString *error)
{
    QMutexLocker lock(&m_mutex);
    if (!isOpenUnlocked()) {
        *error = QStringLiteral("Kho chưa được mở");
        return nullptr;
    }
    QByteArray cek;
    if (meta.isShared()) {
        cek = m_sharedKeys.value(meta.shareId);
        if (cek.isEmpty()) {
            *error = QStringLiteral("Hãy tải lại danh sách video được chia sẻ");
            return nullptr;
        }
    } else if (!readCek(meta.oid, &cek, error)) {
        return nullptr;
    }

    const QString path = QStringLiteral("obj/") + hex(meta.oid);
    QByteArray headerBytes;
    VideoHeader header;
    if (!m_store->getRange(path, 0, VideoHeader::Size, &headerBytes) || !VideoHeader::parse(headerBytes, &header)) {
        *error = m_store->notFound() ? QStringLiteral("Video không còn trên server")
                                     : QStringLiteral("Không đọc được header video: %1").arg(m_store->lastError());
        CryptoService::wipe(cek);
        return nullptr;
    }
    auto *device = new DecryptingDevice(m_store, path, cek, meta.oid, headerBytes);
    CryptoService::wipe(cek);
    device->open(QIODevice::ReadOnly);
    return device;
}
