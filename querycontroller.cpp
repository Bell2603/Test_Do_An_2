#include "querycontroller.h"
#include "vaultcontroller.h"

QueryController::QueryController(VaultController *vault, QObject *parent)
    : QObject(parent)
    , m_vault(vault)
{
}

void QueryController::run(const VaultQuery &query)
{
    auto service = m_vault->service();
    if (!service) {
        QueryResult result;
        result.error = QStringLiteral("Hãy mở kho trước khi tìm kiếm");
        emit finished(result);
        return;
    }
    m_vault->runTask(
        QStringLiteral("Đang duyệt chỉ mục mã hoá…"),
        [service, query]() { return service->query(query); },
        [this](const QueryResult &result) {
            m_vault->reportStats(result.stats);
            emit finished(result);
        });
}
