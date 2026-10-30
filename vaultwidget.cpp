#include "vaultwidget.h"
#include "ui_vaultwidget.h"

#include "dialog.h"
#include "querycontroller.h"
#include "sharecontroller.h"
#include "sharedialog.h"
#include "vaultcontroller.h"
#include "vaultlogindialog.h"
#include "vaultmodel.h"

#include <QDateTime>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>

namespace {

void setupTable(QTableView *view, VaultModel *model)
{
    view->setModel(model);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->verticalHeader()->hide();
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    view->horizontalHeader()->setSectionResizeMode(VaultModel::Name, QHeaderView::Stretch);
}

} // namespace

VaultWidget::VaultWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::VaultWidget)
    , m_mine(new VaultModel(false, this))
    , m_shared(new VaultModel(true, this))
{
    ui->setupUi(this);

    ui->comboBox_Field->addItem(QStringLiteral("Tên chính xác (Fig. 2)"), int(QueryField::NameExact));
    ui->comboBox_Field->addItem(QStringLiteral("Tên bắt đầu bằng"), int(QueryField::NamePrefix));
    ui->comboBox_Field->addItem(QStringLiteral("Từ khoá trong tên"), int(QueryField::Word));
    ui->comboBox_Field->addItem(QStringLiteral("Ngày upload (khoảng)"), int(QueryField::UploadDate));
    ui->comboBox_Field->addItem(QStringLiteral("Thời lượng (khoảng, phút)"), int(QueryField::Duration));
    ui->comboBox_Field->addItem(QStringLiteral("Dung lượng (khoảng, MB)"), int(QueryField::Size));
    ui->comboBox_Strategy->addItem(QStringLiteral("Preorder – chỉ mục ghép"), int(QueryStrategy::Preorder));
    ui->comboBox_Strategy->addItem(QStringLiteral("Prefetch – contour + lọc"), int(QueryStrategy::Prefetch));
    ui->comboBox_Strategy->setToolTip(QStringLiteral(
        "Mục II.C của bài báo:\n"
        "• Preorder: chỉ mục ghép (từ khoá, thời lượng), không lộ số phần tử khớp.\n"
        "• Prefetch: ước lượng điều kiện nhỏ hơn bằng contour, tải trọn tập đó rồi lọc ở client."));

    const QDate today = QDate::currentDate();
    ui->dateEdit_From->setDate(today.addMonths(-1));
    ui->dateEdit_To->setDate(today);
    ui->doubleSpinBox_To->setValue(60);
    ui->spinBox_Limit->setToolTip(QStringLiteral("Có giới hạn: xuống lá đầu khoảng rồi đi sang phải (Fig. 3a).\n"
                                                 "Tất cả: tải theo từng tầng (Fig. 3b)."));
    ui->progressBar_Upload->hide();

    setupTable(ui->tableView_Mine, m_mine);
    setupTable(ui->tableView_Shared, m_shared);
    ui->treeWidget_Outgoing->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(ui->comboBox_Field, &QComboBox::currentIndexChanged, this, &VaultWidget::handleFieldChanged);
    connect(ui->checkBox_Duration, &QCheckBox::toggled, this, &VaultWidget::updateState);
    connect(ui->lineEdit_Text, &QLineEdit::returnPressed, this, &VaultWidget::search);
    connect(ui->pushButton_Search, &QPushButton::clicked, this, &VaultWidget::search);
    connect(ui->pushButton_Open, &QPushButton::clicked, this, &VaultWidget::openVault);
    connect(ui->pushButton_Upload, &QPushButton::clicked, this, &VaultWidget::chooseAndUpload);
    connect(ui->tableView_Mine, &QTableView::customContextMenuRequested, this, &VaultWidget::showMineMenu);
    connect(ui->tableView_Shared, &QTableView::customContextMenuRequested, this, &VaultWidget::showSharedMenu);
    connect(ui->pushButton_Revoke, &QPushButton::clicked, this, &VaultWidget::revokeSelected);

    handleFieldChanged();
    updateState();
}

