#include "ConfigPathSafety.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
    bool validateWindowsPathComponents(const QString& relative, QString* error) {
        static const QSet<QString> reservedNames = {
            QStringLiteral("CON"),
            QStringLiteral("PRN"),
            QStringLiteral("AUX"),
            QStringLiteral("NUL"),
            QStringLiteral("CONIN$"),
            QStringLiteral("CONOUT$"),
        };
        const auto components = relative.split('/', Qt::SkipEmptyParts);
        for (const auto& component: components) {
            if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))) {
                *error = QStringLiteral(
                             "Windows path components must not end in a dot or space: %1")
                             .arg(component);
                return false;
            }
            for (const auto character: component) {
                if (character.unicode() < 32 || QStringLiteral(":\"<>|?*~").contains(character)) {
                    *error = QStringLiteral("Windows path component contains an unsafe character: %1")
                                 .arg(component);
                    return false;
                }
            }
            const auto baseName = component.section(QLatin1Char('.'), 0, 0).toUpper();
            const auto numberedDevice =
                (baseName.startsWith(QStringLiteral("COM")) ||
                 baseName.startsWith(QStringLiteral("LPT"))) &&
                baseName.size() == 4 && baseName.at(3) >= QLatin1Char('1') &&
                baseName.at(3) <= QLatin1Char('9');
            if (reservedNames.contains(baseName) || numberedDevice) {
                *error = QStringLiteral("Windows reserved device name is not a safe path component: %1")
                             .arg(component);
                return false;
            }
        }
        return true;
    }
#endif

    bool pathComponentIsReparsePoint(const QString& path, bool* known, QString* error) {
#ifdef Q_OS_WIN
        const auto nativePath = QDir::toNativeSeparators(path);
        const auto attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(nativePath.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const QFileInfo info(path);
            if (info.exists() || info.isSymLink()) {
                *known = false;
                *error = QStringLiteral("Cannot inspect Windows path attributes: %1").arg(path);
                return false;
            }
            *known = true;
            return false;
        }
        *known = true;
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        const QFileInfo info(path);
        *known = true;
        return info.isSymLink();
#endif
    }

    bool relativePathWithinRoot(
        const QString& rootPath,
        const QString& candidatePath,
        QString* relativePath,
        QString* error) {
        const QDir root(QFileInfo(rootPath).absoluteFilePath());
        const auto absoluteCandidate = QFileInfo(candidatePath).absoluteFilePath();
        auto relative = QDir::cleanPath(root.relativeFilePath(absoluteCandidate));
        relative.replace('\\', '/');

        if (relative.isEmpty() || relative == QStringLiteral(".") ||
            relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")) ||
            QDir::isAbsolutePath(relative)) {
            *error = QStringLiteral("Path escapes its trusted root: %1").arg(absoluteCandidate);
            return false;
        }

        if (relativePath != nullptr) *relativePath = relative;
        return true;
    }
} // namespace

namespace NekoGui_ConfigPathSafety {
    bool ValidatePathWithinRoot(
        const QString& rootPath,
        const QString& candidatePath,
        QString* relativePath,
        QString* error) {
        QString relative;
        if (!relativePathWithinRoot(rootPath, candidatePath, &relative, error)) return false;
#ifdef Q_OS_WIN
        if (!validateWindowsPathComponents(relative, error)) return false;
#endif

        auto current = QFileInfo(rootPath).absoluteFilePath();
        const auto components = relative.split('/', Qt::SkipEmptyParts);
        for (int index = 0; index < components.size(); ++index) {
            current = QDir(current).absoluteFilePath(components.at(index));
            const QFileInfo info(current);
            bool attributesKnown = true;
            QString attributeError;
            const auto reparsePoint = pathComponentIsReparsePoint(
                current, &attributesKnown, &attributeError);
            if (!attributesKnown) {
                *error = attributeError;
                return false;
            }
            if (reparsePoint) {
                *error = QStringLiteral("Path contains a reparse or symbolic-link component: %1")
                             .arg(current);
                return false;
            }
            if (!info.exists() && !info.isSymLink()) break;
            if (index + 1 < components.size() && !info.isDir()) {
                *error = QStringLiteral("Path parent is not a directory: %1").arg(current);
                return false;
            }
        }

        if (relativePath != nullptr) *relativePath = relative;
        return true;
    }

    bool RelativeConfigPath(
        const QString& sourcePath,
        QString* relativePath,
        QString* error) {
        QString relative;
        if (!ValidatePathWithinRoot(
                QDir::currentPath(), sourcePath, &relative, error)) {
            return false;
        }
        if (relative.compare(QStringLiteral("recovery"), Qt::CaseInsensitive) == 0 ||
            relative.startsWith(QStringLiteral("recovery/"), Qt::CaseInsensitive)) {
            *error = QStringLiteral("Configuration mutation refuses the reserved recovery tree: %1")
                         .arg(relative);
            return false;
        }
        if (relativePath != nullptr) *relativePath = relative;
        return true;
    }

    bool EnsureDirectoryWithinRoot(
        const QString& rootPath,
        const QString& directoryPath,
        QString* error) {
        const auto absoluteRoot = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
        const auto absoluteDirectory = QDir::cleanPath(QFileInfo(directoryPath).absoluteFilePath());
#ifdef Q_OS_WIN
        const auto sameAsRoot = absoluteRoot.compare(absoluteDirectory, Qt::CaseInsensitive) == 0;
#else
        const auto sameAsRoot = absoluteRoot == absoluteDirectory;
#endif
        if (sameAsRoot) {
            const QFileInfo rootInfo(absoluteRoot);
            if (!rootInfo.exists() || !rootInfo.isDir()) {
                *error = QStringLiteral("Trusted root directory does not exist: %1").arg(absoluteRoot);
                return false;
            }
            return true;
        }
        if (!ValidatePathWithinRoot(rootPath, directoryPath, nullptr, error)) return false;
        if (!QDir().mkpath(QFileInfo(directoryPath).absoluteFilePath())) {
            *error = QStringLiteral("Cannot create contained directory: %1")
                         .arg(QFileInfo(directoryPath).absoluteFilePath());
            return false;
        }
        return ValidatePathWithinRoot(rootPath, directoryPath, nullptr, error);
    }
} // namespace NekoGui_ConfigPathSafety
