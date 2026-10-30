#ifndef BUCKETCACHE_H
#define BUCKETCACHE_H

#include <QByteArray>
#include <QCache>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>

#include <memory>

// Một phần tử của chỉ mục: (giá trị đã mã hoá thứ tự, oid). Dạng "unfold" ở Mục V.B:
// thay vì BTree(giá trị -> TreeSet(oid)), cây chứa trực tiếp các bộ đôi duy nhất.
struct BTreeEntry
{
    QByteArray key;
    QByteArray oid;

    bool operator<(const BTreeEntry &other) const
    {
        if (key != other.key)
            return key < other.key;
        return oid < other.oid;
    }
    bool operator==(const BTreeEntry &other) const { return key == other.key && oid == other.oid; }
    bool operator<=(const BTreeEntry &other) const { return !(other < *this); }
};

// Một bucket (nút) của B-tree ở dạng rõ. Chỉ tồn tại trong RAM phía client.
struct BTreeNode
{
    bool leaf = true;
    QList<BTreeEntry> entries;        // nút lá
    QList<BTreeEntry> separators;     // nút nhánh: separators[i] = phần tử nhỏ nhất của children[i+1]
    QList<QByteArray> children;       // nút nhánh: id bucket con (16 byte)

    int estimatedSize() const;
};

using BTreeNodePtr = std::shared_ptr<const BTreeNode>;

// Cache phía client (Mục II.A: "frequently accessed buckets can be cached client-side").
// - RAM: nút đã giải mã, LRU theo dung lượng (mặc định 5 MB như ví dụ ở Mục V.A).
// - Đĩa (SQLite): chỉ lưu CIPHERTEXT của object bất biến (bucket, keyring), nên đĩa
//   không bao giờ chứa dữ liệu rõ. Bucket id không bao giờ bị ghi đè (copy-on-write)
//   nên cache không thể bị cũ.
class BucketCache
{
public:
    BucketCache(qint64 ramBytes, const QString &diskPath = QString(),
                const QString &scope = QString(), qint64 diskBytes = 64ll * 1024 * 1024);
    ~BucketCache();

    BTreeNodePtr node(const QByteArray &id);
    void insertNode(const QByteArray &id, const BTreeNodePtr &node);
    void clearMemory();

    bool blob(const QString &path, QByteArray *data);
    void insertBlob(const QString &path, const QByteArray &data);

    bool hasDisk() const { return !m_diskPath.isEmpty(); }

private:
    struct Holder { BTreeNodePtr node; };
    QString connectionName();
    void pruneDiskIfNeeded();

    mutable QMutex m_mutex;
    QCache<QByteArray, Holder> m_memory;
    QString m_diskPath;
    QString m_scope;
    qint64 m_diskBytes;
    int m_insertsSincePrune = 0;
    QSet<QString> m_connections;
};

#endif // BUCKETCACHE_H
