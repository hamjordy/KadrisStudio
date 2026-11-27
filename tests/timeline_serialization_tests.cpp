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
    const bool replacementOpened = replacementFile.open(QIODevice::WriteOnly);
    assert(replacementOpened);
    if (!replacementOpened) return 1;
    replacementFile.write("test");
    replacementFile.close();

    const int replacedCount = timeline.replaceMediaPath("missing-media.mp4", replacementPath);
    assert(replacedCount == 1);
    if (replacedCount != 1) return 1;
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

    // Slip edits shift the source window while preserving timeline placement and duration.
    QJsonObject slipItem;
    slipItem.insert("id", 77);
    slipItem.insert("path", replacementPath);
    slipItem.insert("track", 0);
    slipItem.insert("timelineStartMs", 9000);
    slipItem.insert("sourceInMs", 1000);
    slipItem.insert("durationMs", 5000);
    slipItem.insert("durationKnown", true);
    slipItem.insert("sourceDurationMs", 12000);
    TimelineWidget slipTimeline;
    slipTimeline.fromJson(QJsonArray{slipItem});
    TimelineWidget::ClipInfo slipBefore;
    assert(slipTimeline.selectedClipInfo(&slipBefore));
    assert(slipTimeline.slipClip(77, 1000));
    TimelineWidget::ClipInfo slipAfter;
    assert(slipTimeline.selectedClipInfo(&slipAfter));
    assert(slipAfter.sourceInMs == 2000);
    assert(slipAfter.timelineStartMs == slipBefore.timelineStartMs);
    assert(slipAfter.durationMs == slipBefore.durationMs);
    assert(slipTimeline.slipClip(77, 100000));
    assert(slipTimeline.selectedClipInfo(&slipAfter));
    assert(slipAfter.sourceInMs == 7000);
    assert(slipTimeline.slipClip(77, -100000));
    assert(slipTimeline.selectedClipInfo(&slipAfter));
    assert(slipAfter.sourceInMs == 0);
    return 0;
}
