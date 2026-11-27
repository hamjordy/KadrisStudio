#pragma once
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace ProjectMediaPaths {
inline QString toStoredPath(const QString &projectFilePath, const QString &mediaPath)
{
    if (mediaPath.isEmpty() || !QFileInfo(mediaPath).isAbsolute())
        return QDir::fromNativeSeparators(mediaPath);
    return QDir::fromNativeSeparators(
        QFileInfo(projectFilePath).absoluteDir().relativeFilePath(mediaPath));
}
inline QString resolveStoredPath(const QString &projectFilePath, const QString &storedPath)
{
    if (storedPath.isEmpty()) return {};
    const QString resolved = QFileInfo(storedPath).isAbsolute()
        ? storedPath : QFileInfo(projectFilePath).absoluteDir().absoluteFilePath(storedPath);
    return QDir::cleanPath(resolved);
}
}
