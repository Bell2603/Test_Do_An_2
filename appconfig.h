#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>

// Model: đọc config.ini MỘT LẦN DUY NHẤT lúc khởi động app.
// Trước đây QSettings + đường dẫn config bị hard-code lặp lại ở 3 nơi
// khác nhau trong MainWindow (connectSftp(), constructor, on_btnPlayStream_clicked()),
// với on_btnPlayStream_clicked() còn dùng một đường dẫn KHÁC hẳn ("/home/hung/...")
// nên khoá "Stream/rtsp" không đọc từ cùng 1 file config với phần còn lại.
// AppConfig gom việc này về một chỗ duy nhất.
class AppConfig
{
public:
    static AppConfig &instance();

    // Tìm config.ini: biến môi trường SETP_CONFIG, rồi đi ngược lên các thư mục cha
    // của file chạy (<dự án>/build/<kit>/App.app/Contents/MacOS -> <dự án>/config.ini),
    // cuối cùng là thư mục cấu hình của ứng dụng. Trả chuỗi rỗng nếu không thấy.
    static QString locateConfigFile();

    // Gọi 1 lần trong main.cpp hoặc constructor của MainWindow
    void load(const QString &configPath);

    QString host() const { return m_host; }
    QString port() const { return m_port; }
    QString user() const { return m_user; }
    // Mật khẩu trong config.ini, hoặc mật khẩu người dùng nhập lúc chạy (chỉ giữ trong RAM)
    QString password() const { return m_sessionPassword.isEmpty() ? m_password : m_sessionPassword; }
    void setSessionPassword(const QString &password) { m_sessionPassword = password; }
    QString keyPath() const { return m_keyPath; }
    QString remoteDefaultPath() const { return m_remoteDefaultPath; }
    QString localPath() const { return m_localPath; }
    QString tmpDir() const { return m_tmpDir; }
    QString rtspLink() const { return m_rtspLink; }
    QString configPath() const { return m_configPath; }

    // [Vault] - kho video mã hoá đầu-cuối
    QString vaultRemotePath() const { return m_vaultRemotePath; }
    int bucketSize() const { return m_bucketSize; }
    int cacheSizeMB() const { return m_cacheSizeMB; }
    int chunkSizeKB() const { return m_chunkSizeKB; }

    bool isLoaded() const { return m_loaded; }

private:
    AppConfig() = default;

    QString m_configPath;
    QString m_host;
    QString m_port;
    QString m_user;
    QString m_password;
    QString m_sessionPassword;
    QString m_keyPath;
    QString m_remoteDefaultPath;
    QString m_localPath;
    QString m_tmpDir;
    QString m_rtspLink;
    QString m_vaultRemotePath;
    int m_bucketSize = 8192;
    int m_cacheSizeMB = 5;
    int m_chunkSizeKB = 1024;
    bool m_loaded = false;
};

#endif // APPCONFIG_H
