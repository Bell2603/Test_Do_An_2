#include "encryptedbtree.h"
#include "cryptoservice.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------- IndexKey ----------------

namespace IndexKey {

QByteArray fromInt(qint64 value)
{
    // Big-endian + lật bit dấu: thứ tự byte trùng thứ tự số có dấu
    const quint64 flipped = quint64(value) ^ (quint64(1) << 63);
    QByteArray out(8, Qt::Uninitialized);
    qToBigEndian(flipped, out.data());
    return out;
}

QByteArray fromText(const QString &text)
{
    QString normalized = text.normalized(QString::NormalizationForm_C).toCaseFolded();
    normalized.remove(QChar(0));
    return normalized.left(200).toUtf8();
}

QByteArray composite(const QByteArray &textKey, qint64 value)
{
    // 0x00 nhỏ hơn mọi byte của chuỗi UTF-8 nên ("ab", x) < ("abc", y)
    return textKey + '\0' + fromInt(value);
}

QByteArray exactUpperBound(const QByteArray &key)
{
    return key + '\0';
}

QByteArray prefixUpperBound(const QByteArray &prefix)
{
    QByteArray bound = prefix;
    while (!bound.isEmpty()) {
        const uchar last = uchar(bound.back());
        if (last != 0xFF) {
            bound.back() = char(last + 1);
            return bound;
        }
        bound.chop(1);
    }
    return {};
}

QByteArray intUpperBound(qint64 inclusiveTo)
{
    if (inclusiveTo == std::numeric_limits<qint64>::max())
        return {};
    return fromInt(inclusiveTo + 1);
}

QStringList words(const QString &text)
{
    static const QRegularExpression separator(QStringLiteral("[^\\p{L}\\p{N}]+"));
    QStringList result;
    QSet<QString> seen;
    const QStringList parts = text.normalized(QString::NormalizationForm_C)
                                  .toCaseFolded()
                                  .split(separator, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (seen.contains(part))
            continue;
        seen.insert(part);
        result.append(part);
        if (result.size() >= 32)
            break;
    }
    return result;
}

} // namespace IndexKey

// ---------------- Tuần tự hoá bucket ----------------

namespace {

QCborArray entriesToCbor(const QList<BTreeEntry> &entries)
{
    QCborArray array;
    for (const BTreeEntry &e : entries)
        array.append(QCborArray{QCborValue(e.key), QCborValue(e.oid)});
    return array;
}

bool entriesFromCbor(const QCborArray &array, QList<BTreeEntry> *out)
{
    out->clear();
    out->reserve(array.size());
    for (const QCborValue &value : array) {
        const QCborArray pair = value.toArray();
        if (pair.size() != 2 || !pair.at(0).isByteArray() || !pair.at(1).isByteArray())
            return false;
        out->append({pair.at(0).toByteArray(), pair.at(1).toByteArray()});
    }
    return true;
}

QByteArray bucketAad(const QByteArray &id)
{
    return QByteArrayLiteral("b/") + id.toHex();
}

bool inRange(const QByteArray &key, const QByteArray &lo, const QByteArray &hi)
{
    return !(key < lo) && (hi.isEmpty() || key < hi);
}

} // namespace

// ---------------- EncryptedBTree ----------------

EncryptedBTree::EncryptedBTree(IObjectStore *store, BucketCache *cache, const QByteArray &indexKey, int bucketSize)
    : m_store(store)
    , m_cache(cache)
    , m_indexKey(indexKey)
    , m_bucketSize(qMax(128, bucketSize))
{
}

QString EncryptedBTree::bucketPath(const QByteArray &id)
{
    return QStringLiteral("b/") + QString::fromLatin1(id.toHex());
}

QByteArray EncryptedBTree::encryptNode(const QByteArray &id, const BTreeNode &node) const
{
    QCborMap map;
    map.insert(QStringLiteral("l"), node.leaf);
    if (node.leaf) {
        map.insert(QStringLiteral("e"), entriesToCbor(node.entries));
    } else {
        map.insert(QStringLiteral("s"), entriesToCbor(node.separators));
        QCborArray children;
        for (const QByteArray &child : node.children)
            children.append(QCborValue(child));
        map.insert(QStringLiteral("c"), children);
    }
    // Nén zlib trước khi mã hoá (Mục V.A: tỉ lệ nén c ~ 3)
    const QByteArray plain = qCompress(QCborValue(map).toCbor(), 6);
    return CryptoService::encrypt(m_indexKey, plain, bucketAad(id));
}

