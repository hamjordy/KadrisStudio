#include "project_media_collection.h"

#include <QTemporaryDir>
#include <cassert>

int main()
{
    QTemporaryDir temp;
    assert(temp.isValid());

    const QString sourceA = temp.filePath("camera-a/clip.mp4");
    const QString sourceB = temp.filePath("camera-b/clip.mp4");
    assert(QDir().mkpath(QFileInfo(sourceA).absolutePath()));
    assert(QDir().mkpath(QFileInfo(sourceB).absolutePath()));
    QFile fileA(sourceA);
    assert(fileA.open(QIODevice::WriteOnly));
    fileA.write("camera-a");
    fileA.close();
    QFile fileB(sourceB);
    assert(fileB.open(QIODevice::WriteOnly));
    fileB.write("camera-b");
    fileB.close();

    const QString destination = temp.filePath("Project/Media");
    QHash<QString, QString> mapping;
    QStringList missing;
    QString error;
    assert(ProjectMediaCollection::copyReferencedMedia(
        {sourceA, sourceB, sourceA}, destination, &mapping, &missing, &error));
    assert(missing.isEmpty());
    assert(mapping.size() == 2);
    assert(QFileInfo::exists(mapping.value(QFileInfo(sourceA).canonicalFilePath())));
    assert(QFileInfo::exists(mapping.value(QFileInfo(sourceB).canonicalFilePath())));
    assert(QFileInfo(mapping.value(QFileInfo(sourceA).canonicalFilePath())).fileName()
        != QFileInfo(mapping.value(QFileInfo(sourceB).canonicalFilePath())).fileName());

    const QString missingPath = temp.filePath("not-found.mov");
    assert(!ProjectMediaCollection::copyReferencedMedia(
        {sourceA, missingPath}, temp.filePath("Other/Media"), &mapping, &missing, &error));
    assert(missing.size() == 1);
    assert(!QDir().exists(temp.filePath("Other/Media")));
    return 0;
}
