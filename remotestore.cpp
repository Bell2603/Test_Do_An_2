#include "remotestore.h"
#include "cryptoservice.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::once_flag g_libssh2Init;

constexpr qint64 kReadBlock = 256 * 1024;
constexpr int kConnectTimeoutMs = 10000;

LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(keyboardInteractive)
{
    Q_UNUSED(name)
    Q_UNUSED(name_len)
    Q_UNUSED(instruction)
    Q_UNUSED(instruction_len)
    Q_UNUSED(prompts)
    // Trả mật khẩu cho mọi prompt (macOS sshd thường chỉ cho keyboard-interactive)
    const auto *password = static_cast<const QByteArray *>(*abstract);
    for (int i = 0; i < num_prompts; ++i) {
        responses[i].text = static_cast<char *>(malloc(size_t(password->size()) + 1));
        std::memcpy(responses[i].text, password->constData(), size_t(password->size()));
        responses[i].length = static_cast<unsigned int>(password->size());
    }
}

int knownHostKeyBits(int hostKeyType)
{
    switch (hostKeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

int connectWithTimeout(const QString &host, int port, QString *error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const int rc = getaddrinfo(host.toUtf8().constData(), QByteArray::number(port).constData(), &hints, &addresses);
    if (rc != 0) {
        *error = QStringLiteral("Không phân giải được %1: %2").arg(host, QString::fromUtf8(gai_strerror(rc)));
        return -1;
    }

    int connected = -1;
    for (addrinfo *a = addresses; a && connected < 0; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        int one = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); // tránh SIGPIPE làm sập app
#endif
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int result = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (result != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, kConnectTimeoutMs) == 1) {
                int soError = 0;
                socklen_t length = sizeof(soError);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
                result = soError == 0 ? 0 : -1;
                errno = soError;
            } else {
                result = -1;
                errno = ETIMEDOUT;
            }
        }
        if (result == 0) {
            fcntl(fd, F_SETFL, flags);
            connected = fd;
        } else {
            *error = QStringLiteral("Không kết nối được %1:%2 (%3)")
                         .arg(host).arg(port).arg(QString::fromLocal8Bit(strerror(errno)));
            ::close(fd);
        }
    }
    freeaddrinfo(addresses);
    return connected;
}

} // namespace

RemoteStore::RemoteStore(const RemoteStoreConfig &config)
    : m_config(config)
{
    m_config.rootPath = QDir::cleanPath(m_config.rootPath);
    m_passwordBytes = m_config.password.toUtf8();
}

RemoteStore::~RemoteStore()
{
    QMutexLocker lock(&m_mutex);
    closeUnlocked();
    CryptoService::wipe(m_passwordBytes);
}

bool RemoteStore::connectToServer(bool acceptNewHostKey, HostKeyStatus *status, QString *fingerprint)
{
    QMutexLocker lock(&m_mutex);
    return connectUnlocked(acceptNewHostKey, status, fingerprint);
}

void RemoteStore::disconnectFromServer()
{
    QMutexLocker lock(&m_mutex);
    closeUnlocked();
    m_hostTrusted = false;
    CryptoService::wipe(m_passwordBytes);
}

bool RemoteStore::isConnected() const
{
    QMutexLocker lock(&m_mutex);
    return m_session && m_sftp;
}

void RemoteStore::closeUnlocked()
{
    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "SETP vault closed");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
}

QString RemoteStore::sessionError() const
{
    if (!m_session)
        return QStringLiteral("chưa kết nối");
    char *message = nullptr;
    int length = 0;
    libssh2_session_last_error(m_session, &message, &length, 0);
    return message ? QString::fromUtf8(message, length) : QString();
}

void RemoteStore::setError(const QString &message)
{
    m_lastError = message;
}

