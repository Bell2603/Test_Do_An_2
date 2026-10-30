#ifndef QUERYCONTROLLER_H
#define QUERYCONTROLLER_H

#include "vaulttypes.h"

#include <QObject>

class VaultController;

// Controller tìm kiếm trên chỉ mục mã hoá (Mục II: equality, range, nhiều điều kiện).
class QueryController : public QObject
{
    Q_OBJECT
public:
    explicit QueryController(VaultController *vault, QObject *parent = nullptr);

    void run(const VaultQuery &query);

signals:
    void finished(const QueryResult &result);

private:
    VaultController *m_vault;
};

#endif // QUERYCONTROLLER_H
