#include "sftpfilemodel.h"

#include <QRegularExpression>
#include <QStyle>
#include <QApplication>

namespace {

// Trước đây regex lọc loại luôn cả thư mục nên nhánh isDir không bao giờ được hiển thị.
// Proxy này cho qua thư mục (và "..") cùng các file video, xếp thư mục lên trước.
class VideoFilterProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        const QString kind = sourceModel()->index(sourceRow, 0, sourceParent).data(Qt::UserRole).toString();
        if (kind == QLatin1String("dir") || kind == QLatin1String("up"))
            return true;
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }

    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        auto rank = [](const QModelIndex &index) {
            const QString kind = index.data(Qt::UserRole).toString();
            return kind == QLatin1String("up") ? 0 : kind == QLatin1String("dir") ? 1 : 2;
        };
        if (rank(left) != rank(right))
            return rank(left) < rank(right);
        return left.data().toString().compare(right.data().toString(), Qt::CaseInsensitive) < 0;
    }
};

} // namespace

SftpFileModel::SftpFileModel(QObject *parent)
    : QObject(parent)
{
    m_sourceModel = new QStandardItemModel(this);
    m_sourceModel->setHorizontalHeaderLabels({"Server"});

    m_proxyModel = new VideoFilterProxy(this);
    m_proxyModel->setSourceModel(m_sourceModel);

    QRegularExpression regExp(".*\\.(mp4|webm|mov)$",
                              QRegularExpression::CaseInsensitiveOption);
    m_proxyModel->setFilterRegularExpression(regExp);
    m_proxyModel->sort(0);
}

void SftpFileModel::addEntry(const QString &name, bool isDir)
{
    QStandardItem *item = new QStandardItem(name);
    item->setEditable(false);

    if (isDir) {
        item->setIcon(qApp->style()->standardIcon(QStyle::SP_DirIcon));
        item->setData("dir", Qt::UserRole);
    } else {
        item->setIcon(qApp->style()->standardIcon(QStyle::SP_MediaPlay));
        item->setData("file", Qt::UserRole);
    }

    m_sourceModel->appendRow(item);
}

void SftpFileModel::addParentEntry()
{
    QStandardItem *item = new QStandardItem("..");
    item->setEditable(false);
    item->setIcon(qApp->style()->standardIcon(QStyle::SP_FileDialogToParent));
    item->setData("up", Qt::UserRole);
    m_sourceModel->appendRow(item);
}

void SftpFileModel::clear()
{
    m_sourceModel->removeRows(0, m_sourceModel->rowCount());
}

int SftpFileModel::rowCount() const
{
    return m_sourceModel->rowCount();
}
