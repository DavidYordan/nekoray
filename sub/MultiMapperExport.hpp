#pragma once

#include "db/Database.hpp"

namespace NekoGui_sub {
    QString BuildMultiMapperExport(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles);
    QString BuildMultiMapperExportJson(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles);
}
