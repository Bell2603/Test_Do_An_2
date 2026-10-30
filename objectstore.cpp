#include "objectstore.h"

#include <QMutexLocker>
#include <QSet>

QList<QByteArray> IObjectStore::getMany(const QStringList &paths, bool *ok)
{
    QList<QByteArray> result;
    result.reserve(paths.size());
    bool allOk = true;
    for (const QString &path : paths) {
        QByteArray data;
        if (!get(path, &data)) {
            if (!notFound())
                allOk = false;
            data.clear();
        }
        result.append(data);
    }
    if (ok)
        *ok = allOk;
    return result;
}

void IObjectStore::setObserver(const Observer &observer)
{
    QMutexLocker lock(&m_observerMutex);
    m_observer = observer;
}

void IObjectStore::notify(const StoreOperation &operation) const
{
    Observer observer;
    {
        QMutexLocker lock(&m_observerMutex);
        observer = m_observer;
    }
    if (observer)
        observer(operation);
}

// ---------------- MemoryObjectStore ----------------

void MemoryObjectStore::record(const QString &op, const QString &path, qint64 in, qint64 out, bool ok)
{
    StoreOperation operation;
    operation.time = QDateTime::currentDateTime();
    operation.op = op;
    operation.path = path;
    operation.bytesIn = in;
    operation.bytesOut = out;
    operation.ok = ok;
    notify(operation);
}

bool MemoryObjectStore::get(const QString &path, QByteArray *data)
{
    bool ok = false;
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_objects.constFind(path);
        ok = it != m_objects.constEnd();
        m_notFound = !ok;
        m_lastError = ok ? QString() : QStringLiteral("Không tồn tại: %1").arg(path);
        if (ok)
            *data = it.value();
    }
    record(QStringLiteral("GET"), path, ok ? data->size() : 0, 0, ok);
    return ok;
}

bool MemoryObjectStore::getRange(const QString &path, qint64 offset, qint64 length, QByteArray *data)
{
    bool ok = false;
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_objects.constFind(path);
        ok = it != m_objects.constEnd() && offset >= 0 && offset <= it.value().size();
        m_notFound = it == m_objects.constEnd();
        m_lastError = ok ? QString() : QStringLiteral("Không đọc được: %1").arg(path);
        if (ok)
            *data = it.value().mid(offset, length);
    }
    record(QStringLiteral("RANGE"), path, ok ? data->size() : 0, 0, ok);
    return ok;
}

bool MemoryObjectStore::put(const QString &path, const QByteArray &data)
{
    {
        QMutexLocker lock(&m_mutex);
        m_objects.insert(path, data);
    }
    record(QStringLiteral("PUT"), path, 0, data.size(), true);
    return true;
}

bool MemoryObjectStore::putStream(const QString &path, const Producer &producer)
{
    QByteArray all;
    for (;;) {
        QByteArray chunk;
        if (!producer(&chunk)) {
            QMutexLocker lock(&m_mutex);
            m_lastError = QStringLiteral("Nguồn dữ liệu báo lỗi");
            return false;
        }
        if (chunk.isEmpty())
            break;
        all.append(chunk);
    }
    return put(path, all);
}

bool MemoryObjectStore::putAtomic(const QString &path, const QByteArray &data)
{
    return put(path, data);
}

bool MemoryObjectStore::remove(const QString &path)
{
    bool ok = false;
    {
        QMutexLocker lock(&m_mutex);
        ok = m_objects.remove(path) > 0;
    }
    record(QStringLiteral("RM"), path, 0, 0, ok);
    return true;
}

bool MemoryObjectStore::exists(const QString &path)
{
    bool ok = false;
    {
        QMutexLocker lock(&m_mutex);
        ok = m_objects.contains(path);
    }
    record(QStringLiteral("STAT"), path, 0, 0, ok);
    return ok;
}

QList<StoreEntry> MemoryObjectStore::list(const QString &dir)
{
    QList<StoreEntry> result;
    {
        QMutexLocker lock(&m_mutex);
        const QString prefix = dir.isEmpty() ? QString() : dir + QLatin1Char('/');
        QSet<QString> seenDirs;
        for (auto it = m_objects.constBegin(); it != m_objects.constEnd(); ++it) {
            if (!it.key().startsWith(prefix))
                continue;
            const QString rest = it.key().mid(prefix.size());
            const int slash = rest.indexOf(QLatin1Char('/'));
            if (slash < 0) {
                result.append({rest, it.value().size(), false});
            } else {
                const QString sub = rest.left(slash);
                if (!seenDirs.contains(sub)) {
                    seenDirs.insert(sub);
                    result.append({sub, 0, true});
                }
            }
        }
    }
    record(QStringLiteral("LIST"), dir, 0, 0, true);
    return result;
}

bool MemoryObjectStore::removeDirectory(const QString &dir)
{
    {
        QMutexLocker lock(&m_mutex);
        const QString prefix = dir + QLatin1Char('/');
        for (auto it = m_objects.begin(); it != m_objects.end();) {
            if (it.key().startsWith(prefix))
                it = m_objects.erase(it);
            else
                ++it;
        }
    }
    record(QStringLiteral("RMDIR"), dir, 0, 0, true);
    return true;
}

QString MemoryObjectStore::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastError;
}

bool MemoryObjectStore::notFound() const
{
    QMutexLocker lock(&m_mutex);
    return m_notFound;
}

QHash<QString, QByteArray> MemoryObjectStore::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    return m_objects;
}

void MemoryObjectStore::replace(const QString &path, const QByteArray &data)
{
    QMutexLocker lock(&m_mutex);
    m_objects.insert(path, data);
}