VaultWidget::~VaultWidget()
{
    delete ui;
}

void VaultWidget::setControllers(VaultController *vault, QueryController *query, ShareController *share)
{
    m_vault = vault;
    m_query = query;
    m_share = share;

    connect(ui->pushButton_Lock, &QPushButton::clicked, m_vault, &VaultController::lock);
    connect(ui->pushButton_All, &QPushButton::clicked, m_vault, &VaultController::reloadLibrary);
    connect(ui->pushButton_RefreshInbox, &QPushButton::clicked, m_share, &ShareController::loadInbox);
    connect(ui->pushButton_RefreshOutgoing, &QPushButton::clicked, m_share, &ShareController::loadOutgoing);
    connect(ui->tableView_Mine, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        m_vault->play(m_mine->itemAt(index.row()));
    });
    connect(ui->tableView_Shared, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        m_vault->play(m_shared->itemAt(index.row()));
    });
    connect(ui->tabWidget_Lists, &QTabWidget::currentChanged, this, [this](int index) {
        if (!m_vault->isOpen())
            return;
        if (ui->tabWidget_Lists->widget(index) == ui->tab_Shared)
            m_share->loadInbox();
        else if (ui->tabWidget_Lists->widget(index) == ui->tab_Outgoing)
            m_share->loadOutgoing();
    });

    // ----- VaultController -> View -----
    connect(m_vault, &VaultController::busyChanged, this, [this](bool busy, const QString &message) {
        m_busy = busy;
        if (busy) {
            if (!m_showingProgress) {
                ui->progressBar_Upload->setRange(0, 0);
                ui->progressBar_Upload->setFormat(message);
            }
            ui->progressBar_Upload->show();
        } else {
            m_showingProgress = false;
            ui->progressBar_Upload->hide();
        }
        updateState();
    });
    connect(m_vault, &VaultController::uploadProgress, this, [this](qint64 done, qint64 total) {
        m_showingProgress = true;
        ui->progressBar_Upload->setRange(0, 1000);
        ui->progressBar_Upload->setValue(total > 0 ? int(done * 1000 / total) : 0);
        ui->progressBar_Upload->setFormat(QStringLiteral("Đang mã hoá và upload… %1 / %2")
                                              .arg(QLocale::system().formattedDataSize(done),
                                                   QLocale::system().formattedDataSize(total)));
        ui->progressBar_Upload->show();
    });
    connect(m_vault, &VaultController::failed, this, [this](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("Vault"), message);
    });
    connect(m_vault, &VaultController::hostKeyConfirmationNeeded, this, [this](const QString &host, const QString &fingerprint) {
        const auto reply = QMessageBox::question(
            this, QStringLiteral("Xác nhận server"),
            QStringLiteral("Lần đầu kết nối tới %1 qua libssh2.\nFingerprint host key:\n%2\n\n"
                           "Chỉ đồng ý nếu fingerprint khớp với server của bạn. Tin cậy server này?")
                .arg(host, fingerprint));
        m_vault->confirmHostKey(reply == QMessageBox::Yes);
    });
    connect(m_vault, &VaultController::userNotFound, this, [this](const QString &username) {
        const auto reply = QMessageBox::question(
            this, QStringLiteral("Tạo kho mới"),
            QStringLiteral("Người dùng '%1' chưa có kho trên server. Tạo kho mới?").arg(username));
        if (reply != QMessageBox::Yes) {
            m_vault->cancelPending();
            return;
        }
        bool ok = false;
        const QString confirm = QInputDialog::getText(this, QStringLiteral("Tạo kho mới"),
                                                      QStringLiteral("Nhập lại passphrase (tối thiểu 8 ký tự):"),
                                                      QLineEdit::Password, QString(), &ok);
        if (!ok) {
            m_vault->cancelPending();
            return;
        }
        m_vault->createPendingVault(confirm);
    });
    connect(m_vault, &VaultController::opened, this, [this](const QString &username, const QString &fingerprint) {
        ui->label_Status->setText(QStringLiteral("Đã mở kho của <b>%1</b><br><small>Khoá công khai: <tt>%2</tt></small>")
                                      .arg(username.toHtmlEscaped(), fingerprint));
        updateState();
    });
    connect(m_vault, &VaultController::locked, this, [this] {
        ui->label_Status->setText(QStringLiteral("Kho đang khoá"));
        m_mine->setItems({});
        m_shared->setItems({});
        ui->treeWidget_Outgoing->clear();
        ui->label_Stats->clear();
        updateState();
    });
    connect(m_vault, &VaultController::libraryLoaded, this, [this](const QList<VideoMeta> &items) {
        m_mine->setItems(items);
    });
    connect(m_vault, &VaultController::uploaded, this, [this](const VideoMeta &meta) {
        ui->label_Stats->setText(QStringLiteral("Đã mã hoá và upload %1 → server chỉ thấy obj/%2")
                                     .arg(meta.name, meta.oidHex()));
    });

    // ----- QueryController / ShareController -> View -----
    connect(m_query, &QueryController::finished, this, [this](const QueryResult &result) {
        if (!result.ok()) {
            ui->label_Stats->setText(QStringLiteral("<span style='color:red'>%1</span>").arg(result.error.toHtmlEscaped()));
            return;
        }
        m_mine->setItems(result.items);
        ui->tabWidget_Lists->setCurrentWidget(ui->tab_Mine);
        showStats(result.stats, int(result.items.size()));
    });
    connect(m_vault, &VaultController::statsUpdated, this, [this](const QueryStats &stats) {
        if (!stats.note.startsWith(QStringLiteral("Bulk-fetch")))
            return; // kết quả truy vấn đã được hiển thị ở QueryController::finished
        showStats(stats, m_mine->rowCount());
    });
    connect(m_share, &ShareController::inboxLoaded, this, [this](const QList<VideoMeta> &items) {
        m_shared->setItems(items);
    });
    connect(m_share, &ShareController::outgoingLoaded, this, &VaultWidget::showOutgoing);
    connect(m_share, &ShareController::shared, this, [this](const QString &videoName, const QString &recipient) {
        ui->label_Stats->setText(QStringLiteral("Đã chia sẻ %1 cho %2 (CEK được bọc bằng khoá X25519 của người nhận).")
                                     .arg(videoName, recipient));
    });
    connect(m_share, &ShareController::revoked, this, [this] {
        ui->label_Stats->setText(QStringLiteral("Đã thu hồi: mục inbox của người nhận đã bị xoá."));
    });
    connect(m_share, &ShareController::failed, this, [this](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("Chia sẻ"), message);
    });

    updateState();
}

