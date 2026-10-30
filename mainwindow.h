#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QModelIndex>
#include <QFileSystemModel>

#include "dialog.h"
#include "sftpcontroller.h"
#include "playercontroller.h"
#include "localfilecontroller.h"

class VaultController;
class QueryController;
class ShareController;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// View: chỉ setup UI, forward thao tác người dùng sang Controller,
// và cập nhật UI khi Controller phát signal. Không còn QProcess,
// không còn logic parse dữ liệu, không còn quyết định nghiệp vụ nào ở đây.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // Buttons - btnConnect nối thủ công (xem constructor), các on_..._clicked
    // còn lại được Qt tự auto-connect nhờ đúng tên theo Designer.
    void onBtnConnectClicked();
    void on_btnPlayStream_clicked();
    void on_pushButton_Play_Pause_clicked();
    void on_horizontalSlider_Duration_sliderMoved(int position);
    void on_pushButton_Next_clicked();
    void on_pushButton_Prev_clicked();

    // Tree view - remote
    void on_treeView_doubleClicked(const QModelIndex &index);
    void on_treeView_customContextMenuRequested(const QPoint &pos);

    // Tree view - local
    void on_treeView_Local_doubleClicked(const QModelIndex &index);
    void on_treeView_Local_clicked(const QModelIndex &index);
    void on_treeView_Local_customContextMenuRequested(const QPoint &pos);

    // Nhận signal từ SftpController
    void handleConnectionStateChanged(bool connected);
    void handleConnectionFailed(const QString &reason);
    void handleVideoReadyToPlay(const QString &localPath);
    void handleDownloadFinished(const QString &localPath);
    void handleDownloadFailed(const QString &fileName);

    // Nhận signal từ PlayerController
    void handlePlayerError(const QString &message);
    void handleDurationChangedSec(qint64 durationSec);
    void handlePositionChangedSec(qint64 positionSec);
    void handlePlayStateChanged(bool isPaused);

    // Nhận signal từ VaultController: video giải mã trong RAM đã sẵn sàng
    void handleEncryptedVideoReady(QIODevice *device, const QString &title);

    void on_horizontalSlider_Duration_sliderReleased();

private:
    void updateTimeLabels(qint64 currentSec);
    void promptRenameRemote(const QString &oldName);
    void promptRenameLocal(const QString &oldPath);
    void playNeighbourInLocalTree(int rowOffset);

    Ui::MainWindow *ui;

    SftpController *m_sftpController;
    PlayerController *m_playerController;
    LocalFileController *m_localFileController;
    VaultController *m_vaultController;
    QueryController *m_queryController;
    ShareController *m_shareController;

    QFileSystemModel *m_localModel;

    qint64 m_totalDurationSec = 0;
};
#endif // MAINWINDOW_H
