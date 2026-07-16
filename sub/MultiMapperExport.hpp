#pragma once

#include "db/Database.hpp"

namespace NekoGui_sub {
    QString BuildMultiMapperExportJson(const QList<std::shared_ptr<NekoGui::ProxyEntity>> &profiles);
}
