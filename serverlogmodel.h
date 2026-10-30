#ifndef SERVERLOGMODEL_H
#define SERVERLOGMODEL_H

#include "objectstore.h"

#include <QAbstractTableModel>
#include <QList>

// Log mọi yêu cầu client gửi tới server = đúng những gì server quan sát được
// (Mục IV: resident adversary thấy mẫu truy cập, kích thước, thời điểm).
class ServerLogModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Time, Operation, Path, Bytes, Elapsed, ColumnCount };

    explicit ServerLogModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void append(const StoreOperation &operation);
    void clear();

    int operationCount() const { return m_total; }
    qint64 totalBytesIn() const { return m_bytesIn; }
    qint64 totalBytesOut() const { return m_bytesOut; }
    // Ước lượng RTT (ms) từ các GET nhỏ và băng thông (byte/s) từ các lần đọc lớn
    double estimatedRttMs() const;
    double estimatedBandwidth() const;

signals:
    void totalsChanged();

private:
    static constexpr int MaxRows = 5000;
    QList<StoreOperation> m_rows;
    int m_total = 0;
    qint64 m_bytesIn = 0;
    qint64 m_bytesOut = 0;
    QList<double> m_smallReadMs;
    double m_largeBytes = 0;
    double m_largeSeconds = 0;
};

#endif // SERVERLOGMODEL_H
