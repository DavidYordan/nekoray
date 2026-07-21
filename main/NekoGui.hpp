#pragma once

#include <atomic>

#include "Const.hpp"
#include "NekoGui_Utils.hpp"
#include "NekoGui_ConfigItem.hpp"
#include "NekoGui_DataStore.hpp"

namespace NekoGui {
    QString FindCoreAsset(const QString& name);

    QString FindNekoBoxCoreRealPath();

    bool IsAdmin();
} // namespace NekoGui

#define ROUTES_PREFIX_NAME QStringLiteral("routes_box")
#define ROUTES_PREFIX QString(ROUTES_PREFIX_NAME + "/")
