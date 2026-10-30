#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "appconfig.h"

#include "vault/querycontroller.h"
#include "vault/sharecontroller.h"
#include "vault/vaultcontroller.h"

#include <QMessageBox>
#include <QMenu>
#include <QVBoxLayout>
#include <QTime>
#include <QLocale>
#include <QFileInfo>
#include <QDir>
#include <QInputDialog>
#include <QTimer>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("SETP Ver 2");

    // Nap config MOT LAN DUY NHAT o day (truoc day bi doc rai rac o 3 cho,
    // va duong dan bi hard-code toi mot thu muc du an cu)
    const QString configPath = AppConfig::locateConfigFile();
    AppConfig::instance().load(configPath);
    const auto &config = AppConfig::instance();
    if (!config.isLoaded()) {
        QTimer::singleShot(0, this, [this] {
            QMessageBox::warning(this, "Config",
                                 "Không tìm thấy config.ini.\nĐặt file cạnh Project2_ver2.pro "
                                 "hoặc chỉ đường dẫn bằng biến môi trường SETP_CONFIG.");
        });
    }

    // ----- Controllers -----
    m_sftpController = new SftpController(this);
    m_localFileController = new LocalFileController(this);

    ui->treeView->setModel(m_sftpController->fileModel());

    // treeview_Local - van dung QFileSystemModel truc tiep, ban than no da la
    // 1 Model dung chuan cua Qt roi nen khong can boc them lop nao khac
    m_localModel = new QFileSystemModel(this);
    m_localModel->setRootPath(config.localPath());
    m_localModel->setNameFilters(QStringList() << "*.mp4" << "*.webm" << "*.mov" << "*.mkv" << "*.avi");
    m_localModel->setNameFilterDisables(true);

    ui->treeView_Local->setModel(m_localModel);
    ui->treeView_Local->setRootIndex(m_localModel->index(config.localPath()));
    ui->treeView_Local->hideColumn(1);
    ui->treeView_Local->hideColumn(2);

    // Video
    QVideoWidget *videoWidget = new QVideoWidget(ui->groupBoxVideo);
    videoWidget->hide();
    QVBoxLayout *videoLayout = new QVBoxLayout(ui->groupBoxVideo);
    videoLayout->setContentsMargins(5, 5, 5, 5);
    videoLayout->addWidget(videoWidget);

    m_playerController = new PlayerController(videoWidget, this);

    // Kho video ma hoa dau-cuoi (ZeroDB)
    m_vaultController = new VaultController(this);
    m_queryController = new QueryController(m_vaultController, this);
    m_shareController = new ShareController(m_vaultController, this);
    ui->vaultWidget->setControllers(m_vaultController, m_queryController, m_shareController);
    ui->serverViewWidget->setController(m_vaultController);

    // Thiet lap giao dien
    ui->pushButton_Play_Pause->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    ui->pushButton_Prev->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    ui->pushButton_Next->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    ui->btnConnect->setStyleSheet("color: white; background-color: green");
    ui->splitter->setSizes({460, 720});

    ui->treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->treeView_Local->setContextMenuPolicy(Qt::CustomContextMenu);

    // ----- Ket noi Controller <-> View -----
    connect(ui->btnConnect, &QPushButton::clicked, this, &MainWindow::onBtnConnectClicked);

    connect(m_sftpController, &SftpController::connectionStateChanged, this, &MainWindow::handleConnectionStateChanged);
    connect(m_sftpController, &SftpController::connectionFailed, this, &MainWindow::handleConnectionFailed);
    connect(m_sftpController, &SftpController::videoReadyToPlay, this, &MainWindow::handleVideoReadyToPlay);
    connect(m_sftpController, &SftpController::downloadFinished, this, &MainWindow::handleDownloadFinished);
    connect(m_sftpController, &SftpController::downloadFailed, this, &MainWindow::handleDownloadFailed);

    connect(m_playerController, &PlayerController::error, this, &MainWindow::handlePlayerError);
    connect(m_playerController, &PlayerController::durationChangedSec, this, &MainWindow::handleDurationChangedSec);
    connect(m_playerController, &PlayerController::positionChangedSec, this, &MainWindow::handlePositionChangedSec);
    connect(m_playerController, &PlayerController::playStateChanged, this, &MainWindow::handlePlayStateChanged);

    connect(m_vaultController, &VaultController::videoReady, this, &MainWindow::handleEncryptedVideoReady);
    connect(m_vaultController, &VaultController::aboutToLock, m_playerController, &PlayerController::stopEncrypted);
    connect(m_vaultController, &VaultController::busyChanged, this, [this](bool busy, const QString &message) {
        if (busy)
            statusBar()->showMessage(message);
        else
            statusBar()->clearMessage();
    });
}

