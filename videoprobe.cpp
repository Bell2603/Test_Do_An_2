#include "videoprobe.h"

#include <QMediaPlayer>
#include <QUrl>

VideoProbe::VideoProbe(QObject *parent)
    : QObject(parent)
    , m_player(new QMediaPlayer(this))
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(0); });

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (m_path.isEmpty())
            return;
        if (status == QMediaPlayer::LoadedMedia)
            finish(m_player->duration());
        else if (status == QMediaPlayer::InvalidMedia)
            finish(0);
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this] {
        if (!m_path.isEmpty())
            finish(0);
    });
}

void VideoProbe::probe(const QString &path)
{
    m_path = path;
    m_timeout.start();
    m_player->setSource(QUrl::fromLocalFile(path));
}

void VideoProbe::finish(qint64 durationMs)
{
    const QString path = m_path;
    m_path.clear();
    m_timeout.stop();
    m_player->setSource(QUrl());
    emit finished(path, qMax<qint64>(0, durationMs));
}
