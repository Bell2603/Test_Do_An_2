#ifndef VIDEOPROBE_H
#define VIDEOPROBE_H

#include <QObject>
#include <QTimer>

class QMediaPlayer;

// Đọc thời lượng video trước khi upload (để đưa vào chỉ mục thời lượng).
// Chạy bất đồng bộ trên luồng UI vì QMediaPlayer cần event loop.
class VideoProbe : public QObject
{
    Q_OBJECT
public:
    explicit VideoProbe(QObject *parent = nullptr);

    void probe(const QString &path);

signals:
    // durationMs = 0 nếu không đọc được (vẫn cho upload)
    void finished(const QString &path, qint64 durationMs);

private:
    void finish(qint64 durationMs);

    QMediaPlayer *m_player;
    QTimer m_timeout;
    QString m_path;
};

#endif // VIDEOPROBE_H
