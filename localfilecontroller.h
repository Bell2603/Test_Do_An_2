#ifndef LOCALFILECONTROLLER_H
#define LOCALFILECONTROLLER_H

#include <QObject>
#include <QString>

// Controller nhỏ: xử lý rename/delete file trên máy local.
// Tách khỏi MainWindow để View không tự ý đụng vào QFile.
class LocalFileController : public QObject
{
    Q_OBJECT
public:
    explicit LocalFileController(QObject *parent = nullptr);

    // Giữ đuôi của tên cũ nếu tên mới chưa có (vd. "a.webm" + "b" -> "b.webm")
    static QString withOriginalSuffix(const QString &oldName, const QString &newName);

    // Nhận đường dẫn đầy đủ (lấy từ QFileSystemModel) nên đúng cả với file trong thư mục con
    bool renameFile(const QString &oldPath, const QString &newName, QString *error = nullptr);
    bool deleteFile(const QString &filePath, QString *error = nullptr);
};

#endif // LOCALFILECONTROLLER_H
