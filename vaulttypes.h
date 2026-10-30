#ifndef VAULTTYPES_H
#define VAULTTYPES_H

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

// Thống kê một truy vấn / thao tác trên kho, để đối chiếu với Mục V (hiệu năng)
// và Mục IV (những gì server quan sát được) của bài báo ZeroDB.
struct QueryStats
{
    int roundTrips = 0;      // số vòng client <-> server (mỗi tầng cây = 1 vòng)
    int objectsFetched = 0;  // số object tải về
    int cacheHits = 0;       // số bucket lấy từ cache phía client
    int objectsWritten = 0;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
    int treeHeight = 0;
    qint64 elapsedMs = 0;
    QString note;            // chiến lược đã dùng và thông tin bị lộ cho server

    void add(const QueryStats &other)
    {
        roundTrips += other.roundTrips;
        objectsFetched += other.objectsFetched;
        cacheHits += other.cacheHits;
        objectsWritten += other.objectsWritten;
        bytesIn += other.bytesIn;
        bytesOut += other.bytesOut;
        treeHeight = qMax(treeHeight, other.treeHeight);
    }
};

// Metadata của một video trong kho. Chỉ tồn tại dạng rõ ở client;
// trên server nó là object users/<uid>/meta/<oid> đã mã hoá.
struct VideoMeta
{
    QByteArray oid;          // 16 byte ngẫu nhiên, tên object trên server
    QString name;
    qint64 uploadedAt = 0;   // ms kể từ epoch (UTC)
    qint64 durationMs = 0;
    qint64 size = 0;
    QString mime;
    // Chỉ có với video người khác chia sẻ cho mình
    QString sharedBy;
    QString shareId;

    QString oidHex() const { return QString::fromLatin1(oid.toHex()); }
    bool isShared() const { return !shareId.isEmpty(); }
};

// Một lượt mình đã chia sẻ cho người khác (dùng để thu hồi)
struct ShareRecord
{
    QString shareId;
    QString recipient;
    QString recipientUid;
    QByteArray oid;
    QString videoName;
    qint64 sharedAt = 0;
};

enum class QueryField { NameExact, NamePrefix, Word, UploadDate, Duration, Size };
enum class QueryStrategy { Preorder, Prefetch };

struct VaultQuery
{
    QueryField field = QueryField::NameExact;
    QString text;
    qint64 from = 0;         // khoảng đóng [from, to]: ms (ngày), ms (thời lượng), byte
    qint64 to = 0;
    int limit = 0;           // 0 = lấy toàn bộ khoảng
    bool withDuration = false;
    qint64 durationFrom = 0;
    qint64 durationTo = 0;
    QueryStrategy strategy = QueryStrategy::Preorder;
};

struct QueryResult
{
    QList<VideoMeta> items;
    QueryStats stats;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

struct VaultSettings
{
    int bucketSize = 8192;              // Mục V.A: ~8 KB là tối ưu với mạng 1 MB/s, RTT 50 ms
    int chunkSize = 1024 * 1024;        // chunk mã hoá video
    qint64 ramCacheBytes = 5 * 1024 * 1024;
    QString diskCachePath;              // SQLite chứa bucket đã mã hoá; rỗng = không dùng
    QString cacheScope;                 // phân biệt các kho khác nhau trong cùng file cache
};

Q_DECLARE_METATYPE(QueryStats)
Q_DECLARE_METATYPE(VideoMeta)
Q_DECLARE_METATYPE(ShareRecord)
Q_DECLARE_METATYPE(QueryResult)

#endif // VAULTTYPES_H