void VaultWidget::updateState()
{
    const bool ready = m_vault != nullptr;
    const bool open = ready && m_vault->isOpen();
    ui->pushButton_Open->setEnabled(ready && !open && !m_busy);
    ui->pushButton_Lock->setEnabled(open && !m_busy);
    ui->pushButton_Upload->setEnabled(open);
    ui->groupBox_Search->setEnabled(open && !m_busy);
    ui->tabWidget_Lists->setEnabled(open);

    const auto field = QueryField(ui->comboBox_Field->currentData().toInt());
    const bool durationAllowed = field != QueryField::Duration;
    ui->checkBox_Duration->setEnabled(durationAllowed);
    const bool withDuration = durationAllowed && ui->checkBox_Duration->isChecked();
    ui->doubleSpinBox_DurationFrom->setEnabled(withDuration);
    ui->doubleSpinBox_DurationTo->setEnabled(withDuration);
    ui->comboBox_Strategy->setEnabled(withDuration);
}

void VaultWidget::handleFieldChanged()
{
    const auto field = QueryField(ui->comboBox_Field->currentData().toInt());
    switch (field) {
    case QueryField::NameExact:
    case QueryField::NamePrefix:
    case QueryField::Word:
        ui->stackedWidget_Value->setCurrentWidget(ui->page_Text);
        break;
    case QueryField::UploadDate:
        ui->stackedWidget_Value->setCurrentWidget(ui->page_Date);
        break;
    case QueryField::Duration:
        ui->stackedWidget_Value->setCurrentWidget(ui->page_Number);
        ui->label_Unit->setText(QStringLiteral("phút"));
        break;
    case QueryField::Size:
        ui->stackedWidget_Value->setCurrentWidget(ui->page_Number);
        ui->label_Unit->setText(QStringLiteral("MB"));
        break;
    }
    updateState();
}

