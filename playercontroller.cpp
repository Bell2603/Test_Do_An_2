#include "playercontroller.h"

#include <QFile>
#include <QIODevice>
#include <QTimer>

PlayerController::PlayerController(QVideoWidget *videoWidget, QObject *parent)
    : QObject(parent)
    , m_videoWidget(videoWidget)
{
    m_player = new QMediaPlayer(this);
    m_player->setVideoOutput(m_videoWidget);

    connect(m_player, &QMediaPlayer::durationChanged, this, &PlayerController::handleDurationChanged);
    connect(m_player, &QMediaPlayer::positionChanged, this, &PlayerController::handlePositionChanged);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, &PlayerController::handleMediaStatusChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this, &PlayerController::handleErrorOccurred);
}

PlayerController::~PlayerController()
{
    // Huỷ player trước (dừng luồng giải mã của backend) rồi mới huỷ device nó đang đọc
    delete m_player;
    m_player = nullptr;
    delete m_device;
}

void PlayerController::play(const QString &localPath)
{
    if (!QFile::exists(localPath)) {
        emit error("File not exist " + localPath);
        return;
    }

    if (!m_player->isAvailable()) {
        emit error("Video can't play");
        return;
    }

    releaseDevice();
    m_isStream = false;
    m_player->stop();
    m_player->setSource(QUrl::fromLocalFile(localPath));
    startPlayback();
}

void PlayerController::playUrl(const QUrl &url)
{
    // Kiểm tra URL trước khi đưa cho player (trước đây setSource rồi mới kiểm tra)
    if (url.isEmpty() || !url.isValid() || url.scheme().isEmpty()) {
        emit error("No stream source.\nPlease set a valid [Stream] rtsp=... link in config.ini");
        return;
    }
    if (!m_player->isAvailable()) {
        emit error("Video can't play");
        return;
    }

    releaseDevice();
    m_isStream = true;
    m_streamUrl = url;
    m_retryCount = 0;
    m_player->stop();
    m_player->setSource(url);
    startPlayback();
}

void PlayerController::playEncrypted(QIODevice *device, const QUrl &hint)
{
    if (!device)
        return;
    if (!m_player->isAvailable()) {
        delete device;
        emit error("Video can't play");
        return;
    }
    if (!device->isOpen())
        device->open(QIODevice::ReadOnly);

    releaseDevice();
    m_isStream = false;
    m_device = device;
    m_player->stop();
    m_player->setSourceDevice(device, hint);
    startPlayback();
}

void PlayerController::stopEncrypted()
{
    if (!m_device)
        return;
    releaseDevice();
    m_isPaused = true;
    emit playStateChanged(m_isPaused);
}

void PlayerController::releaseDevice()
{
    if (!m_device)
        return;
    QIODevice *old = m_device;
    m_device = nullptr;
    m_player->stop();
    m_player->setSourceDevice(nullptr);
    // Luồng demux của backend có thể vẫn đang đọc dở: không close() ngay,
    // huỷ device sau một nhịp an toàn (destructor tự xoá khoá khỏi RAM)
    QTimer::singleShot(3000, old, &QObject::deleteLater);
}

void PlayerController::startPlayback()
{
    m_videoWidget->show();
    m_player->play();

    // reset lai trang thai cua nut Play_Pause
    m_isPaused = false;
    emit playStateChanged(m_isPaused);
}

void PlayerController::togglePlayPause()
{
    if (m_isPaused) {
        m_isPaused = false;
        m_player->play();
    } else {
        m_isPaused = true;
        m_player->pause();
    }
    emit playStateChanged(m_isPaused);
}

void PlayerController::handleDurationChanged(qint64 duration)
{
    m_durationSec = duration / 1000; // Doi sang don vi giay
    emit durationChangedSec(m_durationSec);
}

void PlayerController::handlePositionChanged(qint64 position)
{
    emit positionChangedSec(position / 1000);
}

void PlayerController::handleMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    switch (status) {
    case QMediaPlayer::LoadedMedia:
    case QMediaPlayer::BufferedMedia:
        m_retryCount = 0; // stream đã lên hình -> reset số lần thử lại
        break;
    case QMediaPlayer::EndOfMedia:
        m_isPaused = true;
        emit playStateChanged(m_isPaused);
        break;
    default:
        break;
    }
}

void PlayerController::handleErrorOccurred(QMediaPlayer::Error errorCode, const QString &errorString)
{
    if (errorCode == QMediaPlayer::NoError)
        return;
    if (retryStream())
        return;
    m_isPaused = true;
    emit playStateChanged(m_isPaused);
    emit error(errorString.isEmpty() ? QStringLiteral("Video can't play") : errorString);
}

bool PlayerController::retryStream()
{
    // Stream RTSP hay rớt khi thiết bị mới bật: thử lại vài lần trước khi báo lỗi
    if (!m_isStream || m_retryCount >= MaxStreamRetries)
        return false;
    ++m_retryCount;
    QTimer::singleShot(2000, this, [this] {
        if (!m_isStream)
            return;
        m_player->setSource(m_streamUrl);
        m_player->play();
    });
    return true;
}

void PlayerController::seek(qint64 positionSec)
{
    if (m_isStream || !m_player->isSeekable()) {
        return;
    }
    m_player->setPosition(positionSec * 1000);
}
