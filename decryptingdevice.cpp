#include "decryptingdevice.h"
#include "cryptoservice.h"

#include <QMutexLocker>
#include <QtEndian>

#include <cstring>

namespace {
const QByteArray kMagic("SETPV1\0\0", 8);
constexpr int kCachedChunks = 4;
}

// ---------------- VideoHeader ----------------

VideoHeader VideoHeader::forSize(quint64 plainSize, quint32 chunkSize)
{
    VideoHeader header;
    header.chunkSize = chunkSize;
    header.plainSize = plainSize;
    header.chunkCount = chunkSize == 0 ? 0 : quint32((plainSize + chunkSize - 1) / chunkSize);
    return header;
}

QByteArray VideoHeader::serialize() const
{
    QByteArray out(Size, Qt::Uninitialized);
    std::memcpy(out.data(), kMagic.constData(), 8);
    qToBigEndian(chunkSize, out.data() + 8);
    qToBigEndian(plainSize, out.data() + 12);
    qToBigEndian(chunkCount, out.data() + 20);
    return out;
}

bool VideoHeader::parse(const QByteArray &bytes, VideoHeader *header)
{
    if (bytes.size() < Size || !bytes.startsWith(kMagic))
        return false;
    header->chunkSize = qFromBigEndian<quint32>(bytes.constData() + 8);
    header->plainSize = qFromBigEndian<quint64>(bytes.constData() + 12);
    header->chunkCount = qFromBigEndian<quint32>(bytes.constData() + 20);
    const VideoHeader expected = forSize(header->plainSize, header->chunkSize);
    return header->chunkSize > 0 && header->chunkCount == expected.chunkCount;
}

QByteArray VideoHeader::chunkAad(const QByteArray &headerBytes, const QByteArray &oid, quint32 index, bool last)
{
    QByteArray aad = headerBytes.left(Size) + oid;
    char tail[5];
    qToBigEndian(index, tail);
    tail[4] = last ? 1 : 0;
    aad.append(tail, 5);
    return aad;
}

qint64 VideoHeader::chunkOffset(quint32 index) const
{
    return Size + qint64(index) * (qint64(chunkSize) + CryptoService::GcmOverhead);
}

qint64 VideoHeader::chunkPlainLength(quint32 index) const
{
    if (index + 1 < chunkCount)
        return chunkSize;
    return qint64(plainSize) - qint64(index) * chunkSize;
}

// ---------------- DecryptingDevice ----------------

DecryptingDevice::DecryptingDevice(std::shared_ptr<IObjectStore> store, const QString &objectPath,
                                   const QByteArray &cek, const QByteArray &oid, const QByteArray &headerBytes,
                                   QObject *parent)
    : QIODevice(parent)
    , m_store(std::move(store))
    , m_path(objectPath)
    , m_cek(cek)
    , m_oid(oid)
    , m_headerBytes(headerBytes.left(VideoHeader::Size))
{
    VideoHeader::parse(m_headerBytes, &m_header);
}

DecryptingDevice::~DecryptingDevice()
{
    QMutexLocker lock(&m_mutex);
    CryptoService::wipe(m_cek);
    for (auto &chunk : m_chunks)
        CryptoService::wipe(chunk.second);
}

bool DecryptingDevice::open(OpenMode mode)
{
    if (mode & WriteOnly)
        return false;
    // Unbuffered: tránh QIODevice giữ thêm một bản plaintext trong bộ đệm riêng
    return QIODevice::open(ReadOnly | Unbuffered);
}

bool DecryptingDevice::seek(qint64 pos)
{
    if (pos < 0 || pos > size())
        return false;
    return QIODevice::seek(pos);
}

bool DecryptingDevice::atEnd() const
{
    return pos() >= size();
}

int DecryptingDevice::chunksFetched() const
{
    QMutexLocker lock(&m_mutex);
    return m_chunksFetched;
}

bool DecryptingDevice::loadChunk(quint32 index, QByteArray *plain)
{
    for (int i = 0; i < m_chunks.size(); ++i) {
        if (m_chunks.at(i).first == index) {
            *plain = m_chunks.at(i).second;
            if (i != m_chunks.size() - 1)
                m_chunks.move(i, m_chunks.size() - 1);
            return true;
        }
    }

    const qint64 plainLength = m_header.chunkPlainLength(index);
    const qint64 cipherLength = plainLength + CryptoService::GcmOverhead;
    QByteArray blob;
    if (!m_store->getRange(m_path, m_header.chunkOffset(index), cipherLength, &blob)
        || blob.size() != cipherLength) {
        setErrorString(QStringLiteral("Không đọc được chunk %1: %2").arg(index).arg(m_store->lastError()));
        return false;
    }
    const bool last = index + 1 == m_header.chunkCount;
    QByteArray decrypted;
    if (!CryptoService::decrypt(m_cek, blob, VideoHeader::chunkAad(m_headerBytes, m_oid, index, last), &decrypted)
        || decrypted.size() != plainLength) {
        setErrorString(QStringLiteral("Chunk %1 không hợp lệ: sai khoá hoặc đã bị sửa/tráo trên server").arg(index));
        return false;
    }

    ++m_chunksFetched;
    m_chunks.append({index, decrypted});
    while (m_chunks.size() > kCachedChunks) {
        CryptoService::wipe(m_chunks.first().second);
        m_chunks.removeFirst();
    }
    *plain = decrypted;
    return true;
}

qint64 DecryptingDevice::readData(char *data, qint64 maxSize)
{
    QMutexLocker lock(&m_mutex);
    if (m_header.chunkSize == 0 || m_cek.isEmpty()) {
        setErrorString(QStringLiteral("Header video không hợp lệ"));
        return -1;
    }

    const qint64 start = pos();
    qint64 copied = 0;
    while (copied < maxSize && start + copied < size()) {
        const qint64 position = start + copied;
        const quint32 index = quint32(position / m_header.chunkSize);
        const qint64 offset = position % m_header.chunkSize;
        QByteArray plain;
        if (!loadChunk(index, &plain))
            return copied > 0 ? copied : -1;
        const qint64 n = qMin(maxSize - copied, qint64(plain.size()) - offset);
        if (n <= 0)
            break;
        std::memcpy(data + copied, plain.constData() + offset, size_t(n));
        copied += n;
    }
    return copied;
}