MainWindow::~MainWindow()
{
    // Delete tmp (AppConfig dam bao tmpDir khong bao gio la thu muc home hay "/")
    const QString tmp = AppConfig::instance().tmpDir();
    if (!tmp.isEmpty() && QDir::cleanPath(tmp) != QDir::cleanPath(QDir::homePath()) && tmp != "/") {
        QDir tmpDir(tmp);
        tmpDir.removeRecursively();
    }
    delete ui;
}

// ---------------- Buttons ----------------

void MainWindow::onBtnConnectClicked()
{
    auto &config = AppConfig::instance();
    if (!m_sftpController->isRunning() && config.password().isEmpty() && config.keyPath().isEmpty()) {
        // config.ini khong con luu mat khau: hoi luc ket noi, chi giu trong RAM
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, "SSH", QString("Mật khẩu SSH cho %1@%2:").arg(config.user(), config.host()),
            QLineEdit::Password, QString(), &ok);
        if (!ok || password.isEmpty())
            return;
        config.setSessionPassword(password);
    }
    m_sftpController->toggleConnection();
}

void MainWindow::on_btnPlayStream_clicked()
{
    QUrl rtsp(AppConfig::instance().rtspLink());
    m_playerController->playUrl(rtsp);
}

void MainWindow::on_pushButton_Play_Pause_clicked()
{
    m_playerController->togglePlayPause();
}

void MainWindow::on_horizontalSlider_Duration_sliderMoved(int position)
{
    updateTimeLabels(position);
}

void MainWindow::on_pushButton_Next_clicked()
{
    playNeighbourInLocalTree(1);
}

void MainWindow::on_pushButton_Prev_clicked()
{
    playNeighbourInLocalTree(-1);
}

// ---------------- Tree view remote ----------------

void MainWindow::on_treeView_doubleClicked(const QModelIndex &index)
{
    const QString name = index.data().toString().trimmed();
    const QString kind = index.data(Qt::UserRole).toString();
    SftpController::EntryKind entryKind = SftpController::EntryKind::File;
    if (kind == "up")
        entryKind = SftpController::EntryKind::Parent;
    else if (kind == "dir")
        entryKind = SftpController::EntryKind::Directory;
    m_sftpController->openEntry(name, entryKind);
}

void MainWindow::on_treeView_customContextMenuRequested(const QPoint &pos)
{
    QModelIndex index = ui->treeView->indexAt(pos);
    QString fileName = index.data().toString().trimmed();
    const bool isFile = index.isValid() && index.data(Qt::UserRole).toString() == "file";

    QMenu menu;
    // Chi hien Rename/Download khi bam dung vao mot file (truoc day ca vung trong)
    QAction *renameAction = isFile ? menu.addAction("Rename") : nullptr;
    QAction *downloadAction = isFile ? menu.addAction("Download") : nullptr;
    QAction *refreshAction = menu.addAction("Refresh");
    refreshAction->setEnabled(m_sftpController->isConnected());
    QAction *action = menu.exec(ui->treeView->viewport()->mapToGlobal(pos));

    if (!action) return;

    if (action == renameAction) {
        promptRenameRemote(fileName);
    } else if (action == downloadAction) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "Download window",
            "Do you really want to download this file to your local machine??",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            m_sftpController->downloadToLocal(fileName);
        }
    } else if (action == refreshAction) {
        m_sftpController->refresh();
    }
}

// ---------------- Tree view local ----------------

void MainWindow::on_treeView_Local_clicked(const QModelIndex &index)
{
    if (m_localModel->isDir(index)) {
        ui->label_Video_Info->setText("This is Dir");
        return;
    }
    QString filePath = m_localModel->filePath(index);
    QFileInfo info(filePath);
    QString sizeText = QLocale::system().formattedDataSize(info.size());
    ui->label_Video_Info->setText(QString("%1  [%2]").arg(info.fileName()).arg(sizeText));
}

void MainWindow::on_treeView_Local_doubleClicked(const QModelIndex &index)
{
    if (m_localModel->isDir(index)) return;
    QString name = index.data().toString().trimmed();
    QString localPath = m_localModel->filePath(index);
    setWindowTitle(name);
    m_playerController->play(localPath);
}

