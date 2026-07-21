#include <csignal>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTranslator>
#include <QMessageBox>
#include <QStandardPaths>
#include <QLocalSocket>
#include <QLocalServer>
#include <QThread>
#include <QTextStream>

#include "3rdparty/RunGuard.hpp"
#include "main/ConfigTransaction.hpp"
#include "main/NekoGui.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"

#include "ui/mainwindow_interface.h"

#ifdef Q_OS_WIN
#include "sys/windows/MiniDump.h"
#endif

#ifndef Q_OS_WIN
void signal_handler(int signum) {
    Q_UNUSED(signum);
    if (qApp) {
        GetMainWindow()->on_commitDataRequest();
        qApp->exit();
    }
}
#endif

QTranslator* trans = nullptr;
QTranslator* trans_qt = nullptr;

namespace {
    constexpr int kNoProfileId = -1919;

    struct ProfileConfigExportOptions {
        bool enabled = false;
        int profileId = kNoProfileId;
        QString outputPath;
        bool forTest = false;
        bool forExport = true;
    };

    void writeStderr(const QString &message) {
        QTextStream(stderr) << message << Qt::endl;
    }

    bool loadOrCreateStore(JsonStore *store) {
        const bool existed = QFileInfo::exists(store->fn);
        if (store->Load()) return true;
        if (existed) {
            writeStderr("Existing config could not be loaded and was preserved: " + store->fn);
            return false;
        }

        store->Save();
        if (!store->last_save_succeeded || !QFileInfo::exists(store->fn)) {
            writeStderr("New config could not be created: " + store->fn);
            return false;
        }
        return true;
    }

    ProfileConfigExportOptions parseProfileConfigExportOptions(const QStringList &argv) {
        ProfileConfigExportOptions options;
        const auto exportIndex = argv.indexOf("-flag_export_profile_config");
        if (exportIndex < 0) return options;

        options.enabled = true;
        if (argv.size() > exportIndex + 1) {
            bool ok = false;
            const auto profileId = argv.at(exportIndex + 1).toInt(&ok);
            if (ok) options.profileId = profileId;
        }
        if (argv.size() > exportIndex + 2) {
            options.outputPath = argv.at(exportIndex + 2);
        }
        options.forTest = argv.contains("-flag_export_profile_config_for_test");
        // Every file export is side-effect-free and omits product TUN state.
        // The legacy "for_share" spelling is retained as an explicit alias;
        // there is no CLI path that exports a live OS-mode configuration.
        options.forExport = !options.forTest;
        return options;
    }

    bool ensureConfigSubdirs() {
        QDir dir;
        bool ok = true;
        if (!dir.exists("profiles")) ok &= dir.mkdir("profiles");
        if (!dir.exists("groups")) ok &= dir.mkdir("groups");
        if (!dir.exists(ROUTES_PREFIX_NAME)) ok &= dir.mkdir(ROUTES_PREFIX_NAME);
        return ok;
    }

    bool loadRuntimeStores() {
        NekoGui::dataStore->fn = "groups/nekobox.json";
        if (!loadOrCreateStore(NekoGui::dataStore)) return false;

        NekoGui::dataStore->routing = std::make_unique<NekoGui::Routing>();
        NekoGui::dataStore->routing->fn = ROUTES_PREFIX + NekoGui::dataStore->active_routing;
        if (!loadOrCreateStore(NekoGui::dataStore->routing.get())) return false;

        NekoGui::profileManager->LoadManager();
        return true;
    }

    QString resolveProfileConfigExportPath(const QString &rawPath) {
        QFileInfo info(rawPath);
        if (info.isAbsolute()) return info.absoluteFilePath();
        return QDir(QApplication::applicationDirPath()).absoluteFilePath(rawPath);
    }

    int exportProfileConfigAndExit(const ProfileConfigExportOptions &options) {
        if (options.profileId == kNoProfileId || options.outputPath.trimmed().isEmpty()) {
            writeStderr("Usage: nekobox.exe -flag_export_profile_config <profile_id> <output_json_path> "
                        "[-flag_export_profile_config_for_test|-flag_export_profile_config_for_share]");
            return 2;
        }
        if (!ensureConfigSubdirs()) {
            writeStderr("No permission to create config subdirectories.");
            return 1;
        }
        if (!loadRuntimeStores()) return 1;

        const auto profile = NekoGui::profileManager->GetProfile(options.profileId);
        if (profile == nullptr || profile->bean == nullptr) {
            writeStderr(QStringLiteral("Profile not found: %1").arg(options.profileId));
            return 1;
        }
        const auto result = NekoGui::BuildConfig(profile, options.forTest, options.forExport);
        if (!result->error.isEmpty()) {
            writeStderr(QStringLiteral("BuildConfig return error: %1").arg(result->error));
            return 1;
        }

        const auto outputPath = resolveProfileConfigExportPath(options.outputPath);
        const QFileInfo outputInfo(outputPath);
        if (!outputInfo.dir().exists() && !outputInfo.dir().mkpath(".")) {
            writeStderr(QStringLiteral("Cannot create output directory: %1").arg(outputInfo.dir().absolutePath()));
            return 1;
        }

        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            writeStderr(QStringLiteral("Cannot write output file: %1").arg(outputPath));
            return 1;
        }
        file.write(QJsonObject2QString(result->coreConfig, false).toUtf8());
        file.close();
        return 0;
    }
} // namespace

