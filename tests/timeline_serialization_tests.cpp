#include "timelinewidget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>

#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    TimelineWidget timeline;

    timeline.addClip("missing-media.mp4");
    TimelineWidget::ClipInfo clip;
    assert(timeline.selectedClipInfo(&clip));
    const int clipId = clip.id;

    assert(timeline.setClipColorSettings(clipId, 12, -8, 115, 4, -5, 6, 7, -9, 10));
    assert(timeline.setClipTransformSettings(clipId, 125, 18, -22));
    assert(timeline.setClipOpacity(clipId, 73));
    assert(timeline.setClipBlendMode(clipId, 2));
    assert(timeline.setClipAudioSettings(clipId, 82, 450, 700));
    assert(timeline.setClipAudioMuted(clipId, true));

    assert(timeline.addBrightnessKeyframe(clipId, 800, 35));
    assert(timeline.addContrastKeyframe(clipId, 1200, -25));
    assert(timeline.addTransformKeyframe(clipId, 1600, 140, 25, -15));
    assert(timeline.addOpacityKeyframe(clipId, 2000, 60));
    assert(timeline.addAudioVolumeKeyframe(clipId, 2400, 55));

    timeline.setPlayheadMs(3200);
    timeline.addMarker();
    QJsonArray trackStates;
    trackStates.append(QJsonObject{{"locked", true}, {"muted", false}});
    trackStates.append(QJsonObject{{"locked", false}, {"muted", true}});
    timeline.trackStatesFromJson(trackStates);

    QTemporaryDir temporaryDirectory;
    assert(temporaryDirectory.isValid());
    const QString replacementPath = temporaryDirectory.filePath("replacement.mp4");
    QFile replacementFile(replacementPath);
    assert(replacementFile.open(QIODevice::WriteOnly));
    replacementFile.write("test");
    replacementFile.close();

    assert(timeline.replaceMediaPath("missing-media.mp4", replacementPath) == 1);
    const QJsonArray clipsBefore = timeline.toJson();
    assert(clipsBefore.first().toObject().value("path").toString() == replacementPath);
    const QJsonArray markersBefore = timeline.markersToJson();
    const QJsonArray tracksBefore = timeline.trackStatesToJson();

    TimelineWidget restored;
    restored.fromJson(clipsBefore);
    restored.markersFromJson(markersBefore);
    restored.trackStatesFromJson(tracksBefore);

    assert(restored.toJson() == clipsBefore);
    assert(restored.markersToJson() == markersBefore);
    assert(restored.trackStatesToJson() == tracksBefore);
    assert(restored.selectedClipInfo(&clip));
    assert(clip.path == replacementPath);
    assert(clip.brightness == 12);
    assert(clip.contrast == -8);
    assert(clip.saturation == 115);
    assert(clip.scale == 125);
    assert(clip.positionX == 18);
    assert(clip.positionY == -22);
    assert(clip.opacity == 73);
    assert(clip.blendMode == 2);
    assert(clip.audioVolume == 82);
    assert(clip.audioMuted);
    assert(clip.fadeInMs == 450);
    assert(clip.fadeOutMs == 700);
    return 0;
}
