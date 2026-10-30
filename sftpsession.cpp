#include "sftpsession.h"

#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QString findExecutable(const QString &name)
{
    // App mở từ Finder/Qt Creator thường không có /opt/homebrew/bin trong PATH
    QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(name, {QStringLiteral("/opt/homebrew/bin"),
                                                     QStringLiteral("/usr/local/bin"),
                                                     QStringLiteral("/usr/bin")});
    return path;
}

} // namespace

SftpSession::SftpSession(QObject *parent)
    : QObject(parent)
{
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &SftpSession::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, &SftpSession::onReadyReadError);
    connect(m_process, &QProcess::finished, this, &SftpSession::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &SftpSession::onErrorOccurred);
}

bool SftpSession::isRunning() const
{
    return m_process->state() != QProcess::NotRunning;
}

void SftpSession::connectToServer(const QString &host, const QString &port,
                                   const QString &user, const QString &password,
                                   const QString &keyPath)
{
    m_connected = false;
    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_lastErrorLine.clear();

    QStringList sftpArgs;
    sftpArgs << "-P" << port                  // Cong ket noi
             << "-o" << "ConnectTimeout=10";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString program;
    QStringList args;
    if (!keyPath.isEmpty()) {
        program = findExecutable(QStringLiteral("sftp"));
        args << "-i" << keyPath << "-o" << "BatchMode=yes" << sftpArgs;
        m_usingSshpass = false;
    } else {
        program = findExecutable(QStringLiteral("sshpass"));
        if (program.isEmpty()) {
            emit startFailed("Not found sshpass (brew install sshpass)");
            return;
        }
        // Mat khau truyen qua bien moi truong (sshpass -e), khong nam tren dong lenh (ps)
        env.insert(QStringLiteral("SSHPASS"), password);
        args << "-e"                          // sshpass doc mat khau tu SSHPASS
             << "sftp" << sftpArgs;           // sshpass goi chuong trinh sftp
        m_usingSshpass = true;
    }
    args << QString("%1@%2").arg(user, host);

    m_process->setProcessEnvironment(env);
    m_process->start(program, args);

    m_stopRequested = false;
    if (!m_process->waitForStarted()) {
        return; // onErrorOccurred() đã báo lỗi
    }
    // Chỉ coi là đã kết nối khi sftp báo "Connected to" hoặc hiện dấu nhắc (xem handleLine)
}

void SftpSession::disconnectFromServer()
{
    if (isRunning()) {
        m_stopRequested = true;
        m_process->terminate();
    }
}

void SftpSession::sendCommand(const QString &cmd)
{
    if (isRunning()) {
        m_process->write((cmd + "\n").toUtf8());
    }
}

int SftpSession::sendTrackedCommand(const QString &cmd)
{
    const int token = m_nextToken++;
    sendCommand(cmd);
    // sftp chạy lệnh tuần tự: dòng đánh dấu chỉ in ra sau khi lệnh trước đã xong
    // (thay cho cách cũ: thấy "Fetching" rồi chờ 800 ms, dễ phát file chưa tải xong)
    sendCommand(QStringLiteral("!echo __SETP_DONE_%1__").arg(token));
    return token;
}

void SftpSession::markConnected()
{
    if (m_connected)
        return;
    m_connected = true;
    emit started();
}

void SftpSession::onReadyRead()
{
    m_stdoutBuffer += m_process->readAllStandardOutput();

    // Chỉ xử lý các dòng đã đủ; phần dòng bị cắt giữa hai lần đọc giữ lại lần sau
    int newline;
    while ((newline = int(m_stdoutBuffer.indexOf('\n'))) >= 0) {
        const QString line = QString::fromUtf8(m_stdoutBuffer.left(newline));
        m_stdoutBuffer.remove(0, newline + 1);
        handleLine(line);
    }
}

void SftpSession::onReadyReadError()
{
    m_stderrBuffer += m_process->readAllStandardError();
    int newline;
    while ((newline = int(m_stderrBuffer.indexOf('\n'))) >= 0) {
        const QString line = QString::fromUtf8(m_stderrBuffer.left(newline)).trimmed();
        m_stderrBuffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        if (line.contains("Connected to")) {
            markConnected();
            continue;
        }
        m_lastErrorLine = line;
    }
}

void SftpSession::handleLine(const QString &line)
{
    static const QRegularExpression marker(QStringLiteral("^__SETP_DONE_(\\d+)__$"));
    // Dòng của `ls -l`: quyền, số link, owner, group, kích thước, tháng, ngày, giờ/năm, TÊN (có thể có dấu cách)
    static const QRegularExpression longListing(
        QStringLiteral("^([dlbcps-])[rwxsStT-]{9}\\S*\\s+\\d+\\s+\\S+\\s+\\S+\\s+\\d+\\s+\\S+\\s+\\S+\\s+\\S+\\s(.+)$"));

    const QString clean = line.trimmed();
    if (clean.isEmpty())
        return;
    if (clean.contains("Connected to") || clean.startsWith("sftp>")) {
        markConnected();
        return;
    }

    const QRegularExpressionMatch done = marker.match(clean);
    if (done.hasMatch()) {
        emit commandFinished(done.captured(1).toInt());
        return;
    }

    // Bỏ qua mọi dòng không phải kết quả ls (ví dụ "Fetching ...")
    const QRegularExpressionMatch entry = longListing.match(line.trimmed());
    if (!entry.hasMatch())
        return;
    const QString type = entry.captured(1);
    QString name = entry.captured(2);
    if (type == QLatin1String("l"))
        name = name.section(QStringLiteral(" -> "), 0, 0);
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return;

    emit entryParsed(name, type == QLatin1String("d"));
}

void SftpSession::onFinished(int exitCode, QProcess::ExitStatus status)
{
    const bool wasConnected = m_connected;
    m_connected = false;
    if (!wasConnected && !m_stopRequested) {
        QString reason = m_lastErrorLine;
        if (m_usingSshpass && status == QProcess::NormalExit) {
            // Mã thoát của sshpass
            if (exitCode == 5)
                reason = QStringLiteral("Sai mật khẩu SSH");
            else if (exitCode == 6)
                reason = QStringLiteral("Host key chưa có trong known_hosts. Hãy chạy `ssh` tới server một lần để xác nhận.");
        }
        if (reason.isEmpty())
            reason = QStringLiteral("Không kết nối được (mã thoát %1)").arg(exitCode);
        emit startFailed(reason);
    }
    emit disconnected();
}

void SftpSession::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
        emit startFailed(QStringLiteral("Không chạy được sftp/sshpass: %1").arg(m_process->errorString()));
}
