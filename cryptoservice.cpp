#include "cryptoservice.h"

#include <QStringList>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <cstring>
#include <memory>

namespace {

struct CipherCtxDeleter { void operator()(EVP_CIPHER_CTX *ctx) const { EVP_CIPHER_CTX_free(ctx); } };
struct PkeyDeleter { void operator()(EVP_PKEY *key) const { EVP_PKEY_free(key); } };
struct PkeyCtxDeleter { void operator()(EVP_PKEY_CTX *ctx) const { EVP_PKEY_CTX_free(ctx); } };
struct KdfCtxDeleter { void operator()(EVP_KDF_CTX *ctx) const { EVP_KDF_CTX_free(ctx); } };

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using KdfCtx = std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter>;

const unsigned char *bytes(const QByteArray &data)
{
    return reinterpret_cast<const unsigned char *>(data.constData());
}

unsigned char *bytes(QByteArray &data)
{
    return reinterpret_cast<unsigned char *>(data.data());
}

const QByteArray kSealInfo("setp/seal/v1");

} // namespace

QByteArray CryptoService::randomBytes(int size)
{
    QByteArray out(size, Qt::Uninitialized);
    if (size > 0 && RAND_bytes(bytes(out), size) != 1)
        return {};
    return out;
}

QByteArray CryptoService::sha256(const QByteArray &data)
{
    QByteArray out(32, Qt::Uninitialized);
    unsigned int length = 0;
    if (EVP_Digest(data.constData(), size_t(data.size()), bytes(out), &length, EVP_sha256(), nullptr) != 1)
        return {};
    return out;
}

QByteArray CryptoService::hkdf(const QByteArray &ikm, const QByteArray &salt,
                               const QByteArray &info, int length)
{
    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (!kdf)
        return {};
    KdfCtx ctx(EVP_KDF_CTX_new(kdf));
    EVP_KDF_free(kdf);
    if (!ctx)
        return {};

    // OSSL_PARAM cần con trỏ không-const nên sao chép ra bản riêng
    QByteArray key = ikm;
    QByteArray saltCopy = salt;
    QByteArray infoCopy = info;
    char digest[] = "SHA256";

    OSSL_PARAM params[5];
    int n = 0;
    params[n++] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0);
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, key.data(), size_t(key.size()));
    if (!saltCopy.isEmpty())
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, saltCopy.data(), size_t(saltCopy.size()));
    if (!infoCopy.isEmpty())
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, infoCopy.data(), size_t(infoCopy.size()));
    params[n] = OSSL_PARAM_construct_end();

    QByteArray out(length, Qt::Uninitialized);
    const bool ok = EVP_KDF_derive(ctx.get(), bytes(out), size_t(length), params) == 1;
    wipe(key);
    if (!ok)
        return {};
    return out;
}

QByteArray CryptoService::scrypt(const QByteArray &passphrase, const QByteArray &salt,
                                 quint64 n, quint32 r, quint32 p, int length)
{
    // Giới hạn bộ nhớ đủ cho N=2^15, r=8 (32 MB) cộng phần phụ
    constexpr uint64_t maxMemory = 256ull * 1024 * 1024;
    QByteArray out(length, Qt::Uninitialized);
    if (EVP_PBE_scrypt(passphrase.constData(), size_t(passphrase.size()),
                       bytes(salt), size_t(salt.size()),
                       n, r, p, maxMemory, bytes(out), size_t(length)) != 1)
        return {};
    return out;
}

QByteArray CryptoService::encrypt(const QByteArray &key, const QByteArray &plain, const QByteArray &aad)
{
    if (key.size() != KeySize)
        return {};

    const QByteArray nonce = randomBytes(NonceSize);
    if (nonce.size() != NonceSize)
        return {};

    QByteArray out(NonceSize + plain.size() + TagSize, Qt::Uninitialized);
    std::memcpy(out.data(), nonce.constData(), NonceSize);
    unsigned char *ciphertext = bytes(out) + NonceSize;

    CipherCtx ctx(EVP_CIPHER_CTX_new());
    int length = 0;
    if (!ctx
        || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, NonceSize, nullptr) != 1
        || EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), bytes(nonce)) != 1)
        return {};

    if (!aad.isEmpty() && EVP_EncryptUpdate(ctx.get(), nullptr, &length, bytes(aad), int(aad.size())) != 1)
        return {};
    if (!plain.isEmpty()
        && EVP_EncryptUpdate(ctx.get(), ciphertext, &length, bytes(plain), int(plain.size())) != 1)
        return {};
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext + plain.size(), &length) != 1)
        return {};
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, TagSize, ciphertext + plain.size()) != 1)
        return {};
    return out;
}

