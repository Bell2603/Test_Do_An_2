#ifndef SERVERVIEWWIDGET_H
#define SERVERVIEWWIDGET_H

#include "vaulttypes.h"

#include <QWidget>

namespace Ui {
class ServerViewWidget;
}

class VaultController;

// Tab "Góc nhìn server": minh hoạ phần phân tích bảo mật (Mục IV) và hiệu năng (Mục V).
class ServerViewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ServerViewWidget(QWidget *parent = nullptr);
    ~ServerViewWidget() override;

    void setController(VaultController *vault);

    // Kích thước bucket tối ưu s_b = c·b·τ / W(c·b·τ / (e·s_r))  (Mục V.A)
    static double optimalBucketSize(double compression, double bandwidth, double rttSeconds, double recordSize);
    static double lambertW(double x);

private:
    void updateTotals();

    Ui::ServerViewWidget *ui;
    VaultController *m_vault = nullptr;
};

#endif // SERVERVIEWWIDGET_H