void VaultWidget::openVault()
{
    if (!m_vault)
        return;
    VaultLoginDialog dialog(m_vault->serverDescription(), m_vault->needsSshPassword(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_vault->openVault(dialog.username(), dialog.passphrase(), dialog.sshPassword());
}

void VaultWidget::uploadFile(const QString &path)
{
    if (!m_vault || !m_vault->isOpen()) {
        QMessageBox::information(this, QStringLiteral("Vault"), QStringLiteral("Hãy mở kho (tab Vault) trước khi upload."));
        return;
    }
    m_vault->upload(path);
}

void VaultWidget::chooseAndUpload()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Chọn video để mã hoá và upload"), QString(),
        QStringLiteral("Video (*.mp4 *.webm *.mov *.mkv *.avi);;Tất cả (*)"));
    for (const QString &file : files)
        uploadFile(file);
}

void VaultWidget::search()
{
    if (!m_query || !m_vault || !m_vault->isOpen())
        return;

    VaultQuery query;
    query.field = QueryField(ui->comboBox_Field->currentData().toInt());
    query.limit = ui->spinBox_Limit->value();
    switch (query.field) {
    case QueryField::NameExact:
    case QueryField::NamePrefix:
    case QueryField::Word:
        query.text = ui->lineEdit_Text->text().trimmed();
        if (query.text.isEmpty()) {
            ui->label_Stats->setText(QStringLiteral("Hãy nhập tên hoặc từ khoá."));
            return;
        }
        break;
    case QueryField::UploadDate:
        query.from = ui->dateEdit_From->date().startOfDay().toMSecsSinceEpoch();
        query.to = ui->dateEdit_To->date().endOfDay().toMSecsSinceEpoch();
        break;
    case QueryField::Duration:
        query.from = qRound64(ui->doubleSpinBox_From->value() * 60000);
        query.to = qRound64(ui->doubleSpinBox_To->value() * 60000);
        break;
    case QueryField::Size:
        query.from = qRound64(ui->doubleSpinBox_From->value() * 1024 * 1024);
        query.to = qRound64(ui->doubleSpinBox_To->value() * 1024 * 1024);
        break;
    }
    query.withDuration = ui->checkBox_Duration->isEnabled() && ui->checkBox_Duration->isChecked();
    query.durationFrom = qRound64(ui->doubleSpinBox_DurationFrom->value() * 60000);
    query.durationTo = qRound64(ui->doubleSpinBox_DurationTo->value() * 60000);
    query.strategy = QueryStrategy(ui->comboBox_Strategy->currentData().toInt());
    m_query->run(query);
}

void VaultWidget::showStats(const QueryStats &stats, int resultCount)
{
    ui->label_Stats->setText(
        QStringLiteral("<b>%1 kết quả</b> · %2 round-trip · tải %3 object (%4) · %5 cache hit · cây cao %6 · %7 ms<br>%8")
            .arg(resultCount)
            .arg(stats.roundTrips)
            .arg(stats.objectsFetched)
            .arg(QLocale::system().formattedDataSize(stats.bytesIn))
            .arg(stats.cacheHits)
            .arg(stats.treeHeight)
            .arg(stats.elapsedMs)
            .arg(stats.note.toHtmlEscaped()));
}

