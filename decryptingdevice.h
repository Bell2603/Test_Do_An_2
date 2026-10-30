#ifndef DECRYPTINGDEVICE_H
#define DECRYPTINGDEVICE_H

#include "objectstore.h"

#include <QIODevice>
#include <QList>
#include <QMutex>
#include <QPair>

#include <memory>

// Định dạng object video obj/<oid> trên server:
//   header 24 byte (rõ): "SETPV1\0\0" | chunkSize u32 | plainSize u64 | chunkCount u32
//   chunk i: nonce(12) | AES-256-GCM(cek, chunk_i) | tag(16)
//   AAD của chunk i = header | oid | i | isLast  -> chống tráo, đổi thứ tự, cắt bớt chunk
struct VideoHeader
{
    static constexpr int Size = 24;

    quint32 chunkSize = 0;
    quint64 plainSize = 0;
    quint32 chunkCount = 0;

    static VideoHeader forSize(quint64 plainSize, quint32 chunkSize);
    QByteArray serialize() const;
    static bool parse(const QByteArray &bytes, VideoHeader *header);
    static QByteArray chunkAad(const QByteArray &headerBytes, const QByteArray &oid, quint32 index, bool last);

    qint64 chunkOffset(quint32 index) const;
    qint64 chunkPlainLength(quint32 index) const;
};

// QIODevice đọc video mã hoá trực tiếp từ server theo từng chunk và giải mã trong RAM.
// QMediaPlayer::setSourceDevice() dùng thiết bị này nên plaintext không bao giờ
// được ghi xuống đĩa (thay cho cách cũ: tải file về thư mục tmp rồi mới phát).
// seek() đổi vị trí byte thành chỉ số chunk -> tua video không cần giải mã cả file.
class DecryptingDevice : public QIODevice
{
    Q_OBJECT
public:
    DecryptingDevice(std::shared_ptr<IObjectStore> store, const QString &objectPath,
                     const QByteArray &cek, const QByteArray &oid, const QByteArray &headerBytes,
                     QObject *parent = nullptr);
    ~DecryptingDevice() override;

    bool open(OpenMode mode) override;
    bool isSequential() const override { return false; }
    qint64 size() const override { return qint64(m_header.plainSize); }
    bool seek(qint64 pos) override;
    bool atEnd() const override;

    int chunksFetched() const;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    bool loadChunk(quint32 index, QByteArray *plain);

    std::shared_ptr<IObjectStore> m_store;
    QString m_path;
    QByteArray m_cek;
    QByteArray m_oid;
    QByteArray m_headerBytes;
    VideoHeader m_header;
    mutable QMutex m_mutex;
    QList<QPair<quint32, QByteArray>> m_chunks; // LRU nhỏ các chunk đã giải mã
    int m_chunksFetched = 0;
};

#endif // DECRYPTINGDEVICE_H