bool RemoteStore::connectUnlocked(bool acceptNewHostKey, HostKeyStatus *status, QString *fingerprint)
{
    closeUnlocked();
    std::call_once(g_libssh2Init, [] { libssh2_init(0); });
    if (status)
        *status = HostKeyStatus::Failure;

    QString error;
    m_socket = connectWithTimeout(m_config.host, m_config.port, &error);
    if (m_socket < 0) {
        setError(error);
        return false;
    }

    m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, &m_passwordBytes);
    if (!m_session) {
        setError(QStringLiteral("Không khởi tạo được phiên SSH"));
        closeUnlocked();
        return false;
    }
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, 20000);
    if (libssh2_session_handshake(m_session, m_socket) != 0) {
        setError(QStringLiteral("Bắt tay SSH thất bại: %1").arg(sessionError()));
        closeUnlocked();
        return false;
    }

    // Kiểm tra host key: chống giả mạo server (MITM)
    size_t keyLength = 0;
    int keyType = 0;
    const char *key = libssh2_session_hostkey(m_session, &keyLength, &keyType);
    const char *hash = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!key || !hash) {
        setError(QStringLiteral("Server không gửi host key"));
        closeUnlocked();
        return false;
    }
    const QString print = QStringLiteral("SHA256:")
                          + QString::fromLatin1(QByteArray(hash, 32).toBase64(QByteArray::OmitTrailingEquals));
    if (fingerprint)
        *fingerprint = print;

    const HostKeyStatus hostStatus = checkHostKey(key, keyLength, keyType);
    if (status)
        *status = hostStatus;
    if (hostStatus == HostKeyStatus::Mismatch) {
        setError(QStringLiteral("Host key của %1 KHÔNG khớp known_hosts (%2). Có thể đang bị giả mạo server.")
                     .arg(m_config.host, print));
        closeUnlocked();
        return false;
    }
    if (hostStatus != HostKeyStatus::Match) {
        if (!acceptNewHostKey) {
            setError(QStringLiteral("Host key của %1 chưa được xác nhận (%2)").arg(m_config.host, print));
            closeUnlocked();
            return false;
        }
        rememberHostKey(key, keyLength, keyType);
    }

    if (!authenticate()) {
        closeUnlocked();
        return false;
    }

    m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp) {
        setError(QStringLiteral("Không mở được kênh SFTP: %1").arg(sessionError()));
        closeUnlocked();
        return false;
    }
    libssh2_keepalive_config(m_session, 1, 30);
    m_hostTrusted = true;
    m_lastError.clear();
    return true;
}

