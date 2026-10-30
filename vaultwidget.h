#ifndef VAULTWIDGET_H
#define VAULTWIDGET_H

#include "vaulttypes.h"

#include <QWidget>

namespace Ui {
class VaultWidget;
}

class QTableView;
class QueryController;
class ShareController;
class VaultController;
class VaultModel;

// View của tab "Vault": chỉ dựng giao diện, chuyển thao tác sang các controller
// và cập nhật lại khi controller phát signal.
class VaultWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VaultWidget(QWidget *parent = nullptr);
    ~VaultWidget() override;

    void setControllers(VaultController *vault, QueryController *query, ShareController *share);
    // Gọi từ menu chuột phải của cây file local
    void uploadFile(const QString &path);

private:
    void updateState();
    void openVault();
    void chooseAndUpload();
    void search();
    void handleFieldChanged();
    void showMineMenu(const QPoint &pos);
    void showSharedMenu(const QPoint &pos);
    void renameVideo(const VideoMeta &meta);
    void deleteVideo(const VideoMeta &meta);
    void shareVideo(const VideoMeta &meta);
    void revokeSelected();
    void showStats(const QueryStats &stats, int resultCount);
    void showOutgoing(const QList<ShareRecord> &records);

    Ui::VaultWidget *ui;
    VaultController *m_vault = nullptr;
    QueryController *m_query = nullptr;
    ShareController *m_share = nullptr;
    VaultModel *m_mine;
    VaultModel *m_shared;
    bool m_busy = false;
    bool m_showingProgress = false;
};

#endif // VAULTWIDGET_H
