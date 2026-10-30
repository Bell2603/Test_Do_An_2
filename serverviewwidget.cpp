#include "serverviewwidget.h"
#include "ui_serverviewwidget.h"

#include "serverlogmodel.h"
#include "vaultcontroller.h"

#include <QHeaderView>
#include <QLocale>

#include <cmath>

namespace {
// Tham số theo bài báo: một bản ghi trong bucket nhánh ~29 byte, zlib nén ~3 lần
constexpr double kRecordSize = 29;
constexpr double kCompression = 3;
}

ServerViewWidget::ServerViewWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ServerViewWidget)
{
    ui->setupUi(this);
    ui->treeWidget_Listing->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->splitter_Server->setStretchFactor(0, 3);
    ui->splitter_Server->setStretchFactor(1, 2);
}

ServerViewWidget::~ServerViewWidget()
{
    delete ui;
}

void ServerViewWidget::setController(VaultController *vault)
{
    m_vault = vault;
    ServerLogModel *log = vault->serverLog();
    ui->tableView_Log->setModel(log);
    ui->tableView_Log->verticalHeader()->hide();
    ui->tableView_Log->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableView_Log->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView_Log->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->tableView_Log->horizontalHeader()->setSectionResizeMode(ServerLogModel::Path, QHeaderView::Stretch);

    connect(log, &QAbstractItemModel::rowsInserted, ui->tableView_Log, &QTableView::scrollToBottom);
    connect(log, &ServerLogModel::totalsChanged, this, &ServerViewWidget::updateTotals);
    connect(ui->pushButton_Clear, &QPushButton::clicked, log, &ServerLogModel::clear);
    connect(ui->pushButton_ListServer, &QPushButton::clicked, vault, &VaultController::listServer);

    connect(vault, &VaultController::statsUpdated, this, [this](const QueryStats &stats) {
        ui->label_LastQuery->setText(QStringLiteral("%1 round-trip · %2 object mới · %3 cache hit · %4 · %5 ms · cây cao %6")
                                         .arg(stats.roundTrips)
                                         .arg(stats.objectsFetched)
                                         .arg(stats.cacheHits)
                                         .arg(QLocale::system().formattedDataSize(stats.bytesIn))
                                         .arg(stats.elapsedMs)
                                         .arg(stats.treeHeight));
    });
    connect(vault, &VaultController::serverListed, this, [this](const QList<StoreEntry> &entries) {
        ui->treeWidget_Listing->clear();
        for (const StoreEntry &entry : entries) {
            auto *item = new QTreeWidgetItem(ui->treeWidget_Listing);
            item->setText(0, entry.name + (entry.isDir ? QStringLiteral("/") : QString()));
            item->setText(1, entry.isDir ? QString() : QLocale::system().formattedDataSize(entry.size));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
    });
    updateTotals();
}

void ServerViewWidget::updateTotals()
{
    if (!m_vault)
        return;
    const ServerLogModel *log = m_vault->serverLog();
    const QLocale locale = QLocale::system();
    ui->label_Totals->setText(QStringLiteral("%1 yêu cầu · ↓ %2 · ↑ %3")
                                  .arg(log->operationCount())
                                  .arg(locale.formattedDataSize(log->totalBytesIn()),
                                       locale.formattedDataSize(log->totalBytesOut())));

    const double rttMs = log->estimatedRttMs();
    const double bandwidth = log->estimatedBandwidth();
    ui->label_Rtt->setText(rttMs > 0 ? QStringLiteral("%1 ms").arg(rttMs, 0, 'f', 2) : QStringLiteral("—"));
    ui->label_Bandwidth->setText(bandwidth > 0 ? QStringLiteral("%1/s").arg(locale.formattedDataSize(qint64(bandwidth)))
                                               : QStringLiteral("— (phát một video để đo)"));
    if (rttMs > 0 && bandwidth > 0) {
        const double optimal = optimalBucketSize(kCompression, bandwidth, rttMs / 1000.0, kRecordSize);
        ui->label_Optimal->setText(QStringLiteral("≈ %1 trước nén (≈ %2 truyền đi sau zlib); đang dùng %3")
                                       .arg(locale.formattedDataSize(qint64(optimal)),
                                            locale.formattedDataSize(qint64(optimal / kCompression)),
                                            locale.formattedDataSize(m_vault->bucketSize())));
    } else {
        ui->label_Optimal->setText(QStringLiteral("— (bài báo: 1 MB/s, RTT 50 ms → 8–10 KB)"));
    }
}

double ServerViewWidget::lambertW(double x)
{
    // Nhánh chính W0 với x > 0, lặp Newton
    if (x <= 0)
        return 0;
    double w = x < 3 ? std::log1p(x) : std::log(x) - std::log(std::log(x));
    for (int i = 0; i < 64; ++i) {
        const double ew = std::exp(w);
        const double step = (w * ew - x) / (ew * (w + 1));
        w -= step;
        if (std::abs(step) < 1e-12 * (1 + std::abs(w)))
            break;
    }
    return w;
}

double ServerViewWidget::optimalBucketSize(double compression, double bandwidth, double rttSeconds, double recordSize)
{
    const double cbt = compression * bandwidth * rttSeconds;
    return cbt / lambertW(cbt / (M_E * recordSize));
}
