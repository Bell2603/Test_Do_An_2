#include "localfilecontroller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

LocalFileController::LocalFileController(QObject *parent)
    : QObject(parent)
{
}

QString LocalFileController::withOriginalSuffix(const QString &oldName, const QString &newName)
{
    const QString suffix = QFileInfo(oldName).suffix();
    if (suffix.isEmpty() || newName.endsWith("." + suffix, Qt::CaseInsensitive))
        return newName;
    return newName + "." + suffix;
}

bool LocalFileController::renameFile(const QString &oldPath, const QString &newName, QString *error)
{
    const QFileInfo info(oldPath);
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty() || trimmed.contains('/')) {
        if (error)
            *error = "Tên mới không hợp lệ";
        return false;
    }
    const QString newPath = info.dir().filePath(withOriginalSuffix(info.fileName(), trimmed));
    if (QFile::exists(newPath)) {
        if (error)
            *error = "Đã có file cùng tên: " + newPath;
        return false;
    }
    QFile file(oldPath);
    if (!file.rename(newPath)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool LocalFileController::deleteFile(const QString &filePath, QString *error)
{
    if (!QFileInfo(filePath).isFile()) {
        if (error)
            *error = "Không phải file: " + filePath;
        return false;
    }
    QFile file(filePath);
    if (!file.remove()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