RemoteStore::HostKeyStatus RemoteStore::checkHostKey(const char *key, size_t keyLength, int keyType)
{
    const QStringList files = {QDir::homePath() + QStringLiteral("/.ssh/known_hosts"), m_config.knownHostsFile};
    const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostKeyBits(keyType);
    bool matched = false;
    bool mismatched = false;
    for (const QString &file : files) {
        if (file.isEmpty() || !QFileInfo::exists(file))
            continue;
        LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(m_session);
        if (!hosts)
            continue;
        libssh2_knownhost_readfile(hosts, QFile::encodeName(file).constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        const int result = libssh2_knownhost_checkp(hosts, m_config.host.toUtf8().constData(), m_config.port,
                                                    key, keyLength, typeMask, nullptr);
        libssh2_knownhost_free(hosts);
        if (result == LIBSSH2_KNOWNHOST_CHECK_MATCH)
            matched = true;
        else if (result == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
            mismatched = true;
    }
    if (mismatched)
        return HostKeyStatus::Mismatch;
    return matched ? HostKeyStatus::Match : HostKeyStatus::NotFound;
}

bool RemoteStore::rememberHostKey(const char *key, size_t keyLength, int keyType)
{
    // Chỉ ghi vào known_hosts riêng của ứng dụng, không động vào ~/.ssh
    if (m_config.knownHostsFile.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(m_config.knownHostsFile).absolutePath());
    LIBSSH2_KNOWNHOSTS *hosts = libssh2_knownhost_init(m_session);
    if (!hosts)
        return false;
    const QByteArray file = QFile::encodeName(m_config.knownHostsFile);
    if (QFileInfo::exists(m_config.knownHostsFile))
        libssh2_knownhost_readfile(hosts, file.constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    const QByteArray hostEntry = m_config.port == 22
                                     ? m_config.host.toUtf8()
                                     : QStringLiteral("[%1]:%2").arg(m_config.host).arg(m_config.port).toUtf8();
    const QByteArray comment("setp");
    const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostKeyBits(keyType);
    const bool ok = libssh2_knownhost_addc(hosts, hostEntry.constData(), nullptr, key, keyLength,
                                           comment.constData(), size_t(comment.size()), typeMask, nullptr) == 0
                    && libssh2_knownhost_writefile(hosts, file.constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0;
    libssh2_knownhost_free(hosts);
    return ok;
}

bool RemoteStore::authenticate()
{
    const QByteArray user = m_config.user.toUtf8();
    const auto userLength = static_cast<unsigned int>(user.size());
    char *methods = libssh2_userauth_list(m_session, user.constData(), userLength);
    const QString methodList = methods ? QString::fromLatin1(methods) : QString();
    bool authenticated = libssh2_userauth_authenticated(m_session) != 0;

    if (!authenticated && !m_config.keyPath.isEmpty() && methodList.contains(QLatin1String("publickey"))) {
        const QByteArray privateKey = QFile::encodeName(m_config.keyPath);
        const QString publicPath = m_config.keyPath + QStringLiteral(".pub");
        const QByteArray publicKey = QFileInfo::exists(publicPath) ? QFile::encodeName(publicPath) : QByteArray();
        authenticated = libssh2_userauth_publickey_fromfile_ex(m_session, user.constData(), userLength,
                                                               publicKey.isEmpty() ? nullptr : publicKey.constData(),
                                                               privateKey.constData(), "") == 0;
    }
    if (!authenticated && !m_passwordBytes.isEmpty() && methodList.contains(QLatin1String("password"))) {
        authenticated = libssh2_userauth_password_ex(m_session, user.constData(), userLength,
                                                     m_passwordBytes.constData(),
                                                     static_cast<unsigned int>(m_passwordBytes.size()), nullptr) == 0;
    }
    if (!authenticated && !m_passwordBytes.isEmpty() && methodList.contains(QLatin1String("keyboard-interactive"))) {
        authenticated = libssh2_userauth_keyboard_interactive_ex(m_session, user.constData(), userLength,
                                                                 &keyboardInteractive) == 0;
    }
    if (!authenticated) {
        setError(QStringLiteral("Xác thực SSH thất bại cho %1@%2 (server cho phép: %3)")
                     .arg(m_config.user, m_config.host, methodList));
        return false;
    }
    return true;
}

bool RemoteStore::ensureConnected()
{
    if (m_session && m_sftp)
        return true;
    if (!m_hostTrusted) {
        setError(QStringLiteral("Chưa kết nối tới server"));
        return false;
    }
    // Phiên bị rớt (ví dụ máy ngủ): nối lại với host key đã tin cậy
    return connectUnlocked(false, nullptr, nullptr);
}

bool RemoteStore::isSocketError() const
{
    if (!m_session)
        return true;
    const int code = libssh2_session_last_errno(m_session);
    return code == LIBSSH2_ERROR_SOCKET_SEND || code == LIBSSH2_ERROR_SOCKET_RECV
           || code == LIBSSH2_ERROR_SOCKET_DISCONNECT || code == LIBSSH2_ERROR_SOCKET_TIMEOUT
           || code == LIBSSH2_ERROR_TIMEOUT || code == LIBSSH2_ERROR_CHANNEL_CLOSED;
}

QString RemoteStore::fullPath(const QString &relative) const
{
    if (relative.isEmpty())
        return m_config.rootPath;
    return m_config.rootPath + QLatin1Char('/') + relative;
}

QByteArray RemoteStore::encodedPath(const QString &relative) const
{
    return fullPath(relative).toUtf8();
}

void RemoteStore::report(const QString &op, const QString &path, qint64 in, qint64 out, qint64 elapsedUs, bool ok)
{
    StoreOperation operation;
    operation.time = QDateTime::currentDateTime();
    operation.op = op;
    operation.path = path;
    operation.bytesIn = in;
    operation.bytesOut = out;
    operation.elapsedUs = elapsedUs;
    operation.ok = ok;
    notify(operation);
}

// ---------------- Thao tác SFTP (đã giữ khoá) ----------------

bool RemoteStore::readUnlocked(const QString &path, qint64 offset, qint64 length, QByteArray *data)
{
    m_notFound = false;
    const QByteArray full = encodedPath(path);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()),
                                                       LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!handle) {
        m_notFound = libssh2_sftp_last_error(m_sftp) == LIBSSH2_FX_NO_SUCH_FILE;
        setError(m_notFound ? QStringLiteral("Không tồn tại: %1").arg(path)
                            : QStringLiteral("Không mở được %1: %2").arg(path, sessionError()));
        return false;
    }
    if (offset > 0)
        libssh2_sftp_seek64(handle, libssh2_uint64_t(offset));

    QByteArray out;
    if (length > 0)
        out.reserve(length);
    QByteArray buffer(int(kReadBlock), Qt::Uninitialized);
    bool ok = true;
    for (;;) {
        qint64 want = kReadBlock;
        if (length >= 0)
            want = qMin(want, length - out.size());
        if (want <= 0)
            break;
        const ssize_t n = libssh2_sftp_read(handle, buffer.data(), size_t(want));
        if (n < 0) {
            setError(QStringLiteral("Lỗi đọc %1: %2").arg(path, sessionError()));
            ok = false;
            break;
        }
        if (n == 0)
            break;
        out.append(buffer.constData(), qsizetype(n));
    }
    libssh2_sftp_close_handle(handle);
    if (ok)
        *data = out;
    return ok;
}

bool RemoteStore::writeUnlocked(const QString &path, const Producer &producer)
{
    const QByteArray full = encodedPath(path);
    const unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC;
    const long mode = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR;
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()),
                                                       flags, mode, LIBSSH2_SFTP_OPENFILE);
    if (!handle && libssh2_sftp_last_error(m_sftp) == LIBSSH2_FX_NO_SUCH_FILE) {
        const int slash = int(path.lastIndexOf(QLatin1Char('/')));
        if (slash > 0 && mkdirsUnlocked(path.left(slash)))
            handle = libssh2_sftp_open_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()),
                                          flags, mode, LIBSSH2_SFTP_OPENFILE);
    }
    if (!handle) {
        setError(QStringLiteral("Không tạo được %1: %2").arg(path, sessionError()));
        return false;
    }

    bool ok = true;
    for (;;) {
        QByteArray chunk;
        if (!producer(&chunk)) {
            setError(QStringLiteral("Nguồn dữ liệu báo lỗi khi ghi %1").arg(path));
            ok = false;
            break;
        }
        if (chunk.isEmpty())
            break;
        qsizetype written = 0;
        while (written < chunk.size()) {
            const ssize_t n = libssh2_sftp_write(handle, chunk.constData() + written, size_t(chunk.size() - written));
            if (n < 0) {
                setError(QStringLiteral("Lỗi ghi %1: %2").arg(path, sessionError()));
                ok = false;
                break;
            }
            written += n;
        }
        if (!ok)
            break;
    }
    libssh2_sftp_close_handle(handle);
    return ok;
}

