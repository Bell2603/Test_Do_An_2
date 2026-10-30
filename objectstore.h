#ifndef OBJECTSTORE_H
#define OBJECTSTORE_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <functional>

// Một yêu cầu mà server quan sát được. Dùng cho màn hình "Góc nhìn server":
// server chỉ biết thao tác, đường dẫn (id ngẫu nhiên), số byte và thời điểm.
struct StoreOperation
{
    QDateTime time;
    QString op;           // GET, RANGE, PUT, RENAME, RM, LIST, STAT, RMDIR
    QString path;         // tương đối với gốc vault
    qint64 bytesIn = 0;   // server -> client
    qint64 bytesOut = 0;  // client -> server
    qint64 elapsedUs = 0;
    bool ok = true;
};

struct StoreEntry
{
    QString name;
    qint64 size = 0;
    bool isDir = false;
};

// Kho object phía server (không tin cậy). Đường dẫn luôn tương đối với gốc vault,
// phân cách bằng '/'. Mọi thứ đi qua đây đều đã được mã hoá ở client.
class IObjectStore
{
public:
    using Observer = std::function<void(const StoreOperation &)>;
    // Trả false khi lỗi; *chunk rỗng nghĩa là đã hết dữ liệu.
    using Producer = std::function<bool(QByteArray *chunk)>;

    virtual ~IObjectStore() = default;

    // false nếu không tồn tại hoặc lỗi (xem notFound() / lastError())
    virtual bool get(const QString &path, QByteArray *data) = 0;
    virtual bool getRange(const QString &path, qint64 offset, qint64 length, QByteArray *data) = 0;
    virtual bool put(const QString &path, const QByteArray &data) = 0;
    virtual bool putStream(const QString &path, const Producer &producer) = 0;
    // Ghi file tạm rồi rename để thay thế gần như nguyên tử (root pointer)
    virtual bool putAtomic(const QString &path, const QByteArray &data) = 0;
    virtual bool remove(const QString &path) = 0;
    virtual bool exists(const QString &path) = 0;
    virtual QList<StoreEntry> list(const QString &dir) = 0;
    virtual bool removeDirectory(const QString &dir) = 0;
    virtual QString lastError() const = 0;
    // true nếu lần get/getRange gần nhất thất bại vì object không tồn tại
    virtual bool notFound() const = 0;

    // Lấy một loạt object trong cùng một "vòng" truy vấn. Phần tử rỗng = không tồn tại.
    // *ok = false chỉ khi lỗi đường truyền.
    virtual QList<QByteArray> getMany(const QStringList &paths, bool *ok);

    void setObserver(const Observer &observer);

protected:
    void notify(const StoreOperation &operation) const;

private:
    mutable QMutex m_observerMutex;
    Observer m_observer;
};

// Kho trong bộ nhớ: dùng cho kiểm thử và để minh hoạ "server" chứa những gì.
class MemoryObjectStore : public IObjectStore
{
public:
    bool get(const QString &path, QByteArray *data) override;
    bool getRange(const QString &path, qint64 offset, qint64 length, QByteArray *data) override;
    bool put(const QString &path, const QByteArray &data) override;
    bool putStream(const QString &path, const Producer &producer) override;
    bool putAtomic(const QString &path, const QByteArray &data) override;
    bool remove(const QString &path) override;
    bool exists(const QString &path) override;
    QList<StoreEntry> list(const QString &dir) override;
    bool removeDirectory(const QString &dir) override;
    QString lastError() const override;
    bool notFound() const override;

    QHash<QString, QByteArray> snapshot() const;
    void replace(const QString &path, const QByteArray &data); // dùng để giả lập server sửa dữ liệu

private:
    void record(const QString &op, const QString &path, qint64 in, qint64 out, bool ok);

    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_objects;
    QString m_lastError;
    bool m_notFound = false;
};

#endif // OBJECTSTORE_H
