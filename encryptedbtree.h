#ifndef ENCRYPTEDBTREE_H
#define ENCRYPTEDBTREE_H

#include "bucketcache.h"
#include "objectstore.h"
#include "vaulttypes.h"

#include <QHash>
#include <QPair>

// Mã hoá khoá chỉ mục sao cho thứ tự byte (memcmp) trùng với thứ tự giá trị.
// Chỉ dùng BÊN TRONG bucket đã mã hoá; server không bao giờ thấy các khoá này.
namespace IndexKey {
QByteArray fromInt(qint64 value);
QByteArray fromText(const QString &text);
QByteArray composite(const QByteArray &textKey, qint64 value);  // chỉ mục ghép (Mục II.C.2)
QByteArray exactUpperBound(const QByteArray &key);                // khoá nhỏ nhất > key
QByteArray prefixUpperBound(const QByteArray &prefix);            // rỗng = vô cực
QByteArray intUpperBound(qint64 inclusiveTo);                     // rỗng = vô cực
QStringList words(const QString &text);
}

// B-tree mà mỗi bucket là một object đã mã hoá trên server (Mục II, Fig. 2).
// Client duyệt cây từ xa: tải bucket -> giải mã -> chọn nhánh -> tải bucket con.
// Khoảng khoá luôn là [lo, hi); hi rỗng = không giới hạn trên.
class EncryptedBTree
{
public:
    EncryptedBTree(IObjectStore *store, BucketCache *cache, const QByteArray &indexKey, int bucketSize);

    static QString bucketPath(const QByteArray &id);
    int bucketSize() const { return m_bucketSize; }
    IObjectStore *store() const { return m_store; }
    BucketCache *cache() const { return m_cache; }

    // Tải một tầng: mọi bucket chưa có trong cache được lấy trong CÙNG MỘT vòng
    QList<BTreeNodePtr> fetchLevel(const QList<QByteArray> &ids, QueryStats *stats, QString *error);
    BTreeNodePtr fetch(const QByteArray &id, QueryStats *stats, QString *error);

    // Truy vấn khoảng có limit (Fig. 3a): xuống lá đầu khoảng rồi đi dần sang phải
    bool rangeLimit(const QByteArray &root, const QByteArray &lo, const QByteArray &hi, int limit,
                    QList<BTreeEntry> *out, QueryStats *stats, QString *error);
    // Lấy toàn bộ khoảng theo từng tầng (Fig. 3b) - số vòng = chiều cao cây
    bool rangeAll(const QByteArray &root, const QByteArray &lo, const QByteArray &hi,
                  QList<BTreeEntry> *out, QueryStats *stats, QString *error);
    // Ước lượng số phần tử trong khoảng bằng "contour" (Mục II.C.1), tốn ~H vòng.
    // totalEntries: tổng số phần tử của cây (0 = không biết, dùng fanout trung bình)
    bool estimateCount(const QByteArray &root, const QByteArray &lo, const QByteArray &hi, qint64 totalEntries,
                       qint64 *count, QueryStats *stats, QString *error);
    // Duyệt song song nhiều cây / nhiều khoá (Fig. 4b): nạp trước mọi bucket trên
    // đường đi của các phần tử, mỗi tầng một vòng cho TẤT CẢ các cây cùng lúc.
    bool prefetchPaths(const QList<QPair<QByteArray, QList<BTreeEntry>>> &work,
                       QueryStats *stats, QString *error);

    QByteArray encryptNode(const QByteArray &id, const BTreeNode &node) const;
    bool decryptNode(const QByteArray &id, const QByteArray &blob, BTreeNode *node) const;

    static int childIndexFor(const BTreeNode &node, const BTreeEntry &entry);
    static int childIndexForKey(const BTreeNode &node, const QByteArray &key);

private:
    IObjectStore *m_store;
    BucketCache *m_cache;
    QByteArray m_indexKey;
    int m_bucketSize;
};

// Giao dịch copy-on-write trên một cây: mọi bucket bị sửa được mã hoá lại với
// nonce mới và ghi dưới ID MỚI; bucket cũ được liệt kê để xoá sau khi commit root.
class BTreeTransaction
{
public:
    BTreeTransaction(EncryptedBTree *tree, const QByteArray &root);

    bool insert(const BTreeEntry &entry, QString *error);
    bool remove(const BTreeEntry &entry, QString *error);
    // Ghi mọi bucket đã sửa lên server, trả id root mới (rỗng = cây rỗng)
    bool commit(QByteArray *newRoot, QString *error);

    bool isDirty() const { return m_dirty; }
    qint64 delta() const { return m_delta; }  // số phần tử thêm - bớt thực sự
    QList<QByteArray> obsoleteIds() const { return m_obsolete; }
    QList<QByteArray> writtenIds() const { return m_written; }
    QueryStats stats;

private:
    using NodeRef = std::shared_ptr<BTreeNode>;
    static bool isTemp(const QByteArray &id) { return id.startsWith("tmp:"); }
    BTreeNodePtr readNode(const QByteArray &id, QString *error);
    QByteArray mutableNode(const QByteArray &id, NodeRef *node, QString *error);
    QByteArray insertInto(const QByteArray &id, const BTreeEntry &entry,
                          BTreeEntry *splitSeparator, QByteArray *splitRight, bool *ok, QString *error);
    QByteArray removeFrom(const QByteArray &id, const BTreeEntry &entry,
                          bool *found, bool *nowEmpty, bool *ok, QString *error);
    QByteArray finalize(const QByteArray &id, bool *ok, QString *error);
    bool splitIfNeeded(const NodeRef &node, BTreeEntry *separator, QByteArray *rightId);

    EncryptedBTree *m_tree;
    QByteArray m_root;
    QHash<QByteArray, NodeRef> m_temp;
    QList<QByteArray> m_obsolete;
    QList<QByteArray> m_written;
    int m_nextTemp = 1;
    qint64 m_delta = 0;
    bool m_dirty = false;
};

#endif // ENCRYPTEDBTREE_H
