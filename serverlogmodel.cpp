#include "serverlogmodel.h"

#include <QColor>
#include <QLocale>

#include <algorithm>

ServerLogModel::ServerLogModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int ServerLogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int ServerLogModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ServerLogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const StoreOperation &op = m_rows.at(index.row());
    if (role == Qt::ForegroundRole && !op.ok)
        return QColor(Qt::red);
    if (role == Qt::TextAlignmentRole && (index.column() == Bytes || index.column() == Elapsed))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
        return {};

    switch (index.column()) {
    case Time:
        return op.time.toString(QStringLiteral("HH:mm:ss.zzz"));
    case Operation:
        return op.ok ? op.op : op.op + QStringLiteral(" ✗");
    case Path:
        return op.path;
    case Bytes: {
        const qint64 bytes = op.bytesIn + op.bytesOut;
        if (bytes == 0)
            return QString();
        return (op.bytesIn ? QStringLiteral("↓ ") : QStringLiteral("↑ "))
               + QLocale::system().formattedDataSize(bytes);
    }
    case Elapsed:
        return QString::number(op.elapsedUs / 1000.0, 'f', 1);
    default:
        return {};
    }
}

QVariant ServerLogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Time: return QStringLiteral("Thời điểm");
    case Operation: return QStringLiteral("Thao tác");
    case Path: return QStringLiteral("Object (server thấy)");
    case Bytes: return QStringLiteral("Byte");
    case Elapsed: return QStringLiteral("ms");
    default: return {};
    }
}

void ServerLogModel::append(const StoreOperation &operation)
{
    if (m_rows.size() >= MaxRows) {
        beginRemoveRows(QModelIndex(), 0, 0);
        m_rows.removeFirst();
        endRemoveRows();
    }
    beginInsertRows(QModelIndex(), int(m_rows.size()), int(m_rows.size()));
    m_rows.append(operation);
    endInsertRows();

    ++m_total;
    m_bytesIn += operation.bytesIn;
    m_bytesOut += operation.bytesOut;
    const bool read = operation.op == QLatin1String("GET") || operation.op == QLatin1String("RANGE");
    if (read && operation.ok) {
        if (operation.bytesIn <= 16 * 1024) {
            m_smallReadMs.append(operation.elapsedUs / 1000.0);
            if (m_smallReadMs.size() > 200)
                m_smallReadMs.removeFirst();
        } else if (operation.bytesIn >= 256 * 1024 && operation.elapsedUs > 0) {
            m_largeBytes += operation.bytesIn;
            m_largeSeconds += operation.elapsedUs / 1e6;
        }
    }
    emit totalsChanged();
}

void ServerLogModel::clear()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
    m_total = 0;
    m_bytesIn = 0;
    m_bytesOut = 0;
    m_smallReadMs.clear();
    m_largeBytes = 0;
    m_largeSeconds = 0;
    emit totalsChanged();
}

double ServerLogModel::estimatedRttMs() const
{
    if (m_smallReadMs.isEmpty())
        return 0;
    QList<double> sorted = m_smallReadMs;
    std::sort(sorted.begin(), sorted.end());
    return sorted.at(sorted.size() / 2);
}

double ServerLogModel::estimatedBandwidth() const
{
    return m_largeSeconds > 0 ? m_largeBytes / m_largeSeconds : 0;
}