void loadTranslate(const QString& locale) {
    if (trans != nullptr) {
        trans->deleteLater();
    }
    if (trans_qt != nullptr) {
        trans_qt->deleteLater();
    }
    //
    trans = new QTranslator;
    trans_qt = new QTranslator;
    QLocale::setDefault(QLocale(locale));
    //
    if (trans->load(":/translations/" + locale + ".qm")) {
        QCoreApplication::installTranslator(trans);
    }
    if (trans_qt->load(QApplication::applicationDirPath() + "/qtbase_" + locale + ".qm")) {
        QCoreApplication::installTranslator(trans_qt);
    }
}

#define LOCAL_SERVER_PREFIX "nekoraylocalserver-"

int main(int argc, char* argv[]) {
    // Core dump
#ifdef Q_OS_WIN
    Windows_SetCrashHandler();
#endif

    // pre-init QApplication
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif
    QApplication::setQuitOnLastWindowClosed(false);
    auto preQApp = new QApplication(argc, argv);

    // Clean
    QDir::setCurrent(QApplication::applicationDirPath());
    if (QFile::exists("updater.old")) {
        QFile::remove("updater.old");
    }
#ifndef Q_OS_WIN
    if (!QFile::exists("updater")) {
        QFile::link("launcher", "updater");
    }
#endif

    // Flags
    NekoGui::dataStore->argv = QApplication::arguments();
    const auto profileConfigExportOptions = parseProfileConfigExportOptions(NekoGui::dataStore->argv);
    if (NekoGui::dataStore->argv.contains("-many")) NekoGui::dataStore->flag_many = true;
    if (NekoGui::dataStore->argv.contains("-appdata")) {
        NekoGui::dataStore->flag_use_appdata = true;
        int appdataIndex = NekoGui::dataStore->argv.indexOf("-appdata");
        if (NekoGui::dataStore->argv.size() > appdataIndex + 1 && !NekoGui::dataStore->argv.at(appdataIndex + 1).startsWith("-")) {
            NekoGui::dataStore->appdataDir = NekoGui::dataStore->argv.at(appdataIndex + 1);
        }
    }
    if (NekoGui::dataStore->argv.contains("-tray")) NekoGui::dataStore->flag_tray = true;
    if (NekoGui::dataStore->argv.contains("-debug")) NekoGui::dataStore->flag_debug = true;
    const auto restartProfileIndex = NekoGui::dataStore->argv.indexOf("-flag_restart_profile_id");
    if (restartProfileIndex >= 0 && NekoGui::dataStore->argv.size() > restartProfileIndex + 1) {
        bool ok = false;
        const auto restartProfileId = NekoGui::dataStore->argv.at(restartProfileIndex + 1).toInt(&ok);
        if (ok) NekoGui::dataStore->flag_restart_profile_id = restartProfileId;
    }
    if (NekoGui::dataStore->argv.contains("-flag_reorder")) NekoGui::dataStore->flag_reorder = true;
#ifdef NKR_CPP_USE_APPDATA
    NekoGui::dataStore->flag_use_appdata = true; // Example: Package & MacOS
#endif
#ifdef NKR_CPP_DEBUG
    NekoGui::dataStore->flag_debug = true;
#endif

    // dirs & clean
    auto wd = QDir(QApplication::applicationDirPath());
    if (NekoGui::dataStore->flag_use_appdata) {
        QApplication::setApplicationName("nekoray");
        if (!NekoGui::dataStore->appdataDir.isEmpty()) {
            wd.setPath(NekoGui::dataStore->appdataDir);
        } else {
            wd.setPath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        }
    }
    if (!wd.exists()) wd.mkpath(wd.absolutePath());
    if (!wd.exists("config")) wd.mkdir("config");
    QDir::setCurrent(wd.absoluteFilePath("config"));

    // init QApplication
    delete preQApp;
    QApplication a(argc, argv);

    const auto transactionIssues = NekoGui_ConfigTransaction::BlockingTransactionIssues();
    if (!transactionIssues.isEmpty()) {
        const auto message = QStringLiteral(
                                 "Configuration startup is blocked because an interrupted multi-file transaction "
                                 "requires explicit recovery. No configuration file was modified during this check.\n\n%1")
                                 .arg(transactionIssues.join(QStringLiteral("\n")));
        writeStderr(message);
        if (!profileConfigExportOptions.enabled) {
            QMessageBox::critical(nullptr, QStringLiteral("Configuration recovery required"), message);
        }
        return 1;
    }
    QDir("temp").removeRecursively();

    // dispatchers
    DS_cores = new QThread;
    DS_cores->start();

    if (profileConfigExportOptions.enabled) {
        const auto exportExitCode = exportProfileConfigAndExit(profileConfigExportOptions);
        DS_cores->quit();
        DS_cores->wait();
        return exportExitCode;
    }

    // RunGuard
    RunGuard guard("nekoray" + wd.absolutePath());
    quint64 guard_data_in = GetRandomUint64();
    quint64 guard_data_out = 0;
    if (!NekoGui::dataStore->flag_many && !guard.tryToRun(&guard_data_in)) {
        // Some Good System
        if (guard.isAnotherRunning(&guard_data_out)) {
            // Wake up a running instance
            QLocalSocket socket;
            socket.connectToServer(LOCAL_SERVER_PREFIX + Int2String(guard_data_out));
            qDebug() << socket.fullServerName();
            if (!socket.waitForConnected(500)) {
                qDebug() << "Failed to wake a running instance.";
                return 0;
            }
            qDebug() << "connected to local server, try to raise another program";
            return 0;
        }
        // Some Bad System
        QMessageBox::warning(nullptr, "NekoGui", "RunGuard disallow to run, use -many to force start.");
        return 0;
    }
    MF_release_runguard = [&] { guard.release(); };

// icons
#if (QT_VERSION >= QT_VERSION_CHECK(5, 11, 0))
    QIcon::setFallbackSearchPaths(QStringList{
        ":/neko",
        ":/icon",
    });
#endif

    // icon for no theme
    if (QIcon::themeName().isEmpty()) {
        QIcon::setThemeName("breeze");
    }

    // Dir
    QDir dir;
    bool dir_success = true;
    if (!dir.exists("profiles")) {
        dir_success &= dir.mkdir("profiles");
    }
    if (!dir.exists("groups")) {
        dir_success &= dir.mkdir("groups");
    }
    if (!dir.exists(ROUTES_PREFIX_NAME)) {
        dir_success &= dir.mkdir(ROUTES_PREFIX_NAME);
    }
    if (!dir_success) {
        QMessageBox::warning(nullptr, "Error", "No permission to write " + dir.absolutePath());
        return 1;
    }

    // Load dataStore
    NekoGui::dataStore->fn = "groups/nekobox.json";
    if (!loadOrCreateStore(NekoGui::dataStore)) {
        QMessageBox::critical(
            nullptr,
            "Error",
            "An existing configuration could not be loaded. The original file was preserved; startup is aborted.");
        return 1;
    }

    // Datastore & Flags
    if (NekoGui::dataStore->start_minimal) NekoGui::dataStore->flag_tray = true;

    // load routing
    NekoGui::dataStore->routing = std::make_unique<NekoGui::Routing>();
    NekoGui::dataStore->routing->fn = ROUTES_PREFIX + NekoGui::dataStore->active_routing;
    if (!loadOrCreateStore(NekoGui::dataStore->routing.get())) {
        QMessageBox::critical(
            nullptr,
            "Error",
            "An existing routing configuration could not be loaded. The original file was preserved; startup is aborted.");
        return 1;
    }

    // Translate
    QString locale;
    switch (NekoGui::dataStore->language) {
        case 1: // English
            break;
        case 2:
            locale = "zh_CN";
            break;
        case 3:
            locale = "fa_IR"; // farsi(iran)
            break;
        case 4:
            locale = "ru_RU"; // Russian
            break;
        default:
            locale = QLocale().name();
    }
    QGuiApplication::tr("QT_LAYOUT_DIRECTION");
    loadTranslate(locale);

    // The CRT signal callback cannot safely call Qt, and the legacy callback
    // bypassed the guarded exit path.  On Windows, ignore console termination
    // signals so an internal TUN cannot be dropped through this side door.
    // Normal user exits continue through MainWindow::on_menu_exit_triggered().
#ifdef Q_OS_WIN
    std::signal(SIGTERM, SIG_IGN);
    std::signal(SIGINT, SIG_IGN);
#else
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
#endif

    // QLocalServer
    QLocalServer server;
    auto server_name = LOCAL_SERVER_PREFIX + Int2String(guard_data_in);
    QLocalServer::removeServer(server_name);
    server.listen(server_name);
    QObject::connect(&server, &QLocalServer::newConnection, &a, [&] {
        auto socket = server.nextPendingConnection();
        qDebug() << "nextPendingConnection:" << server_name << socket;
        socket->deleteLater();
        // raise main window
        MW_dialog_message("", "Raise");
    });

    UI_InitMainWindow();
    return QApplication::exec();
}
