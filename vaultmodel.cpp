#include "vaultmodel.h"

#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QStyle>

VaultModel::VaultModel(bool showSharedBy, QObject *parent)
    : QAbstractTableModel(parent)
    , m_showSharedBy(showSharedBy)
{
}

int VaultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

int VaultModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_showSharedBy ? ColumnCount : ColumnCount - 1;
}

QString VaultModel::formatDuration(qint64 ms)
{
    const qint64 seconds = ms / 1000;
    if (seconds >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(seconds / 3600)
            .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QVariant VaultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const VideoMeta &meta = m_items.at(index.row());

    if (role == Qt::DecorationRole && index.column() == Name)
        return qApp->style()->standardIcon(QStyle::SP_MediaPlay);
    if (role == Qt::TextAlignmentRole && (index.column() == Duration || index.column() == Size))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole && index.column() == Name)
        return QStringLiteral("%1\nTrên server: obj/%2").arg(meta.name, meta.oidHex());
    if (role != Qt::DisplayRole)
        return {};

    switch (index.column()) {
    case Name:
        return meta.name;
    case Uploaded:
        return QDateTime::fromMSecsSinceEpoch(meta.uploadedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case Duration:
        return meta.durationMs > 0 ? formatDuration(meta.durationMs) : QStringLiteral("?");
    case Size:
        return QLocale::system().formattedDataSize(meta.size);
    case SharedBy:
        return meta.sharedBy;
    default:
        return {};
    }
}

QVariant VaultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return QStringLiteral("Tên");
    case Uploaded: return QStringLiteral("Upload");
    case Duration: return QStringLiteral("Thời lượng");
    case Size: return QStringLiteral("Dung lượng");
    case SharedBy: return QStringLiteral("Chia sẻ bởi");
    default: return {};
    }
}

void VaultModel::setItems(const QList<VideoMeta> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

VideoMeta VaultModel::itemAt(int row) const
{
    return m_items.value(row);
}

int VaultModel::rowOf(const QByteArray &oid) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).oid == oid)
            return i;
    }
    return -1;
}
