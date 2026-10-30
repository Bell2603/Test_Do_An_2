#ifndef KEYMANAGER_H
#define KEYMANAGER_H

#include <QByteArray>
#include <QString>

// Giữ các khoá của người dùng đang mở kho. Chỉ tồn tại trong RAM phía client.
//   masterKey = scrypt(passphrase, salt)     (Mục VI: salt lưu trên server)
//   indexKey / metaKey / kek / identityKey / verifyKey = HKDF(masterKey, nhãn)
// CEK của từng video được bọc bằng kek (Mục III.A: e_a(cek_i)).
class KeyManager
{
public:
    static constexpr quint64 ScryptN = 1u << 15;
    static constexpr quint32 ScryptR = 8;
    static constexpr quint32 ScryptP = 1;

    ~KeyManager();

    bool derive(const QString &passphrase, const QByteArray &salt);
    void setIdentity(const QByteArray &privateKey, const QByteArray &publicKey);
    void wipe();
    bool isReady() const { return !m_indexKey.isEmpty(); }

    const QByteArray &indexKey() const { return m_indexKey; }
    const QByteArray &metaKey() const { return m_metaKey; }
    const QByteArray &identityKey() const { return m_identityKey; }
    const QByteArray &verifyKey() const { return m_verifyKey; }
    const QByteArray &publicKey() const { return m_publicKey; }

    QByteArray wrapCek(const QByteArray &oid, const QByteArray &cek) const;
    bool unwrapCek(const QByteArray &oid, const QByteArray &wrapped, QByteArray *cek) const;

    bool openSealed(const QByteArray &sealed, const QByteArray &aad, QByteArray *plain) const;

private:
    QByteArray m_indexKey;
    QByteArray m_metaKey;
    QByteArray m_kek;
    QByteArray m_identityKey;
    QByteArray m_verifyKey;
    QByteArray m_privateKey;
    QByteArray m_publicKey;
};

#endif // KEYMANAGER_H