void VaultWidget::showMineMenu(const QPoint &pos)
{
    const QModelIndex index = ui->tableView_Mine->indexAt(pos);
    if (!index.isValid())
        return;
    const VideoMeta meta = m_mine->itemAt(index.row());

    QMenu menu;
    QAction *playAction = menu.addAction(QStringLiteral("Phát (giải mã trong RAM)"));
    QAction *renameAction = menu.addAction(QStringLiteral("Đổi tên…"));
    QAction *shareAction = menu.addAction(QStringLiteral("Chia sẻ…"));
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(QStringLiteral("Xoá khỏi kho…"));
    QAction *action = menu.exec(ui->tableView_Mine->viewport()->mapToGlobal(pos));

    if (action == playAction)
        m_vault->play(meta);
    else if (action == renameAction)
        renameVideo(meta);
    else if (action == shareAction)
        shareVideo(meta);
    else if (action == deleteAction)
        deleteVideo(meta);
}

void VaultWidget::showSharedMenu(const QPoint &pos)
{
    const QModelIndex index = ui->tableView_Shared->indexAt(pos);
    if (!index.isValid())
        return;
    QMenu menu;
    QAction *playAction = menu.addAction(QStringLiteral("Phát (giải mã trong RAM)"));
    if (menu.exec(ui->tableView_Shared->viewport()->mapToGlobal(pos)) == playAction)
        m_vault->play(m_shared->itemAt(index.row()));
}

void VaultWidget::renameVideo(const VideoMeta &meta)
{
    Dialog dialog(this);
    dialog.setInitialName(meta.name);
    if (dialog.exec() != QDialog::Accepted || dialog.NewName_Test().isEmpty())
        return;
    // Chỉ đổi metadata + chỉ mục; tên object trên server vẫn là id ngẫu nhiên
    m_vault->rename(meta, dialog.NewName_Test());
}

void VaultWidget::deleteVideo(const VideoMeta &meta)
{
    const auto reply = QMessageBox::question(
        this, QStringLiteral("Xoá khỏi kho"),
        QStringLiteral("Xoá vĩnh viễn \"%1\" khỏi kho? Các lượt chia sẻ video này cũng bị thu hồi.").arg(meta.name));
    if (reply == QMessageBox::Yes)
        m_vault->remove(meta);
}

void VaultWidget::shareVideo(const VideoMeta &meta)
{
    ShareDialog dialog(meta.name, this);
    connect(&dialog, &ShareDialog::lookupRequested, m_share, &ShareController::lookup);
    const auto connection = connect(m_share, &ShareController::recipientLookedUp, &dialog, &ShareDialog::setLookupResult);
    const int result = dialog.exec();
    disconnect(connection);
    if (result == QDialog::Accepted)
        m_share->share(meta, dialog.recipient());
}

void VaultWidget::showOutgoing(const QList<ShareRecord> &records)
{
    ui->treeWidget_Outgoing->clear();
    for (const ShareRecord &record : records) {
        auto *item = new QTreeWidgetItem(ui->treeWidget_Outgoing);
        item->setText(0, record.videoName);
        item->setText(1, record.recipient);
        item->setText(2, QDateTime::fromMSecsSinceEpoch(record.sharedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        item->setData(0, Qt::UserRole, record.shareId);
    }
}

void VaultWidget::revokeSelected()
{
    QTreeWidgetItem *item = ui->treeWidget_Outgoing->currentItem();
    if (!item)
        return;
    const auto reply = QMessageBox::question(
        this, QStringLiteral("Thu hồi chia sẻ"),
        QStringLiteral("Thu hồi chia sẻ \"%1\" với %2?\nNếu người nhận đã mở video, họ vẫn giữ được khoá của video đó.")
            .arg(item->text(0), item->text(1)));
    if (reply == QMessageBox::Yes)
        m_share->revoke(item->data(0, Qt::UserRole).toString());
}
