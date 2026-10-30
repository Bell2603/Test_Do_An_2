#include "keymanager.h"
#include "cryptoservice.h"

KeyManager::~KeyManager()
{
    wipe();
}

bool KeyManager::derive(const QString &passphrase, const QByteArray &salt)
{
    wipe();
    QByteArray pass = passphrase.toUtf8();
    QByteArray master = CryptoService::scrypt(pass, salt, ScryptN, ScryptR, ScryptP);
    CryptoService::wipe(pass);
    if (master.isEmpty())
        return false;

    m_indexKey = CryptoService::hkdf(master, salt, "setp/index");
    m_metaKey = CryptoService::hkdf(master, salt, "setp/meta");
    m_kek = CryptoService::hkdf(master, salt, "setp/kek");
    m_identityKey = CryptoService::hkdf(master, salt, "setp/identity");
    m_verifyKey = CryptoService::hkdf(master, salt, "setp/verify");
    CryptoService::wipe(master);

    if (m_indexKey.isEmpty() || m_metaKey.isEmpty() || m_kek.isEmpty()
        || m_identityKey.isEmpty() || m_verifyKey.isEmpty()) {
        wipe();
        return false;
    }
    return true;
}

void KeyManager::setIdentity(const QByteArray &privateKey, const QByteArray &publicKey)
{
    m_privateKey = privateKey;
    m_publicKey = publicKey;
}

void KeyManager::wipe()
{
    CryptoService::wipe(m_indexKey);
    CryptoService::wipe(m_metaKey);
    CryptoService::wipe(m_kek);
    CryptoService::wipe(m_identityKey);
    CryptoService::wipe(m_verifyKey);
    CryptoService::wipe(m_privateKey);
    m_publicKey.clear();
}

QByteArray KeyManager::wrapCek(const QByteArray &oid, const QByteArray &cek) const
{
    return CryptoService::encrypt(m_kek, cek, "cek:" + oid.toHex());
}

bool KeyManager::unwrapCek(const QByteArray &oid, const QByteArray &wrapped, QByteArray *cek) const
{
    return CryptoService::decrypt(m_kek, wrapped, "cek:" + oid.toHex(), cek);
}

bool KeyManager::openSealed(const QByteArray &sealed, const QByteArray &aad, QByteArray *plain) const
{
    return CryptoService::open(m_privateKey, m_publicKey, sealed, aad, plain);
}