bool EncryptedBTree::decryptNode(const QByteArray &id, const QByteArray &blob, BTreeNode *node) const
{
    QByteArray compressed;
    if (!CryptoService::decrypt(m_indexKey, blob, bucketAad(id), &compressed))
        return false;
    QCborParserError parseError;
    const QCborValue value = QCborValue::fromCbor(qUncompress(compressed), &parseError);
    if (parseError.error != QCborError::NoError || !value.isMap())
        return false;

    const QCborMap map = value.toMap();
    node->leaf = map.value(QStringLiteral("l")).toBool(true);
    node->entries.clear();
    node->separators.clear();
    node->children.clear();
    if (node->leaf)
        return entriesFromCbor(map.value(QStringLiteral("e")).toArray(), &node->entries);

    if (!entriesFromCbor(map.value(QStringLiteral("s")).toArray(), &node->separators))
        return false;
    const QCborArray children = map.value(QStringLiteral("c")).toArray();
    for (const QCborValue &child : children)
        node->children.append(child.toByteArray());
    return node->children.size() == node->separators.size() + 1;
}

QList<BTreeNodePtr> EncryptedBTree::fetchLevel(const QList<QByteArray> &ids, QueryStats *stats, QString *error)
{
    QList<BTreeNodePtr> result(ids.size());
    QList<int> missing;
    QStringList paths;

    for (int i = 0; i < ids.size(); ++i) {
        if (BTreeNodePtr cached = m_cache->node(ids.at(i))) {
            result[i] = cached;
            ++stats->cacheHits;
            continue;
        }
        const QString path = bucketPath(ids.at(i));
        QByteArray blob;
        if (m_cache->blob(path, &blob)) {
            auto node = std::make_shared<BTreeNode>();
            if (decryptNode(ids.at(i), blob, node.get())) {
                m_cache->insertNode(ids.at(i), node);
                result[i] = node;
                ++stats->cacheHits;
                continue;
            }
        }
        missing.append(i);
        paths.append(path);
    }

    if (paths.isEmpty())
        return result;

    // Mọi bucket còn thiếu của tầng này được yêu cầu trong cùng một vòng
    ++stats->roundTrips;
    bool ok = true;
    const QList<QByteArray> blobs = m_store->getMany(paths, &ok);
    if (!ok || blobs.size() != paths.size()) {
        *error = QStringLiteral("Không tải được bucket chỉ mục: %1").arg(m_store->lastError());
        return {};
    }
    for (int j = 0; j < missing.size(); ++j) {
        const int i = missing.at(j);
        if (blobs.at(j).isEmpty()) {
            *error = QStringLiteral("Thiếu bucket %1 trên server (kho bị hỏng?)").arg(paths.at(j));
            return {};
        }
        auto node = std::make_shared<BTreeNode>();
        if (!decryptNode(ids.at(i), blobs.at(j), node.get())) {
            *error = QStringLiteral("Bucket %1 không giải mã được: sai khoá hoặc đã bị sửa trên server")
                         .arg(paths.at(j));
            return {};
        }
        ++stats->objectsFetched;
        stats->bytesIn += blobs.at(j).size();
        m_cache->insertNode(ids.at(i), node);
        m_cache->insertBlob(paths.at(j), blobs.at(j));
        result[i] = node;
    }
    return result;
}

BTreeNodePtr EncryptedBTree::fetch(const QByteArray &id, QueryStats *stats, QString *error)
{
    const QList<BTreeNodePtr> nodes = fetchLevel({id}, stats, error);
    return nodes.isEmpty() ? BTreeNodePtr() : nodes.first();
}