bool CryptoService::decrypt(const QByteArray &key, const QByteArray &blob,
                            const QByteArray &aad, QByteArray *plain)
{
    if (key.size() != KeySize || blob.size() < GcmOverhead || !plain)
        return false;

    const int ciphertextLength = int(blob.size()) - GcmOverhead;
    const unsigned char *nonce = bytes(blob);
    const unsigned char *ciphertext = nonce + NonceSize;
    QByteArray tag = blob.right(TagSize);

    QByteArray out(ciphertextLength, Qt::Uninitialized);
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    int length = 0;
    if (!ctx
        || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, NonceSize, nullptr) != 1
        || EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, bytes(key), nonce) != 1)
        return false;

    if (!aad.isEmpty() && EVP_DecryptUpdate(ctx.get(), nullptr, &length, bytes(aad), int(aad.size())) != 1)
        return false;
    if (ciphertextLength > 0
        && EVP_DecryptUpdate(ctx.get(), bytes(out), &length, ciphertext, ciphertextLength) != 1) {
        wipe(out);
        return false;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, TagSize, tag.data()) != 1) {
        wipe(out);
        return false;
    }
    int finalLength = 0;
    unsigned char dummy[16];
    if (EVP_DecryptFinal_ex(ctx.get(), dummy, &finalLength) != 1) {
        // Sai khoá, sai AAD hoặc dữ liệu đã bị sửa
        wipe(out);
        return false;
    }
    *plain = out;
    return true;
}

bool CryptoService::generateX25519(QByteArray *privateKey, QByteArray *publicKey)
{
    Pkey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"));
    if (!key)
        return false;

    QByteArray priv(32, Qt::Uninitialized);
    QByteArray pub(32, Qt::Uninitialized);
    size_t privLength = size_t(priv.size());
    size_t pubLength = size_t(pub.size());
    if (EVP_PKEY_get_raw_private_key(key.get(), bytes(priv), &privLength) != 1
        || EVP_PKEY_get_raw_public_key(key.get(), bytes(pub), &pubLength) != 1)
        return false;

    *privateKey = priv;
    *publicKey = pub;
    return true;
}

QByteArray CryptoService::x25519(const QByteArray &privateKey, const QByteArray &peerPublicKey)
{
    if (privateKey.size() != 32 || peerPublicKey.size() != 32)
        return {};

    Pkey own(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, bytes(privateKey), 32));
    Pkey peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, bytes(peerPublicKey), 32));
    if (!own || !peer)
        return {};

    PkeyCtx ctx(EVP_PKEY_CTX_new(own.get(), nullptr));
    size_t length = 0;
    if (!ctx
        || EVP_PKEY_derive_init(ctx.get()) != 1
        || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) != 1
        || EVP_PKEY_derive(ctx.get(), nullptr, &length) != 1)
        return {};

    QByteArray shared(int(length), Qt::Uninitialized);
    if (EVP_PKEY_derive(ctx.get(), bytes(shared), &length) != 1)
        return {};

    // Từ chối điểm bậc thấp (kết quả toàn 0)
    if (shared.count('\0') == shared.size())
        return {};
    return shared;
}

QByteArray CryptoService::seal(const QByteArray &recipientPublicKey, const QByteArray &plain,
                               const QByteArray &aad)
{
    QByteArray ephemeralPrivate;
    QByteArray ephemeralPublic;
    if (!generateX25519(&ephemeralPrivate, &ephemeralPublic))
        return {};

    QByteArray shared = x25519(ephemeralPrivate, recipientPublicKey);
    wipe(ephemeralPrivate);
    if (shared.isEmpty())
        return {};

    QByteArray key = hkdf(shared, ephemeralPublic + recipientPublicKey, kSealInfo);
    wipe(shared);
    const QByteArray body = encrypt(key, plain, aad);
    wipe(key);
    if (body.isEmpty())
        return {};
    return ephemeralPublic + body;
}

bool CryptoService::open(const QByteArray &privateKey, const QByteArray &publicKey,
                         const QByteArray &sealed, const QByteArray &aad, QByteArray *plain)
{
    if (sealed.size() < 32 + GcmOverhead)
        return false;

    const QByteArray ephemeralPublic = sealed.left(32);
    QByteArray shared = x25519(privateKey, ephemeralPublic);
    if (shared.isEmpty())
        return false;

    QByteArray key = hkdf(shared, ephemeralPublic + publicKey, kSealInfo);
    wipe(shared);
    const bool ok = decrypt(key, sealed.mid(32), aad, plain);
    wipe(key);
    return ok;
}

QString CryptoService::fingerprint(const QByteArray &publicKey)
{
    const QString hex = QString::fromLatin1(sha256(publicKey).left(16).toHex()).toUpper();
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 4)
        groups << hex.mid(i, 4);
    return groups.join(QLatin1Char(' '));
}

void CryptoService::wipe(QByteArray &data)
{
    if (!data.isEmpty())
        OPENSSL_cleanse(data.data(), size_t(data.size()));
    data.clear();
}
