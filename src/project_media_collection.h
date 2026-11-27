#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStringList>

namespace ProjectMediaCollection {
inline bool copyReferencedMedia(const QStringList &mediaPaths,
                                const QString &projectFolder,
                                QHash<QString, QString> *pathMap,
                                QStringList *missingFiles,
                                QString *errorMessage)
{
    if (!pathMap || !missingFiles || !errorMessage) return false;
    pathMap->clear();
    missingFiles->clear();
    errorMessage->clear();

    QSet<QString> uniqueSources;
    for (const QString &path : mediaPaths) {
        if (path.trimmed().isEmpty()) continue;
        const QString source = QFileInfo(path).canonicalFilePath();
        if (source.isEmpty() || !QFileInfo(source).isFile()) {
            missingFiles->append(QDir::cleanPath(path));
            continue;
        }
        uniqueSources.insert(QDir::cleanPath(source));
    }
    if (!missingFiles->isEmpty()) {
        *errorMessage = QStringLiteral("Some referenced media files are missing.");
        return false;
    }
    if (!QDir().mkpath(projectFolder)) {
        *errorMessage = QStringLiteral("Could not create the project media folder.");
        return false;
    }

    QStringList copiedFiles;
    QSet<QString> reservedTargets;
    for (const QString &source : uniqueSources) {
        const QFileInfo sourceInfo(source);
        const QString baseName = sourceInfo.completeBaseName();
        const QString suffix = sourceInfo.completeSuffix();
        QString candidateName = sourceInfo.fileName();
        int duplicate = 2;
        QString target = QDir(projectFolder).filePath(candidateName);

        while (QFileInfo::exists(target) || reservedTargets.contains(target)) {
            candidateName = suffix.isEmpty()
                ? QStringLiteral("%1 (%2)").arg(baseName).arg(duplicate++)
                : QStringLiteral("%1 (%2).%3").arg(baseName).arg(duplicate++).arg(suffix);
            target = QDir(projectFolder).filePath(candidateName);
        }

        if (QDir::cleanPath(source) == QDir::cleanPath(target)) {
            pathMap->insert(source, source);
            reservedTargets.insert(target);
            continue;
        }
        if (!QFile::copy(source, target)) {
            for (const QString &copied : copiedFiles) QFile::remove(copied);
            pathMap->clear();
            *errorMessage = QStringLiteral("Could not copy media file: %1").arg(sourceInfo.fileName());
            return false;
        }
        copiedFiles.append(target);
        reservedTargets.insert(target);
        pathMap->insert(source, QFileInfo(target).absoluteFilePath());
    }
    return true;
}
}
