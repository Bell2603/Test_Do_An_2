#ifndef CRYPTOSERVICE_H
#define CRYPTOSERVICE_H

#include <QByteArray>
#include <QString>

// Các primitive mật mã dùng chung (OpenSSL 3), không giữ trạng thái.
// Theo Mục VI của bài báo ZeroDB: AES-256-GCM cho dữ liệu, scrypt để dẫn xuất
// khoá từ passphrase. Thêm HKDF để tách khoá con và X25519 để chia sẻ.
// Mọi hàm trả QByteArray rỗng / false khi lỗi.
class CryptoService
{
public:
    static constexpr int KeySize = 32;
    static constexpr int NonceSize = 12;
    static constexpr int TagSize = 16;
    static constexpr int GcmOverhead = NonceSize + TagSize;

    static QByteArray randomBytes(int size);
    static QByteArray sha256(const QByteArray &data);
    static QByteArray hkdf(const QByteArray &ikm, const QByteArray &salt,
                           const QByteArray &info, int length = KeySize);
    static QByteArray scrypt(const QByteArray &passphrase, const QByteArray &salt,
                             quint64 n, quint32 r, quint32 p, int length = KeySize);

    // AES-256-GCM, nonce ngẫu nhiên. Định dạng: nonce(12) | ciphertext | tag(16)
    static QByteArray encrypt(const QByteArray &key, const QByteArray &plain, const QByteArray &aad);
    static bool decrypt(const QByteArray &key, const QByteArray &blob,
                        const QByteArray &aad, QByteArray *plain);

    // X25519
    static bool generateX25519(QByteArray *privateKey, QByteArray *publicKey);
    static QByteArray x25519(const QByteArray &privateKey, const QByteArray &peerPublicKey);

    // Niêm phong cho người nhận (ECIES): khoá tạm X25519 + HKDF + AES-GCM.
    // Định dạng: ephemeralPublic(32) | encrypt(...)
    static QByteArray seal(const QByteArray &recipientPublicKey, const QByteArray &plain,
                           const QByteArray &aad);
    static bool open(const QByteArray &privateKey, const QByteArray &publicKey,
                     const QByteArray &sealed, const QByteArray &aad, QByteArray *plain);

    // Dấu vân tay để người dùng đối chiếu khoá công khai (16 byte đầu của SHA-256)
    static QString fingerprint(const QByteArray &publicKey);

    // Ghi đè vùng nhớ chứa khoá trước khi giải phóng
    static void wipe(QByteArray &data);
};

#endif // CRYPTOSERVICE_H
