#include "appconfig.h"

#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

// Đường dẫn trong config tính từ thư mục home (giữ nguyên quy ước cũ).
// Không bao giờ trả về chính thư mục home / thư mục gốc: tmpDir bị xoá đệ quy khi thoát app.
QString homeRelative(const QString &value, const QString &fallback)
{
    if (value.trimmed().isEmpty())
        return fallback;
    const QString path = QDir::cleanPath(QDir::homePath() + QLatin1Char('/') + value);
    if (path == QDir::cleanPath(QDir::homePath()) || path == QLatin1String("/"))
        return fallback;
    return path;
}

} // namespace

AppConfig &AppConfig::instance()
{
    static AppConfig config;
    return config;
}

QString AppConfig::locateConfigFile()
{
    const QString fromEnv = qEnvironmentVariable("SETP_CONFIG");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;

    // Trước đây đường dẫn bị hard-code tới một thư mục dự án cũ; nay tìm cạnh file .pro
    QDir dir(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 8; ++level) {
        if (dir.exists(QStringLiteral("config.ini")))
            return dir.filePath(QStringLiteral("config.ini"));
        if (!dir.cdUp())
            break;
    }

    const QString appConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                              + QStringLiteral("/config.ini");
    return QFileInfo::exists(appConfig) ? appConfig : QString();
}

void AppConfig::load(const QString &configPath)
{
    m_configPath = configPath;
    if (configPath.isEmpty() || !QFile::exists(configPath)) {
        m_loaded = false;
        qWarning() << "[AppConfig] config.ini not found:" << configPath;
        return;
    }

    QSettings settings(configPath, QSettings::IniFormat);

    m_host = settings.value("Server/host").toString();
    m_port = settings.value("Server/port", "22").toString();
    m_user = settings.value("Server/user").toString();
    m_password = settings.value("Server/password").toString();
    m_keyPath = settings.value("Server/keypath").toString();
    if (m_keyPath.startsWith(QStringLiteral("~/")))
        m_keyPath = QDir::homePath() + m_keyPath.mid(1);
    m_remoteDefaultPath = settings.value("Server/remote_defaultPath").toString();

    // Trước đây khoá này được đọc từ MỘT FILE CONFIG KHÁC ("/home/hung/...")
    // trong on_btnPlayStream_clicked(), nay gộp về cùng 1 file config.ini duy nhất.
    // Nhớ thêm mục [Stream] rtsp=... vào config.ini nếu chưa có.
    m_rtspLink = settings.value("Stream/rtsp").toString();

    m_tmpDir = homeRelative(settings.value("Local/tmpDir").toString(),
                            QDir::tempPath() + QStringLiteral("/setp-tmp"));
    m_localPath = homeRelative(settings.value("Local/localPath").toString(),
                               QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));

    // [Vault]: mặc định là thư mục "setp-vault" cạnh thư mục video trên server
    m_vaultRemotePath = settings.value("Vault/remotePath").toString();
    if (m_vaultRemotePath.isEmpty() && !m_remoteDefaultPath.isEmpty())
        m_vaultRemotePath = QDir::cleanPath(m_remoteDefaultPath + QStringLiteral("/../setp-vault"));
    m_bucketSize = qBound(512, settings.value("Vault/bucketSize", 8192).toInt(), 1024 * 1024);
    m_cacheSizeMB = qBound(1, settings.value("Vault/cacheSizeMB", 5).toInt(), 1024);
    m_chunkSizeKB = qBound(64, settings.value("Vault/chunkSizeKB", 1024).toInt(), 16 * 1024);

    QDir().mkpath(m_tmpDir);

    m_loaded = true;

    qDebug() << "[AppConfig] loaded from:" << configPath;
    qDebug() << "[AppConfig] host:" << m_host << "| port:" << m_port << "| user:" << m_user;
    qDebug() << "[AppConfig] vault:" << m_vaultRemotePath << "| bucket:" << m_bucketSize;
}
