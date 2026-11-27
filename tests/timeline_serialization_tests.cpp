#include "timelinewidget.h"

#include <QApplication>
#include <QMouseEvent>
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
    QJsonArray advancedTrackStates = timeline.trackStatesToJson();
    QJsonObject videoOne = advancedTrackStates.at(0).toObject();
    videoOne.insert("targeted", false);
    videoOne.insert("syncLocked", true);
    advancedTrackStates[0] = videoOne;
    timeline.trackStatesFromJson(advancedTrackStates);
    assert(!timeline.isTrackTargeted(0));
    assert(timeline.isTrackSyncLocked(0));

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
    assert(!restored.isTrackTargeted(0));
    assert(restored.isTrackSyncLocked(0));
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

    // Nesting keeps every selected clip's settings and relative timing in the project.
    QJsonArray nestInput{
        QJsonObject{{"id", 201}, {"path", "clip-a.mp4"}, {"track", 0},
            {"timelineStartMs", 1000}, {"durationMs", 3000}},
        QJsonObject{{"id", 202}, {"path", "clip-b.mp4"}, {"track", 1},
            {"timelineStartMs", 2500}, {"durationMs", 2000}}
    };
    TimelineWidget nestTimeline;
    nestTimeline.fromJson(nestInput);
    assert(nestTimeline.nestSelectedClips("Intro Montage") == false);
    // Select both clips using the public timeline interaction path.
    nestTimeline.resize(1600, 300);
    QMouseEvent selectFirst(QEvent::MouseButtonPress, QPointF(230, 55),
        Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&nestTimeline, &selectFirst);
    QMouseEvent selectSecond(QEvent::MouseButtonPress, QPointF(350, 96),
        Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&nestTimeline, &selectSecond);
    assert(nestTimeline.nestSelectedClips("Intro Montage"));
    QJsonArray nestedSaved = nestTimeline.toJson();
    assert(nestedSaved.size() == 1);
    assert(nestedSaved.first().toObject().value("nestedSequence").toBool());
    assert(nestedSaved.first().toObject().value("nestedName").toString() == "Intro Montage");
    assert(nestedSaved.first().toObject().value("nestedClips").toArray().size() == 2);
    TimelineWidget nestedReloaded;
    nestedReloaded.fromJson(nestedSaved);
    assert(nestedReloaded.toJson() == nestedSaved);
    assert(nestedReloaded.unnestSelectedClip());
    assert(nestedReloaded.toJson().size() == 2);

    // Preview lookup resolves child media at the nested sequence's absolute timeline time.
    TimelineWidget previewNested;
    previewNested.fromJson(nestedSaved);
    TimelineWidget::ClipInfo nestedPreviewClip;
    const int nestedParentTrack = nestedSaved.first().toObject().value("track").toInt();
    assert(previewNested.clipAtTimeOnTrack(1200, nestedParentTrack, &nestedPreviewClip));
    assert(nestedPreviewClip.path == "clip-a.mp4");
    assert(nestedPreviewClip.timelineStartMs == 1000);
    assert(previewNested.clipAtTimeOnTrack(3000, nestedParentTrack, &nestedPreviewClip));
    assert(nestedPreviewClip.path == "clip-b.mp4");
    assert(nestedPreviewClip.timelineStartMs == 2500);

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

    // Slip offsets and source bounds scale with the clip playback rate.
    QJsonObject fastSlipItem{{"id", 78}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 1000}, {"durationMs", 4000},
        {"durationKnown", true}, {"sourceDurationMs", 12000}, {"playbackRate", 2.0}};
    TimelineWidget fastSlipTimeline;
    fastSlipTimeline.fromJson(QJsonArray{fastSlipItem});
    assert(fastSlipTimeline.slipClip(78, 1000));
    TimelineWidget::ClipInfo fastSlipInfo;
    assert(fastSlipTimeline.selectedClipInfo(&fastSlipInfo));
    assert(fastSlipInfo.sourceInMs == 3000);
    assert(fastSlipTimeline.slipClip(78, 100000));
    assert(fastSlipTimeline.selectedClipInfo(&fastSlipInfo));
    assert(fastSlipInfo.sourceInMs == 4000);

    // Rate Stretch keeps the source span fixed and stores the playback speed.
    QJsonObject rateItem{{"id", 79}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 1000}, {"durationMs", 4000},
        {"durationKnown", true}, {"sourceDurationMs", 12000}};
    TimelineWidget rateTimeline;
    rateTimeline.fromJson(QJsonArray{rateItem});
    assert(rateTimeline.rateStretchClip(79, 2000));
    QJsonObject rateSaved = rateTimeline.toJson().at(0).toObject();
    assert(rateSaved.value("durationMs").toInt() == 2000);
    assert(qAbs(rateSaved.value("playbackRate").toDouble() - 2.0) < 0.001);
    TimelineWidget rateRestored;
    rateRestored.fromJson(QJsonArray{rateSaved});
    TimelineWidget::ClipInfo rateInfo;
    assert(rateRestored.selectedClipInfo(&rateInfo));
    assert(qAbs(rateInfo.playbackRate - 2.0) < 0.001);
    assert(!rateRestored.rateStretchClip(79, 100));
    assert(!rateRestored.rateStretchClip(79, 20000));

    // Slide edits move the selected clip while trimming the two adjacent clips.
    QJsonObject previousItem{{"id", 81}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 0}, {"durationMs", 4000},
        {"durationKnown", true}, {"sourceDurationMs", 10000}};
    QJsonObject selectedItem{{"id", 82}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 4000}, {"sourceInMs", 1000}, {"durationMs", 3000},
        {"durationKnown", true}, {"sourceDurationMs", 10000}};
    QJsonObject nextItem{{"id", 83}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 7000}, {"sourceInMs", 2000}, {"durationMs", 5000},
        {"durationKnown", true}, {"sourceDurationMs", 10000}};
    TimelineWidget slideTimeline;
    slideTimeline.fromJson(QJsonArray{previousItem, selectedItem, nextItem});
    assert(slideTimeline.slideClip(82, 1000));
    const QJsonArray slideClips = slideTimeline.toJson();
    const QJsonObject slidPrevious = slideClips.at(0).toObject();
    const QJsonObject slidSelected = slideClips.at(1).toObject();
    const QJsonObject slidNext = slideClips.at(2).toObject();
    assert(slidPrevious.value("durationMs").toInt() == 5000);
    assert(slidSelected.value("timelineStartMs").toInt() == 5000);
    assert(slidSelected.value("durationMs").toInt() == 3000);
    assert(slidNext.value("timelineStartMs").toInt() == 8000);
    assert(slidNext.value("sourceInMs").toInt() == 3000);
    assert(slidNext.value("durationMs").toInt() == 4000);

    // Slide advances the next clip's source window at that clip's playback rate.
    nextItem.insert("playbackRate", 2.0);
    TimelineWidget fastSlideTimeline;
    fastSlideTimeline.fromJson(QJsonArray{previousItem, selectedItem, nextItem});
    assert(fastSlideTimeline.slideClip(82, 1000));
    const QJsonObject fastSlidNext = fastSlideTimeline.toJson().at(2).toObject();
    assert(fastSlidNext.value("sourceInMs").toInt() == 4000);
    assert(fastSlidNext.value("durationMs").toInt() == 4000);
    // Splitting a 2x clip must advance the second segment by twice the timeline offset.
    QJsonObject fastSplitItem{{"id", 84}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 1000}, {"durationMs", 6000},
        {"durationKnown", true}, {"sourceDurationMs", 16000}, {"playbackRate", 2.0}};
    TimelineWidget fastSplitTimeline;
    fastSplitTimeline.fromJson(QJsonArray{fastSplitItem});
    fastSplitTimeline.setPlayheadMs(2000);
    assert(fastSplitTimeline.splitSelectedClipAtPlayhead());
    const QJsonArray fastSplitClips = fastSplitTimeline.toJson();
    assert(fastSplitClips.size() == 2);
    assert(fastSplitClips.at(0).toObject().value("durationMs").toInt() == 2000);
    assert(fastSplitClips.at(1).toObject().value("sourceInMs").toInt() == 5000);
    assert(fastSplitClips.at(1).toObject().value("durationMs").toInt() == 4000);

    QJsonObject fastSplitAllItem = fastSplitItem;
    fastSplitAllItem.insert("id", 85);
    TimelineWidget fastSplitAllTimeline;
    fastSplitAllTimeline.fromJson(QJsonArray{fastSplitAllItem});
    fastSplitAllTimeline.setPlayheadMs(2000);
    assert(fastSplitAllTimeline.splitAllClipsAtPlayhead() == 1);
    const QJsonArray fastSplitAllClips = fastSplitAllTimeline.toJson();
    assert(fastSplitAllClips.at(1).toObject().value("sourceInMs").toInt() == 5000);

    // Track targeting limits Add Edit to All Tracks to enabled tracks.
    QJsonArray targetedSplitInput{
        QJsonObject{{"id", 91}, {"path", replacementPath}, {"track", 0},
            {"timelineStartMs", 0}, {"durationMs", 5000}},
        QJsonObject{{"id", 92}, {"path", replacementPath}, {"track", 1},
            {"timelineStartMs", 0}, {"durationMs", 5000}}
    };
    TimelineWidget targetedSplitTimeline;
    targetedSplitTimeline.fromJson(targetedSplitInput);
    QJsonArray targetStates = targetedSplitTimeline.trackStatesToJson();
    QJsonObject disabledTarget = targetStates.at(1).toObject();
    disabledTarget.insert("targeted", false);
    targetStates[1] = disabledTarget;
    targetedSplitTimeline.trackStatesFromJson(targetStates);
    targetedSplitTimeline.setPlayheadMs(2000);
    assert(targetedSplitTimeline.splitAllClipsAtPlayhead() == 1);

    // Ripple delete also shifts later clips on sync-locked tracks.
    QJsonArray syncRippleInput{
        QJsonObject{{"id", 93}, {"path", replacementPath}, {"track", 0},
            {"timelineStartMs", 0}, {"durationMs", 2000}},
        QJsonObject{{"id", 94}, {"path", replacementPath}, {"track", 0},
            {"timelineStartMs", 2000}, {"durationMs", 3000}},
        QJsonObject{{"id", 95}, {"path", replacementPath}, {"track", 1},
            {"timelineStartMs", 3000}, {"durationMs", 2000}}
    };
    TimelineWidget syncRippleTimeline;
    syncRippleTimeline.fromJson(syncRippleInput);
    QJsonArray syncStates = syncRippleTimeline.trackStatesToJson();
    QJsonObject syncTrack = syncStates.at(1).toObject();
    syncTrack.insert("syncLocked", true);
    syncStates[1] = syncTrack;
    syncRippleTimeline.trackStatesFromJson(syncStates);
    assert(syncRippleTimeline.rippleDeleteSelectedClip());
    const QJsonArray syncRippleResult = syncRippleTimeline.toJson();
    assert(syncRippleResult.size() == 2);
    bool shiftedSyncClip = false;
    for (const QJsonValue &value : syncRippleResult) {
        const QJsonObject clip = value.toObject();
        if (clip.value("id").toInt() == 95) {
            shiftedSyncClip = clip.value("timelineStartMs").toInt() == 1000;
        }
    }
    assert(shiftedSyncClip);

    // Mouse-driven trim and roll edits must translate timeline movement
    // into source movement using each clip's playback rate.
    auto dragTimeline = [](TimelineWidget &widget, const QPoint &from,
                           const QPoint &to, Qt::KeyboardModifiers modifiers) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(from), QPointF(from),
            Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&widget, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(to),
            Qt::NoButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(&widget, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to), QPointF(to),
            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(&widget, &release);
    };

    QJsonObject fastTrimLeftItem{{"id", 86}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 2000}, {"durationMs", 5000},
        {"durationKnown", true}, {"sourceDurationMs", 16000}, {"playbackRate", 2.0}};
    TimelineWidget fastTrimLeftTimeline;
    fastTrimLeftTimeline.fromJson(QJsonArray{fastTrimLeftItem});
    dragTimeline(fastTrimLeftTimeline, QPoint(144, 50), QPoint(224, 50), Qt::NoModifier);
    const QJsonObject fastTrimLeft = fastTrimLeftTimeline.toJson().at(0).toObject();
    assert(fastTrimLeft.value("timelineStartMs").toInt() == 1000);
    assert(fastTrimLeft.value("sourceInMs").toInt() == 4000);
    assert(fastTrimLeft.value("durationMs").toInt() == 4000);

    QJsonObject fastTrimRightItem{{"id", 87}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 1000}, {"durationMs", 5000},
        {"durationKnown", true}, {"sourceDurationMs", 16000}, {"playbackRate", 2.0}};
    TimelineWidget fastTrimRightTimeline;
    fastTrimRightTimeline.fromJson(QJsonArray{fastTrimRightItem});
    dragTimeline(fastTrimRightTimeline, QPoint(540, 50), QPoint(620, 50), Qt::NoModifier);
    const QJsonObject fastTrimRight = fastTrimRightTimeline.toJson().at(0).toObject();
    assert(fastTrimRight.value("durationMs").toInt() == 6000);
    assert(fastTrimRight.value("sourceInMs").toInt() == 1000);

    // Ctrl+Shift-dragging the right edge must use Rate Stretch, not ordinary trim.
    QJsonObject mouseRateItem{{"id", 90}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 1000}, {"durationMs", 5000},
        {"durationKnown", true}, {"sourceDurationMs", 16000}};
    TimelineWidget mouseRateTimeline;
    mouseRateTimeline.fromJson(QJsonArray{mouseRateItem});
    dragTimeline(mouseRateTimeline, QPoint(540, 50), QPoint(620, 50),
        Qt::ControlModifier | Qt::ShiftModifier);
    const QJsonObject mouseRate = mouseRateTimeline.toJson().at(0).toObject();
    assert(mouseRate.value("durationMs").toInt() == 6000);
    assert(qAbs(mouseRate.value("playbackRate").toDouble() - (5.0 / 6.0)) < 0.001);

    QJsonObject fastRollOut{{"id", 88}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 0}, {"sourceInMs", 0}, {"durationMs", 4000},
        {"durationKnown", true}, {"sourceDurationMs", 12000}, {"playbackRate", 2.0}};
    QJsonObject fastRollIn{{"id", 89}, {"path", replacementPath}, {"track", 0},
        {"timelineStartMs", 4000}, {"sourceInMs", 1000}, {"durationMs", 5000},
        {"durationKnown", true}, {"sourceDurationMs", 12000}, {"playbackRate", 2.0}};
    TimelineWidget fastRollTimeline;
    fastRollTimeline.fromJson(QJsonArray{fastRollOut, fastRollIn});
    dragTimeline(fastRollTimeline, QPoint(461, 50), QPoint(541, 50), Qt::AltModifier);
    const QJsonArray fastRollClips = fastRollTimeline.toJson();
    assert(fastRollClips.at(0).toObject().value("durationMs").toInt() == 5000);
    assert(fastRollClips.at(1).toObject().value("timelineStartMs").toInt() == 5000);
    assert(fastRollClips.at(1).toObject().value("sourceInMs").toInt() == 3000);
    assert(fastRollClips.at(1).toObject().value("durationMs").toInt() == 4000);

    return 0;
}