int EncryptedBTree::childIndexFor(const BTreeNode &node, const BTreeEntry &entry)
{
    // Số separator <= entry  (upper_bound)
    int lo = 0;
    int hi = int(node.separators.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (node.separators.at(mid) <= entry)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int EncryptedBTree::childIndexForKey(const BTreeNode &node, const QByteArray &key)
{
    // Con đầu tiên có thể chứa phần tử có khoá >= key = số separator có khoá < key
    int lo = 0;
    int hi = int(node.separators.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (node.separators.at(mid).key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool EncryptedBTree::rangeLimit(const QByteArray &root, const QByteArray &lo, const QByteArray &hi, int limit,
                                QList<BTreeEntry> *out, QueryStats *stats, QString *error)
{
    out->clear();
    if (root.isEmpty())
        return true;

    struct Frame { BTreeNodePtr node; int index; };
    QList<Frame> path;

    BTreeNodePtr node = fetch(root, stats, error);
    if (!node)
        return false;
    int height = 1;
    while (!node->leaf) {
        const int i = childIndexForKey(*node, lo);
        path.append({node, i});
        node = fetch(node->children.at(i), stats, error);
        if (!node)
            return false;
        ++height;
    }
    stats->treeHeight = qMax(stats->treeHeight, height);

    for (;;) {
        for (const BTreeEntry &e : node->entries) {
            if (e.key < lo)
                continue;
            if (!hi.isEmpty() && !(e.key < hi))
                return true;
            out->append(e);
            if (limit > 0 && out->size() >= limit)
                return true;
        }

        // Sang lá kế tiếp bên phải: mỗi lá mới là thêm một vòng (nếu chưa có trong cache)
        node.reset();
        while (!path.isEmpty()) {
            const int nextIndex = path.last().index + 1;
            const BTreeNodePtr parent = path.last().node;
            if (nextIndex < parent->children.size()) {
                path.last().index = nextIndex;
                if (!hi.isEmpty() && !(parent->separators.at(nextIndex - 1).key < hi))
                    return true;
                BTreeNodePtr next = fetch(parent->children.at(nextIndex), stats, error);
                if (!next)
                    return false;
                while (!next->leaf) {
                    path.append({next, 0});
                    next = fetch(next->children.at(0), stats, error);
                    if (!next)
                        return false;
                }
                node = next;
                break;
            }
            path.removeLast();
        }
        if (!node)
            return true;
    }
}

bool EncryptedBTree::rangeAll(const QByteArray &root, const QByteArray &lo, const QByteArray &hi,
                              QList<BTreeEntry> *out, QueryStats *stats, QString *error)
{
    out->clear();
    if (root.isEmpty())
        return true;

    QList<QByteArray> level{root};
    int height = 0;
    for (;;) {
        const QList<BTreeNodePtr> nodes = fetchLevel(level, stats, error);
        if (nodes.size() != level.size())
            return false;
        ++height;

        if (nodes.first()->leaf) {
            for (const BTreeNodePtr &node : nodes) {
                if (!node->leaf) {
                    *error = QStringLiteral("Cây chỉ mục không cân bằng");
                    return false;
                }
                for (const BTreeEntry &e : node->entries) {
                    if (inRange(e.key, lo, hi))
                        out->append(e);
                }
            }
            stats->treeHeight = qMax(stats->treeHeight, height);
            return true;
        }

        QList<QByteArray> next;
        for (const BTreeNodePtr &node : nodes) {
            if (node->leaf) {
                *error = QStringLiteral("Cây chỉ mục không cân bằng");
                return false;
            }
            const int last = int(node->children.size()) - 1;
            for (int i = 0; i <= last; ++i) {
                // Con i chứa các phần tử trong [separators[i-1], separators[i])
                const bool reachesLo = i == last || !(node->separators.at(i).key < lo);
                const bool startsBeforeHi = hi.isEmpty() || i == 0 || node->separators.at(i - 1).key < hi;
                if (reachesLo && startsBeforeHi)
                    next.append(node->children.at(i));
            }
        }
        if (next.isEmpty()) {
            stats->treeHeight = qMax(stats->treeHeight, height);
            return true;
        }
        level = next;
    }
}

bool EncryptedBTree::estimateCount(const QByteArray &root, const QByteArray &lo, const QByteArray &hi,
                                   qint64 totalEntries, qint64 *count, QueryStats *stats, QString *error)
{
    *count = 0;
    if (root.isEmpty())
        return true;

    BTreeNodePtr a = fetch(root, stats, error);
    if (!a)
        return false;
    BTreeNodePtr b = a;
    bool same = true;
    int depth = 0;

    // Vị trí tương đối của lo và hi trong toàn cây (0..1), giả định các nút anh em
    // có kích thước xấp xỉ nhau. Chỉ cần tải 2 đường biên (contour) -> ~H vòng.
    double positionLo = 0;
    double positionHi = 0;
    double weightA = 1;
    double weightB = 1;
    // Phương án dự phòng khi không biết tổng số phần tử
    QList<qint64> fullChildren;
    QList<int> fanouts;

    while (!a->leaf) {
        const int ia = childIndexForKey(*a, lo);
        const int ib = hi.isEmpty() ? int(b->children.size()) - 1 : childIndexForKey(*b, hi);
        positionLo += weightA * ia / a->children.size();
        weightA /= a->children.size();
        positionHi += weightB * ib / b->children.size();
        weightB /= b->children.size();

        fullChildren.append(same ? qMax(0, ib - ia - 1) : (a->children.size() - ia - 1) + ib);
        if (depth > 0) {
            fanouts.append(int(a->children.size()));
            if (!same)
                fanouts.append(int(b->children.size()));
        }

        QList<QByteArray> ids{a->children.at(ia)};
        if (!(same && ia == ib))
            ids.append(b->children.at(ib));
        const QList<BTreeNodePtr> nodes = fetchLevel(ids, stats, error);
        if (nodes.size() != ids.size())
            return false;
        a = nodes.first();
        b = nodes.last();
        same = ids.size() == 1;
        ++depth;
    }
    const int height = depth + 1;
    stats->treeHeight = qMax(stats->treeHeight, height);

    qint64 exact = 0;
    int lessThanLo = 0;
    for (const BTreeEntry &e : a->entries) {
        exact += inRange(e.key, lo, hi) ? 1 : 0;
        lessThanLo += e.key < lo ? 1 : 0;
    }
    if (same) {
        // Cả khoảng nằm trong một lá: đếm chính xác
        *count = exact;
        return true;
    }
    int lessThanHi = 0;
    for (const BTreeEntry &e : b->entries) {
        exact += inRange(e.key, lo, hi) ? 1 : 0;
        lessThanHi += (hi.isEmpty() || e.key < hi) ? 1 : 0;
    }
    positionLo += weightA * lessThanLo / qMax<qsizetype>(1, a->entries.size());
    positionHi += weightB * lessThanHi / qMax<qsizetype>(1, b->entries.size());

    if (totalEntries > 0) {
        *count = qMax<qint64>(exact, qRound64(qMax(0.0, positionHi - positionLo) * totalEntries));
        return true;
    }

    const double averageLeaf = (a->entries.size() + b->entries.size()) / 2.0;
    double averageFanout = 0;
    for (int f : fanouts)
        averageFanout += f;
    averageFanout = fanouts.isEmpty() ? 0 : averageFanout / fanouts.size();
    double total = double(exact);
    for (int k = 0; k < fullChildren.size(); ++k) {
        // Cây con ở độ sâu k+1 có (height - k - 2) tầng nhánh phía trên lá
        total += double(fullChildren.at(k)) * averageLeaf * std::pow(averageFanout, height - k - 2);
    }
    *count = qRound64(total);
    return true;
}

bool EncryptedBTree::prefetchPaths(const QList<QPair<QByteArray, QList<BTreeEntry>>> &work,
                                   QueryStats *stats, QString *error)
{
    QList<QPair<QByteArray, QList<BTreeEntry>>> frontier;
    for (const auto &item : work) {
        if (!item.first.isEmpty() && !item.second.isEmpty())
            frontier.append(item);
    }

    while (!frontier.isEmpty()) {
        QList<QByteArray> ids;
        for (const auto &item : frontier)
            ids.append(item.first);
        const QList<BTreeNodePtr> nodes = fetchLevel(ids, stats, error);
        if (nodes.size() != ids.size())
            return false;

        QList<QPair<QByteArray, QList<BTreeEntry>>> next;
        QHash<QByteArray, int> position;
        for (int i = 0; i < nodes.size(); ++i) {
            const BTreeNodePtr &node = nodes.at(i);
            if (node->leaf)
                continue;
            for (const BTreeEntry &e : frontier.at(i).second) {
                const QByteArray child = node->children.at(childIndexFor(*node, e));
                auto it = position.constFind(child);
                if (it == position.constEnd()) {
                    position.insert(child, int(next.size()));
                    next.append({child, {e}});
                } else {
                    next[it.value()].second.append(e);
                }
            }
        }
        frontier = next;
    }
    return true;
}

// ---------------- BTreeTransaction ----------------

BTreeTransaction::BTreeTransaction(EncryptedBTree *tree, const QByteArray &root)
    : m_tree(tree)
    , m_root(root)
{
}

BTreeNodePtr BTreeTransaction::readNode(const QByteArray &id, QString *error)
{
    if (isTemp(id))
        return m_temp.value(id);
    return m_tree->fetch(id, &stats, error);
}

QByteArray BTreeTransaction::mutableNode(const QByteArray &id, NodeRef *node, QString *error)
{
    if (isTemp(id)) {
        *node = m_temp.value(id);
        return id;
    }
    const BTreeNodePtr source = m_tree->fetch(id, &stats, error);
    if (!source)
        return {};
    // Copy-on-write: bucket gốc giữ nguyên, bản sửa sẽ nhận id mới khi commit
    const QByteArray temp = "tmp:" + QByteArray::number(m_nextTemp++);
    *node = std::make_shared<BTreeNode>(*source);
    m_temp.insert(temp, *node);
    m_obsolete.append(id);
    m_dirty = true;
    return temp;
}

bool BTreeTransaction::splitIfNeeded(const NodeRef &node, BTreeEntry *separator, QByteArray *rightId)
{
    if (node->estimatedSize() <= m_tree->bucketSize())
        return false;

    auto right = std::make_shared<BTreeNode>();
    right->leaf = node->leaf;
    if (node->leaf) {
        if (node->entries.size() < 2)
            return false;
        const int mid = int(node->entries.size()) / 2;
        right->entries = node->entries.mid(mid);
        node->entries.resize(mid);
        *separator = right->entries.first();
    } else {
        const int count = int(node->separators.size());
        if (count < 2)
            return false;
        const int mid = count / 2;
        *separator = node->separators.at(mid);
        right->separators = node->separators.mid(mid + 1);
        right->children = node->children.mid(mid + 1);
        node->separators.resize(mid);
        node->children.resize(mid + 1);
    }
    const QByteArray temp = "tmp:" + QByteArray::number(m_nextTemp++);
    m_temp.insert(temp, right);
    *rightId = temp;
    return true;
}

QByteArray BTreeTransaction::insertInto(const QByteArray &id, const BTreeEntry &entry,
                                        BTreeEntry *splitSeparator, QByteArray *splitRight,
                                        bool *ok, QString *error)
{
    NodeRef node;
    const QByteArray myId = mutableNode(id, &node, error);
    if (myId.isEmpty()) {
        *ok = false;
        return {};
    }

    if (node->leaf) {
        auto it = std::lower_bound(node->entries.begin(), node->entries.end(), entry);
        if (it == node->entries.end() || !(*it == entry)) {
            node->entries.insert(it, entry);
            ++m_delta;
        }
    } else {
        const int i = EncryptedBTree::childIndexFor(*node, entry);
        BTreeEntry childSeparator;
        QByteArray childRight;
        const QByteArray newChild = insertInto(node->children.at(i), entry, &childSeparator, &childRight, ok, error);
        if (!*ok)
            return {};
        node->children[i] = newChild;
        if (!childRight.isEmpty()) {
            node->separators.insert(i, childSeparator);
            node->children.insert(i + 1, childRight);
        }
    }

    splitRight->clear();
    splitIfNeeded(node, splitSeparator, splitRight);
    *ok = true;
    return myId;
}

bool BTreeTransaction::insert(const BTreeEntry &entry, QString *error)
{
    if (m_root.isEmpty()) {
        auto leaf = std::make_shared<BTreeNode>();
        leaf->entries.append(entry);
        m_root = "tmp:" + QByteArray::number(m_nextTemp++);
        m_temp.insert(m_root, leaf);
        m_dirty = true;
        ++m_delta;
        return true;
    }

    BTreeEntry separator;
    QByteArray right;
    bool ok = true;
    const QByteArray newRoot = insertInto(m_root, entry, &separator, &right, &ok, error);
    if (!ok)
        return false;

    if (right.isEmpty()) {
        m_root = newRoot;
    } else {
        // Gốc bị tách: cây cao thêm một tầng
        auto root = std::make_shared<BTreeNode>();
        root->leaf = false;
        root->separators = {separator};
        root->children = {newRoot, right};
        m_root = "tmp:" + QByteArray::number(m_nextTemp++);
        m_temp.insert(m_root, root);
    }
    m_dirty = true;
    return true;
}

QByteArray BTreeTransaction::removeFrom(const QByteArray &id, const BTreeEntry &entry,
                                        bool *found, bool *nowEmpty, bool *ok, QString *error)
{
    const BTreeNodePtr current = readNode(id, error);
    if (!current) {
        *ok = false;
        return {};
    }

    if (current->leaf) {
        const auto it = std::lower_bound(current->entries.cbegin(), current->entries.cend(), entry);
        if (it == current->entries.cend() || !(*it == entry)) {
            *found = false;
            *nowEmpty = current->entries.isEmpty();
            *ok = true;
            return id;
        }
        const qsizetype index = it - current->entries.cbegin();
        NodeRef node;
        const QByteArray myId = mutableNode(id, &node, error);
        if (myId.isEmpty()) {
            *ok = false;
            return {};
        }
        node->entries.removeAt(index);
        *found = true;
        *nowEmpty = node->entries.isEmpty();
        *ok = true;
        return myId;
    }

    const int i = EncryptedBTree::childIndexFor(*current, entry);
    bool childEmpty = false;
    const QByteArray newChild = removeFrom(current->children.at(i), entry, found, &childEmpty, ok, error);
    if (!*ok)
        return {};
    if (!*found) {
        *nowEmpty = false;
        return id;
    }

    NodeRef node;
    const QByteArray myId = mutableNode(id, &node, error);
    if (myId.isEmpty()) {
        *ok = false;
        return {};
    }
    if (childEmpty) {
        // Không cân bằng lại: chỉ gỡ bucket con rỗng khỏi nút cha
        if (isTemp(newChild))
            m_temp.remove(newChild);
        node->children.removeAt(i);
        if (!node->separators.isEmpty())
            node->separators.removeAt(i > 0 ? i - 1 : 0);
    } else {
        node->children[i] = newChild;
    }
    *nowEmpty = node->children.isEmpty();
    *ok = true;
    return myId;
}

bool BTreeTransaction::remove(const BTreeEntry &entry, QString *error)
{
    if (m_root.isEmpty())
        return true;

    bool found = false;
    bool empty = false;
    bool ok = true;
    const QByteArray newRoot = removeFrom(m_root, entry, &found, &empty, &ok, error);
    if (!ok)
        return false;
    if (!found)
        return true;

    m_dirty = true;
    --m_delta;
    if (empty) {
        if (isTemp(newRoot))
            m_temp.remove(newRoot);
        m_root.clear();
        return true;
    }
    m_root = newRoot;

    // Gốc là nút nhánh chỉ còn một con -> hạ chiều cao cây
    for (;;) {
        const NodeRef rootNode = m_temp.value(m_root);
        if (!rootNode || rootNode->leaf || rootNode->children.size() != 1)
            break;
        const QByteArray only = rootNode->children.first();
        m_temp.remove(m_root);
        m_root = only;
    }
    return true;
}

QByteArray BTreeTransaction::finalize(const QByteArray &id, bool *ok, QString *error)
{
    if (!isTemp(id)) {
        *ok = true;
        return id;
    }
    const NodeRef node = m_temp.value(id);
    if (!node) {
        *ok = false;
        *error = QStringLiteral("Lỗi nội bộ: mất bucket tạm %1").arg(QString::fromLatin1(id));
        return {};
    }
    if (!node->leaf) {
        for (int i = 0; i < node->children.size(); ++i) {
            node->children[i] = finalize(node->children.at(i), ok, error);
            if (!*ok)
                return {};
        }
    }

    const QByteArray realId = CryptoService::randomBytes(16);
    const QByteArray blob = m_tree->encryptNode(realId, *node);
    const QString path = EncryptedBTree::bucketPath(realId);
    if (blob.isEmpty() || !m_tree->store()->put(path, blob)) {
        *ok = false;
        *error = QStringLiteral("Không ghi được bucket: %1").arg(m_tree->store()->lastError());
        return {};
    }
    ++stats.roundTrips;
    ++stats.objectsWritten;
    stats.bytesOut += blob.size();
    m_written.append(realId);
    m_tree->cache()->insertNode(realId, std::make_shared<const BTreeNode>(*node));
    m_tree->cache()->insertBlob(path, blob);
    *ok = true;
    return realId;
}

bool BTreeTransaction::commit(QByteArray *newRoot, QString *error)
{
    if (!m_dirty) {
        *newRoot = m_root;
        return true;
    }
    bool ok = true;
    const QByteArray realRoot = m_root.isEmpty() ? QByteArray() : finalize(m_root, &ok, error);
    if (!ok)
        return false;
    m_root = realRoot;
    m_temp.clear();
    m_dirty = false;
    *newRoot = realRoot;
    return true;
}
