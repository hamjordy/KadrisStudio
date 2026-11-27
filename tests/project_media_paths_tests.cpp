#include "project_media_paths.h"
#include <QTemporaryDir>
#include <cassert>
int main()
{
    QTemporaryDir temp;
    assert(temp.isValid());
    const QString projectDir = temp.filePath("Project");
    const QString mediaDir = projectDir + "/Media";
    assert(QDir().mkpath(mediaDir));
    const QString project = projectDir + "/edit.kadris";
    const QString media = mediaDir + "/clip.mp4";
    const QString stored = ProjectMediaPaths::toStoredPath(project, media);
    assert(stored == QStringLiteral("Media/clip.mp4"));
    assert(ProjectMediaPaths::resolveStoredPath(project, stored) == QDir::cleanPath(media));
    assert(ProjectMediaPaths::resolveStoredPath(project, media) == QDir::cleanPath(media));
    assert(ProjectMediaPaths::toStoredPath(project, QStringLiteral("Media/clip.mp4")) == QStringLiteral("Media/clip.mp4"));
    assert(ProjectMediaPaths::resolveStoredPath(project, QStringLiteral("Media/clip.mp4")) == QDir::cleanPath(media));
    assert(ProjectMediaPaths::resolveStoredPath(project, QString()).isEmpty());
    return 0;
}
