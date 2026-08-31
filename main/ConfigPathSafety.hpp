#pragma once

#include <QString>

namespace NekoGui_ConfigPathSafety {
    // Produces a canonical lexical path relative to the active config root,
    // rejects the reserved recovery tree case-insensitively, and refuses any
    // existing reparse/symlink component below the selected root.
    [[nodiscard]] bool RelativeConfigPath(
        const QString& sourcePath,
        QString* relativePath,
        QString* error);

    // Validates lexical containment and rejects existing reparse/symlink
    // components below rootPath. rootPath itself is the trusted anchor and may
    // already be a user-selected redirected directory.
    [[nodiscard]] bool ValidatePathWithinRoot(
        const QString& rootPath,
        const QString& candidatePath,
        QString* relativePath,
        QString* error);

    // Creates a contained directory and repeats the reparse-point validation
    // after creation, closing the common "junction as parent" escape route.
    [[nodiscard]] bool EnsureDirectoryWithinRoot(
        const QString& rootPath,
        const QString& directoryPath,
        QString* error);
} // namespace NekoGui_ConfigPathSafety
