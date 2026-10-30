#ifndef PLAYERCONTROLLER_H
#define PLAYERCONTROLLER_H

#include <QObject>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QUrl>

class QIODevice;

// Controller: quản lý QMediaPlayer + QVideoWidget.
// MainWindow không còn tự giữ biến IS_Pause, mDuration nữa - toàn bộ
// trạng thái play/pause/duration nằm ở đây, MainWindow chỉ lắng nghe signal.
class PlayerController : public QObject
{
    Q_OBJECT
public:
    explicit PlayerController(QVideoWidget *videoWidget, QObject *parent = nullptr);
    ~PlayerController() override;

    // Phát file local (kiểm tra file tồn tại trước khi phát)
    void play(const QString &localPath);

    // Phát trực tiếp URL (dùng cho RTSP stream, không kiểm tra QFile::exists)
    void playUrl(const QUrl &url);

    // Phát video giải mã trong RAM từ kho mã hoá. PlayerController nhận quyền sở hữu device.
    // hint: tên file để backend nhận dạng định dạng (vd "clip.mp4").
    void playEncrypted(QIODevice *device, const QUrl &hint);
    // Dừng nếu đang phát từ kho mã hoá (gọi trước khi khoá kho)
    void stopEncrypted();

    void togglePlayPause();

    bool isPaused() const { return m_isPaused; }
    qint64 totalDurationSec() const { return m_durationSec; }

    void seek(qint64 positionSec);

signals:
    void error(const QString &message);
    void durationChangedSec(qint64 durationSec);
    void positionChangedSec(qint64 positionSec);
    void playStateChanged(bool isPaused);

private slots:
    void handleDurationChanged(qint64 duration);
    void handlePositionChanged(qint64 position);
    void handleMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void handleErrorOccurred(QMediaPlayer::Error error, const QString &errorString);

private:
    void startPlayback();
    bool retryStream();
    void releaseDevice();

    static constexpr int MaxStreamRetries = 3;
    bool m_isStream = false;
    int m_retryCount = 0;
    QUrl m_streamUrl;

    QMediaPlayer *m_player;
    QVideoWidget *m_videoWidget;
    QIODevice *m_device = nullptr;
    bool m_isPaused = true;
    qint64 m_durationSec = 0;
};

#endif // PLAYERCONTROLLER_H
