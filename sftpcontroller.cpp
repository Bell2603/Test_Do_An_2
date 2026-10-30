#include "sftpcontroller.h"
#include "appconfig.h"
#include "localfilecontroller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

SftpController::SftpController(QObject *parent)
    : QObject(parent)
{
    m_session = new SftpSession(this);
    m_fileModel = new SftpFileModel(this);

    connect(m_session, &SftpSession::started, this, &SftpController::handleStarted);
    connect(m_session, &SftpSession::entryParsed, this, &SftpController::handleEntryParsed);
    connect(m_session, &SftpSession::commandFinished, this, &SftpController::handleCommandFinished);
    connect(m_session, &SftpSession::disconnected, this, &SftpController::handleDisconnected);
    connect(m_session, &SftpSession::startFailed, this, &SftpController::connectionFailed);
}

QString SftpController::joinPath(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    return dir.endsWith('/') ? dir + name : dir + "/" + name;
}

QString SftpController::quoted(const QString &path)
{
    QString escaped = path;
    escaped.replace("\\", "\\\\").replace("\"", "\\\"");
    return "\"" + escaped + "\"";
}

QString SftpController::remotePath(const QString &name) const
{
    // Moi duong dan remote deu tao qua ham nay (truoc day downloadToLocal thieu dau "/")
    return joinPath(m_currentPath, name);
}

void SftpController::toggleConnection()
{
    if (m_session->isRunning()) {
        // Neu co Process chay truoc, hoac khi ban disconnect
        // -> handleDisconnected() se don model va bao ve View
        m_session->disconnectFromServer();
        return;
    }

    const auto &config = AppConfig::instance();
    m_rootPath = config.remoteDefaultPath();
    m_currentPath = m_rootPath;
    m_session->connectToServer(config.host(), config.port(), config.user(), config.password(), config.keyPath());
}

void SftpController::handleStarted()
{
    emit connectionStateChanged(true);
    listCurrentDirectory();
}

void SftpController::handleDisconnected()
{
    m_fileModel->clear();
    for (const PendingCommand &pending : std::as_const(m_pending)) {
        if (!pending.partPath.isEmpty())
            QFile::remove(pending.partPath);
    }
    m_pending.clear();
    emit connectionStateChanged(false);
}

void SftpController::listCurrentDirectory()
{
    m_fileModel->clear();
    if (QDir::cleanPath(m_currentPath) != QDir::cleanPath(m_rootPath))
        m_fileModel->addParentEntry();
    m_session->sendCommand("cd " + quoted(m_currentPath));
    m_session->sendCommand("ls -l");
}

void SftpController::refresh()
{
    if (!m_session->isConnected())
        return;
    listCurrentDirectory();
}

void SftpController::openEntry(const QString &name, EntryKind kind)
{
    if (!m_session->isConnected())
        return;

    if (kind == EntryKind::Parent) {
        const QString parent = QDir::cleanPath(m_currentPath + "/..");
        // Khong di ra ngoai thu muc goc trong config
        m_currentPath = parent.startsWith(QDir::cleanPath(m_rootPath)) ? parent : m_rootPath;
        listCurrentDirectory();
        return;
    }
    if (kind == EntryKind::Directory) {
        m_currentPath = joinPath(m_currentPath, name);
        listCurrentDirectory();
        return;
    }

    const auto &config = AppConfig::instance();
    const QString localPath = joinPath(config.localPath(), name);
    const QString tmpPath = joinPath(config.tmpDir(), name);

    // Neu file da ton tai phat luon, khong tai lai
    if (QFile::exists(localPath)) {
        emit videoReadyToPlay(localPath);
        return;
    }
    if (QFile::exists(tmpPath)) {
        emit videoReadyToPlay(tmpPath);
        return;
    }

    // Chua co, tai ve thu muc tam roi phat
    startDownload(name, tmpPath, true);
}

void SftpController::startDownload(const QString &fileName, const QString &finalPath, bool playWhenDone)
{
    PendingCommand pending;
    pending.type = PendingCommand::Download;
    pending.fileName = fileName;
    pending.finalPath = finalPath;
    pending.partPath = finalPath + ".part";
    pending.playWhenDone = playWhenDone;
    QFile::remove(pending.partPath);

    const int token = m_session->sendTrackedCommand(
        QString("get %1 %2").arg(quoted(remotePath(fileName)), quoted(pending.partPath)));
    m_pending.insert(token, pending);
}

void SftpController::downloadToLocal(const QString &fileName)
{
    if (!m_session->isConnected())
        return;
    const QString localPath = joinPath(AppConfig::instance().localPath(), fileName);
    startDownload(fileName, localPath, false);
}

void SftpController::renameRemote(const QString &oldName, const QString &newName)
{
    if (!m_session->isConnected() || newName.trimmed().isEmpty() || newName.contains('/'))
        return;
    // Giu duoi file goc (truoc day luon gan ".mp4" ke ca file .webm/.mov)
    const QString finalName = LocalFileController::withOriginalSuffix(oldName, newName.trimmed());
    const int token = m_session->sendTrackedCommand(
        "rename " + quoted(remotePath(oldName)) + " " + quoted(remotePath(finalName)));
    m_pending.insert(token, PendingCommand());
}

void SftpController::handleEntryParsed(const QString &name, bool isDir)
{
    m_fileModel->addEntry(name, isDir);
}

void SftpController::handleCommandFinished(int token)
{
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return;
    const PendingCommand pending = it.value();
    m_pending.erase(it);

    if (pending.type == PendingCommand::Refresh) {
        listCurrentDirectory();
        return;
    }

    // sftp da chay xong lenh get: file .part day du thi moi doi ten va phat
    const QFileInfo part(pending.partPath);
    if (!part.exists() || part.size() == 0) {
        QFile::remove(pending.partPath);
        emit downloadFailed(pending.fileName);
        return;
    }
    QFile::remove(pending.finalPath);
    if (!QFile::rename(pending.partPath, pending.finalPath)) {
        emit downloadFailed(pending.fileName);
        return;
    }
    if (pending.playWhenDone)
        emit videoReadyToPlay(pending.finalPath);
    else
        emit downloadFinished(pending.finalPath);
}