bool RemoteStore::mkdirsUnlocked(const QString &relativeDir)
{
    // Tạo lần lượt từng cấp (kể cả thư mục gốc vault nếu chưa có)
    const QString target = fullPath(relativeDir);
    const QStringList parts = target.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current = target.startsWith(QLatin1Char('/')) ? QStringLiteral("") : QStringLiteral(".");
    bool ok = true;
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        const QByteArray encoded = current.toUtf8();
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        if (libssh2_sftp_stat_ex(m_sftp, encoded.constData(), static_cast<unsigned int>(encoded.size()),
                                 LIBSSH2_SFTP_STAT, &attrs) == 0)
            continue;
        ok = libssh2_sftp_mkdir_ex(m_sftp, encoded.constData(), static_cast<unsigned int>(encoded.size()),
                                   LIBSSH2_SFTP_S_IRWXU) == 0;
        if (!ok) {
            setError(QStringLiteral("Không tạo được thư mục %1: %2").arg(current, sessionError()));
            break;
        }
    }
    return ok;
}

bool RemoteStore::removeUnlocked(const QString &path)
{
    const QByteArray full = encodedPath(path);
    if (libssh2_sftp_unlink_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size())) == 0)
        return true;
    if (libssh2_sftp_last_error(m_sftp) == LIBSSH2_FX_NO_SUCH_FILE)
        return true;
    setError(QStringLiteral("Không xoá được %1: %2").arg(path, sessionError()));
    return false;
}