void MainWindow::on_treeView_Local_customContextMenuRequested(const QPoint &pos)
{
    QModelIndex index = ui->treeView_Local->indexAt(pos);
    if (!index.isValid() || m_localModel->isDir(index)) return;
    // Duong dan day du tu model (dung ca voi file trong thu muc con)
    const QString filePath = m_localModel->filePath(index);

    QMenu menu;
    QAction *renameAction = menu.addAction("Rename");
    QAction *deleteAction = menu.addAction("Delete");
    menu.addSeparator();
    QAction *uploadAction = menu.addAction("Upload vào Vault (mã hoá)");
    QAction *action = menu.exec(ui->treeView_Local->viewport()->mapToGlobal(pos));

    if (!action) return;

    if (action == renameAction) {
        promptRenameLocal(filePath);
    } else if (action == deleteAction) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "Delete window", "Do you really want to Delete this file??",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QString error;
            if (!m_localFileController->deleteFile(filePath, &error))
                QMessageBox::warning(this, "Error", error);
        }
    } else if (action == uploadAction) {
        ui->tabWidget_Left->setCurrentWidget(ui->tab_Vault);
        ui->vaultWidget->uploadFile(filePath);
    }
}

// ---------------- Nhận signal từ Controller ----------------

void MainWindow::handleConnectionStateChanged(bool connected)
{
    ui->btnConnect->setText(connected ? "Disconnect" : "Connect");
}

void MainWindow::handleConnectionFailed(const QString &reason)
{
    QMessageBox::warning(this, "Error", reason);
}

void MainWindow::handleVideoReadyToPlay(const QString &localPath)
{
    m_playerController->play(localPath);
}

void MainWindow::handleDownloadFinished(const QString &localPath)
{
    statusBar()->showMessage("Downloaded: " + localPath, 5000);
}

void MainWindow::handleDownloadFailed(const QString &fileName)
{
    QMessageBox::warning(this, "Error", "Download failed: " + fileName);
}

void MainWindow::handleEncryptedVideoReady(QIODevice *device, const QString &title)
{
    setWindowTitle(title + " (vault)");
    m_playerController->playEncrypted(device, QUrl(title));
}

void MainWindow::handlePlayerError(const QString &message)
{
    QMessageBox::warning(this, "Error", message);
}

void MainWindow::handleDurationChangedSec(qint64 durationSec)
{
    m_totalDurationSec = durationSec;
    ui->horizontalSlider_Duration->setMaximum(durationSec);
}

void MainWindow::handlePositionChangedSec(qint64 positionSec)
{
    if (!ui->horizontalSlider_Duration->isSliderDown()) {
        ui->horizontalSlider_Duration->setValue(positionSec);
    }
    updateTimeLabels(positionSec);
}

void MainWindow::handlePlayStateChanged(bool isPaused)
{
    ui->pushButton_Play_Pause->setIcon(
        style()->standardIcon(isPaused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
}

// ---------------- Helpers ----------------

void MainWindow::updateTimeLabels(qint64 currentSec)
{
    if (currentSec == 0 && m_totalDurationSec == 0) return;

    // Truoc day gio duoc tinh bang (sec / 3600) % 60
    QTime currentTime = QTime(0, 0).addSecs(int(currentSec));
    QTime totalTime = QTime(0, 0).addSecs(int(m_totalDurationSec));

    QString format = (m_totalDurationSec >= 3600) ? "hh:mm:ss" : "mm:ss";
    ui->label_current_Time->setText(currentTime.toString(format));
    ui->label_Total_Time->setText(totalTime.toString(format));
}

void MainWindow::promptRenameRemote(const QString &oldName)
{
    Dialog dialog(this);
    dialog.setInitialName(oldName);
    if (dialog.exec() != QDialog::Accepted) return; // chỉ gọi exec() 1 lần duy nhất

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Rename window", "Do you really want to change name file ??",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    QString newName = dialog.NewName_Test();
    m_sftpController->renameRemote(oldName, newName);
}

void MainWindow::promptRenameLocal(const QString &oldPath)
{
    Dialog dialog(this);
    dialog.setInitialName(QFileInfo(oldPath).fileName());
    if (dialog.exec() != QDialog::Accepted) return; // chỉ gọi exec() 1 lần duy nhất

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Rename window", "Do you really want to change name file ??",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    QString newName = dialog.NewName_Test();
    QString error;
    if (!m_localFileController->renameFile(oldPath, newName, &error))
        QMessageBox::warning(this, "Error", error);
}

void MainWindow::playNeighbourInLocalTree(int rowOffset)
{
    QModelIndex current = ui->treeView_Local->currentIndex();
    if (!current.isValid()) return;

    QModelIndex neighbour = current.siblingAtRow(current.row() + rowOffset);
    if (!neighbour.isValid()) return;

    ui->treeView_Local->setCurrentIndex(neighbour);
    if (m_localModel->isDir(neighbour)) return;
    QString name = neighbour.data().toString().trimmed();
    QString localPath = m_localModel->filePath(neighbour);
    setWindowTitle(name);
    m_playerController->play(localPath);
}

void MainWindow::on_horizontalSlider_Duration_sliderReleased()
{
    m_playerController->seek(ui->horizontalSlider_Duration->sliderPosition());

}
