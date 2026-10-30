#ifndef VAULTMODEL_H
#define VAULTMODEL_H

#include "vaulttypes.h"

#include <QAbstractTableModel>

// Model hiển thị danh sách video trong kho. Dữ liệu dựng lại từ chỉ mục đã giải mã
// ở client (thay cho việc parse `ls -l`, vốn làm lộ tên file cho server).
class VaultModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Name, Uploaded, Duration, Size, SharedBy, ColumnCount };

    explicit VaultModel(bool showSharedBy, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setItems(const QList<VideoMeta> &items);
    VideoMeta itemAt(int row) const;
    int rowOf(const QByteArray &oid) const;

    static QString formatDuration(qint64 ms);

private:
    QList<VideoMeta> m_items;
    bool m_showSharedBy;
};

#endif // VAULTMODEL_H