bool RemoteStore::renameUnlocked(const QString &from, const QString &to)
{
    const QByteArray source = encodedPath(from);
    const QByteArray target = encodedPath(to);
    const long flags = LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;
    auto attempt = [&]() {
        return libssh2_sftp_rename_ex(m_sftp, source.constData(), static_cast<unsigned int>(source.size()),
                                      target.constData(), static_cast<unsigned int>(target.size()), flags) == 0;
    };
    if (attempt())
        return true;
    // SFTP v3 (OpenSSH) không ghi đè khi rename -> xoá đích rồi thử lại
    libssh2_sftp_unlink_ex(m_sftp, target.constData(), static_cast<unsigned int>(target.size()));
    if (attempt())
        return true;
    setError(QStringLiteral("Không đổi tên %1 -> %2: %3").arg(from, to, sessionError()));
    return false;
}

QList<StoreEntry> RemoteStore::listUnlocked(const QString &dir, bool *ok)
{
    QList<StoreEntry> result;
    const QByteArray full = encodedPath(dir);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()),
                                                       0, 0, LIBSSH2_SFTP_OPENDIR);
    if (!handle) {
        const bool missing = libssh2_sftp_last_error(m_sftp) == LIBSSH2_FX_NO_SUCH_FILE;
        *ok = missing; // thư mục chưa có = danh sách rỗng
        if (!missing)
            setError(QStringLiteral("Không liệt kê được %1: %2").arg(dir, sessionError()));
        return result;
    }
    char name[1024];
    for (;;) {
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        const int n = libssh2_sftp_readdir_ex(handle, name, sizeof(name), nullptr, 0, &attrs);
        if (n <= 0)
            break;
        const QString entryName = QString::fromUtf8(name, n);
        if (entryName == QLatin1String(".") || entryName == QLatin1String(".."))
            continue;
        StoreEntry entry;
        entry.name = entryName;
        entry.size = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) ? qint64(attrs.filesize) : 0;
        entry.isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        result.append(entry);
    }
    libssh2_sftp_close_handle(handle);
    *ok = true;
    return result;
}

bool RemoteStore::removeTreeUnlocked(const QString &dir)
{
    bool ok = true;
    const QList<StoreEntry> entries = listUnlocked(dir, &ok);
    if (!ok)
        return false;
    for (const StoreEntry &entry : entries) {
        const QString child = dir + QLatin1Char('/') + entry.name;
        if (entry.isDir) {
            if (!removeTreeUnlocked(child))
                return false;
        } else if (!removeUnlocked(child)) {
            return false;
        }
    }
    const QByteArray full = encodedPath(dir);
    libssh2_sftp_rmdir_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()));
    return true;
}

// ---------------- IObjectStore ----------------

bool RemoteStore::get(const QString &path, QByteArray *data)
{
    return getRange(path, 0, -1, data);
}

