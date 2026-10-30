#include "bucketcache.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

int BTreeNode::estimatedSize() const
{
    // Ước lượng kích thước sau khi tuần tự hoá CBOR (trước khi nén/mã hoá)
    int size = 8;
    for (const BTreeEntry &e : entries)
        size += int(e.key.size() + e.oid.size()) + 6;
    for (const BTreeEntry &e : separators)
        size += int(e.key.size() + e.oid.size()) + 6;
    for (const QByteArray &child : children)
        size += int(child.size()) + 2;
    return size;
}

BucketCache::BucketCache(qint64 ramBytes, const QString &diskPath, const QString &scope, qint64 diskBytes)
    : m_diskPath(diskPath)
    , m_scope(scope)
    , m_diskBytes(diskBytes)
{
    m_memory.setMaxCost(qMax<qint64>(ramBytes, 64 * 1024));
    if (!m_diskPath.isEmpty())
        QDir().mkpath(QFileInfo(m_diskPath).absolutePath());
}

BucketCache::~BucketCache()
{
    QMutexLocker lock(&m_mutex);
    for (const QString &name : std::as_const(m_connections))
        QSqlDatabase::removeDatabase(name);
}

BTreeNodePtr BucketCache::node(const QByteArray &id)
{
    QMutexLocker lock(&m_mutex);
    Holder *holder = m_memory.object(id);
    return holder ? holder->node : BTreeNodePtr();
}

void BucketCache::insertNode(const QByteArray &id, const BTreeNodePtr &node)
{
    QMutexLocker lock(&m_mutex);
    m_memory.insert(id, new Holder{node}, node->estimatedSize());
}

void BucketCache::clearMemory()
{
    QMutexLocker lock(&m_mutex);
    m_memory.clear();
}

QString BucketCache::connectionName()
{
    // QSqlDatabase chỉ được dùng trong luồng đã tạo nó -> mỗi luồng một kết nối
    const QString name = QStringLiteral("setp_cache_%1_%2")
                             .arg(quintptr(this), 0, 16)
                             .arg(quintptr(QThread::currentThreadId()), 0, 16);
    if (!QSqlDatabase::contains(name)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(m_diskPath);
        if (db.open()) {
            QSqlQuery query(db);
            query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS blobs ("
                                      "k TEXT PRIMARY KEY, v BLOB NOT NULL, ts INTEGER NOT NULL)"));
        }
        m_connections.insert(name);
    }
    return name;
}

bool BucketCache::blob(const QString &path, QByteArray *data)
{
    if (m_diskPath.isEmpty())
        return false;
    QMutexLocker lock(&m_mutex);
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT v FROM blobs WHERE k = ?"));
    query.addBindValue(m_scope + QLatin1Char('|') + path);
    if (!query.exec() || !query.next())
        return false;
    *data = query.value(0).toByteArray();
    return !data->isEmpty();
}

void BucketCache::insertBlob(const QString &path, const QByteArray &data)
{
    if (m_diskPath.isEmpty())
        return;
    QMutexLocker lock(&m_mutex);
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    if (!db.isOpen())
        return;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO blobs (k, v, ts) VALUES (?, ?, ?)"));
    query.addBindValue(m_scope + QLatin1Char('|') + path);
    query.addBindValue(data);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.exec();

    if (++m_insertsSincePrune >= 200) {
        m_insertsSincePrune = 0;
        pruneDiskIfNeeded();
    }
}

void BucketCache::pruneDiskIfNeeded()
{
    QSqlDatabase db = QSqlDatabase::database(connectionName());
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COALESCE(SUM(LENGTH(v)), 0), COUNT(*) FROM blobs")) || !query.next())
        return;
    const qint64 total = query.value(0).toLongLong();
    const qint64 count = query.value(1).toLongLong();
    if (total <= m_diskBytes || count == 0)
        return;
    // Xoá 25% bản ghi cũ nhất
    QSqlQuery prune(db);
    prune.prepare(QStringLiteral("DELETE FROM blobs WHERE k IN (SELECT k FROM blobs ORDER BY ts ASC LIMIT ?)"));
    prune.addBindValue(qMax<qint64>(1, count / 4));
    prune.exec();
}
