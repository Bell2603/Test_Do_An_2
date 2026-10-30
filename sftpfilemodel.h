#ifndef SFTPFILEMODEL_H
#define SFTPFILEMODEL_H

#include <QObject>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>

// Model: chỉ lo giữ dữ liệu danh sách file/thư mục trên server.
// Không biết gì về QProcess (đó là việc của SftpSession) hay về UI.
class SftpFileModel : public QObject
{
    Q_OBJECT
public:
    explicit SftpFileModel(QObject *parent = nullptr);

    // Model đã qua filter (thư mục + mp4/webm/mov) - gắn thẳng vào QTreeView::setModel()
    QAbstractItemModel *proxyModel() const { return m_proxyModel; }

    void addEntry(const QString &name, bool isDir);
    void addParentEntry(); // mục ".." để quay lại thư mục cha
    void clear();
    int rowCount() const;

private:
    QStandardItemModel *m_sourceModel;
    QSortFilterProxyModel *m_proxyModel;
};

#endif // SFTPFILEMODEL_H