bool RemoteStore::getRange(const QString &path, qint64 offset, qint64 length, QByteArray *data)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    bool ok = ensureConnected() && readUnlocked(path, offset, length, data);
    if (!ok && !m_notFound && isSocketError()) {
        closeUnlocked();
        ok = ensureConnected() && readUnlocked(path, offset, length, data);
    }
    report(length < 0 ? QStringLiteral("GET") : QStringLiteral("RANGE"), path,
           ok ? data->size() : 0, 0, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

QList<QByteArray> RemoteStore::getMany(const QStringList &paths, bool *ok)
{
    // Giữ khoá suốt cả loạt để các yêu cầu của một tầng đi liền nhau
    QMutexLocker lock(&m_mutex);
    QList<QByteArray> result;
    bool allOk = true;
    for (const QString &path : paths) {
        QByteArray data;
        if (!get(path, &data)) {
            if (!m_notFound)
                allOk = false;
            data.clear();
        }
        result.append(data);
    }
    if (ok)
        *ok = allOk;
    return result;
}

bool RemoteStore::put(const QString &path, const QByteArray &data)
{
    bool sent = false;
    return putStream(path, [&](QByteArray *chunk) {
        if (sent)
            chunk->clear();
        else
            *chunk = data;
        sent = true;
        return true;
    });
}

bool RemoteStore::putStream(const QString &path, const Producer &producer)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    qint64 total = 0;
    const Producer counting = [&](QByteArray *chunk) {
        const bool ok = producer(chunk);
        total += chunk->size();
        return ok;
    };
    // Không tự thử lại vì producer có thể đã tiêu thụ một phần dữ liệu nguồn
    const bool ok = ensureConnected() && writeUnlocked(path, counting);
    if (!ok && isSocketError())
        closeUnlocked();
    report(QStringLiteral("PUT"), path, 0, total, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

bool RemoteStore::putAtomic(const QString &path, const QByteArray &data)
{
    QMutexLocker lock(&m_mutex);
    const QString temp = path + QStringLiteral(".tmp-") + QString::fromLatin1(CryptoService::randomBytes(4).toHex());
    if (!put(temp, data))
        return false;
    QElapsedTimer timer;
    timer.start();
    const bool ok = ensureConnected() && renameUnlocked(temp, path);
    if (!ok && m_sftp)
        removeUnlocked(temp);
    report(QStringLiteral("RENAME"), temp + QStringLiteral(" -> ") + path, 0, 0, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

bool RemoteStore::remove(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    const bool ok = ensureConnected() && removeUnlocked(path);
    report(QStringLiteral("RM"), path, 0, 0, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

bool RemoteStore::exists(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    bool ok = ensureConnected();
    if (ok) {
        const QByteArray full = encodedPath(path);
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        ok = libssh2_sftp_stat_ex(m_sftp, full.constData(), static_cast<unsigned int>(full.size()),
                                  LIBSSH2_SFTP_STAT, &attrs) == 0;
        m_notFound = !ok && libssh2_sftp_last_error(m_sftp) == LIBSSH2_FX_NO_SUCH_FILE;
        if (!ok)
            setError(m_notFound ? QString() : QStringLiteral("Không kiểm tra được %1: %2").arg(path, sessionError()));
    }
    report(QStringLiteral("STAT"), path, 0, 0, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

QList<StoreEntry> RemoteStore::list(const QString &dir)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    bool ok = ensureConnected();
    QList<StoreEntry> entries;
    if (ok)
        entries = listUnlocked(dir, &ok);
    report(QStringLiteral("LIST"), dir, 0, 0, timer.nsecsElapsed() / 1000, ok);
    return entries;
}

bool RemoteStore::removeDirectory(const QString &dir)
{
    QMutexLocker lock(&m_mutex);
    QElapsedTimer timer;
    timer.start();
    const bool ok = ensureConnected() && removeTreeUnlocked(dir);
    report(QStringLiteral("RMDIR"), dir, 0, 0, timer.nsecsElapsed() / 1000, ok);
    return ok;
}

QString RemoteStore::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastError;
}

bool RemoteStore::notFound() const
{
    QMutexLocker lock(&m_mutex);
    return m_notFound;
}
