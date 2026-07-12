#pragma once
#include <QHostAddress>
#include <QObject>
#include <QString>
//
namespace NekoQtCompat::components::proxy {
    void ClearSystemProxy();
    void SetSystemProxy(int http_port, int socks_port);
} // namespace NekoQtCompat::components::proxy

using namespace NekoQtCompat::components;
using namespace NekoQtCompat::components::proxy;
