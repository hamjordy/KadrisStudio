#include "timelinewidget.h"
#include "frame_timebase.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <cstring>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QSizePolicy>
#include <QStringList>
#include <limits>
#include <algorithm>
#include <QTimer>
#include <QUrl>

#include <cmath>

namespace {
constexpr int kHeaderHeight = 34;
constexpr int kTrackLabelWidth = 126;
constexpr int kTrackTop = 44;
constexpr int kTrackHeight = 34;
constexpr int kTrackGap = 7;
constexpr int kTimelineStartX = 142;
constexpr int kClipMinimumWidth = 18;
constexpr qint64 kDefaultClipDurationMs = 5000;
constexpr qint64 kMinimumClipDurationMs = 250;
constexpr int kTrimHandleWidth = 8;
constexpr int kTrackCount = 5;

QVector<float> readWaveformPeaks(const QString &path)
{
    QVector<float> peaks;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return peaks;
    // Read only the RIFF header and chunk metadata first. Avoid loading an
    // entire multi-gigabyte WAV into memory just to build a small thumbnail.
    QByteArray header = file.read(12);
    if (header.size() != 12 || header.left(4) != "RIFF"
        || header.mid(8, 4) != "WAVE") return peaks;

    quint16 format = 0, channels = 0, bitsPerSample = 0, blockAlign = 0;
    qint64 dataOffset = -1;
    qint64 dataSize = 0;
    while (!file.atEnd()) {
        const QByteArray chunkHeader = file.read(8);
        if (chunkHeader.size() != 8) break;
        const quint32 chunkSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(chunkHeader.constData() + 4));
        const qint64 payload = file.pos();
        if (chunkHeader.left(4) == "fmt " && chunkSize >= 16) {
            const QByteArray fmt = file.read(16);
            if (fmt.size() != 16) return peaks;
            format = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(fmt.constData()));
            channels = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(fmt.constData() + 2));
            blockAlign = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(fmt.constData() + 12));
            bitsPerSample = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar *>(fmt.constData() + 14));
        } else if (chunkHeader.left(4) == "data") {
            dataOffset = payload;
            dataSize = chunkSize;
            break;
        }
        const qint64 nextChunk = payload + chunkSize + (chunkSize & 1u);
        if (nextChunk < payload || !file.seek(nextChunk)) break;
    }

    if (dataOffset < 0 || dataSize <= 0 || channels == 0 || blockAlign == 0
        || (format != 1 && format != 3)
        || (bitsPerSample != 8 && bitsPerSample != 16
            && bitsPerSample != 24 && bitsPerSample != 32)
        || (format == 3 && bitsPerSample != 32)) return peaks;

    const int bytesPerSample = bitsPerSample / 8;
    if (blockAlign < channels * bytesPerSample) return peaks;
    dataSize = qMin<qint64>(dataSize, file.size() - dataOffset);
    const qint64 frameCount = dataSize / blockAlign;
    if (frameCount <= 0 || !file.seek(dataOffset)) return peaks;
    const int bucketCount = static_cast<int>(qMin<qint64>(1600, frameCount));
    peaks.fill(0.0f, bucketCount);
    QByteArray frameBuffer;

    for (qint64 frame = 0; frame < frameCount; ++frame) {
        if (frameBuffer.size() != blockAlign) frameBuffer.resize(blockAlign);
        if (file.read(frameBuffer.data(), blockAlign) != blockAlign) break;
        const auto *frameData = reinterpret_cast<const uchar *>(frameBuffer.constData());
        float framePeak = 0.0f;
        for (quint16 channel = 0; channel < channels; ++channel) {
            const uchar *sample = frameData + channel * bytesPerSample;
            double value = 0.0;
            if (format == 3) {
                float decoded = 0.0f;
                std::memcpy(&decoded, sample, sizeof(float));
                value = std::isfinite(decoded) ? decoded : 0.0;
            } else if (bitsPerSample == 8) {
                value = (static_cast<int>(sample[0]) - 128) / 128.0;
            } else if (bitsPerSample == 16) {
                value = qFromLittleEndian<qint16>(sample) / 32768.0;
            } else if (bitsPerSample == 24) {
                qint32 decoded = static_cast<qint32>(sample[0])
                    | (static_cast<qint32>(sample[1]) << 8)
                    | (static_cast<qint32>(sample[2]) << 16);
                if (decoded & 0x800000) decoded |= static_cast<qint32>(0xff000000);
                value = decoded / 8388608.0;
            } else {
                value = qFromLittleEndian<qint32>(sample) / 2147483648.0;
            }
            framePeak = qMax(framePeak, static_cast<float>(qBound(0.0, std::abs(value), 1.0)));
        }
        const int bucket = static_cast<int>(
            (static_cast<long double>(frame) * bucketCount) / frameCount);
        if (bucket < bucketCount)
            peaks[bucket] = qMax(peaks.at(bucket), framePeak);
    }
    return peaks;
}

}

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("timelineCanvas");
    setMinimumSize(1100, 270);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setMouseTracking(true);
    setToolTip("Track controls: click L to lock/unlock and M to mute/unmute a track");
    setFocusPolicy(Qt::StrongFocus);
}

void TimelineWidget::setFrameRate(int numerator, int denominator)
{
    m_timebase = FrameTimebase(numerator, denominator);
    m_playheadMs = m_timebase.snapMilliseconds(m_playheadMs);
    update();
}

int TimelineWidget::frameRateNumerator() const
{
    return static_cast<int>(m_timebase.fpsNumerator());
}

int TimelineWidget::frameRateDenominator() const
{
    return static_cast<int>(m_timebase.fpsDenominator());
}

qint64 TimelineWidget::frameDurationMs() const
{
    return m_timebase.frameToMilliseconds(1);
}

int TimelineWidget::replaceMediaPath(const QString &oldPath, const QString &newPath)
{
    if (oldPath.isEmpty() || newPath.isEmpty() || oldPath == newPath
        || !QFileInfo::exists(newPath)) return 0;

    int replaced = 0;
    for (Clip &clip : m_clips) {
        if (clip.path != oldPath) continue;
        clip.path = newPath;
        clip.waveformPeaks.clear();
        if (clip.track >= 3 && QFileInfo(newPath).suffix().compare(
                "wav", Qt::CaseInsensitive) == 0) {
            clip.waveformPeaks = readWaveformPeaks(newPath);
        }
        ++replaced;
        if (clip.id == selectedClipId()) {
            emit clipActivated(clip.path);
            emit clipSelectionChanged(clip.id, clip.path, clip.track);
        }
    }

    if (replaced > 0) {
        update();
        emit timelineEdited();
    }
    return replaced;
}

void TimelineWidget::zoomIn()
{
    m_pixelsPerSecond = qMin(240, m_pixelsPerSecond + 20);
    updateCanvasSize();
    update();
}

void TimelineWidget::zoomOut()
{
    m_pixelsPerSecond = qMax(20, m_pixelsPerSecond - 20);
    updateCanvasSize();
    update();
}

void TimelineWidget::resetZoom()
{
    m_pixelsPerSecond = 80;
    updateCanvasSize();
    update();
}

void TimelineWidget::addMarker()
{
    const qint64 markerTime = m_timebase.snapMilliseconds(m_playheadMs);
    for (const Marker &marker : m_markers) {
        if (marker.timeMs == markerTime) {
            m_playheadMs = marker.timeMs;
            update();
            return;
        }
    }
    Marker marker;
    marker.timeMs = markerTime;
    marker.label = QString("Marker %1").arg(m_markers.size() + 1);
    m_markers.append(marker);
    update();
    emit markersChanged();
    emit timelineEdited();
}

void TimelineWidget::removeNearestMarker()
{
    if (m_markers.isEmpty()) return;
    int nearestIndex = -1;
    qint64 nearestDistance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < m_markers.size(); ++i) {
        const qint64 distance = qAbs(m_markers.at(i).timeMs - m_playheadMs);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }
    const qint64 tolerance = qMax<qint64>(
        frameDurationMs(), qRound64(12.0 * 1000.0 / m_pixelsPerSecond));
    if (nearestIndex >= 0 && nearestDistance <= tolerance) {
        m_markers.removeAt(nearestIndex);
        update();
        emit markersChanged();
        emit timelineEdited();
    }
}

bool TimelineWidget::removeMarkerAtTime(qint64 timeMs)
{
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers.at(i).timeMs == timeMs) {
            m_markers.removeAt(i);
            update();
            emit markersChanged();
            emit timelineEdited();
            return true;
        }
    }
    return false;
}

bool TimelineWidget::isTrackLocked(int track) const
{
    return track >= 0 && track < kTrackCount && m_trackLocked[track];
}

bool TimelineWidget::isTrackMuted(int track) const
{
    return track >= 0 && track < kTrackCount && m_trackMuted[track];
}

QJsonArray TimelineWidget::trackStatesToJson() const
{
    QJsonArray states;
    for (int track = 0; track < kTrackCount; ++track) {
        QJsonObject state;
        state.insert("locked", m_trackLocked[track]);
        state.insert("muted", m_trackMuted[track]);
        states.append(state);
    }
    return states;
}

void TimelineWidget::trackStatesFromJson(const QJsonArray &states)
{
    for (int track = 0; track < kTrackCount; ++track) {
        const QJsonObject state = track < states.size() ? states.at(track).toObject() : QJsonObject{};
        m_trackLocked[track] = state.value("locked").toBool(false);
        m_trackMuted[track] = state.value("muted").toBool(false);
    }
    update();
    for (int track = 0; track < kTrackCount; ++track)
        emit trackStateChanged(track);
}

void TimelineWidget::goToNextMarker()
{
    qint64 target = std::numeric_limits<qint64>::max();
    for (const Marker &marker : m_markers) {
        if (marker.timeMs > m_playheadMs && marker.timeMs < target)
            target = marker.timeMs;
    }
    if (target != std::numeric_limits<qint64>::max())
        setPlayheadMs(target, true);
}

void TimelineWidget::goToPreviousMarker()
{
    qint64 target = -1;
    for (const Marker &marker : m_markers) {
        if (marker.timeMs < m_playheadMs && marker.timeMs > target)
            target = marker.timeMs;
    }
    if (target >= 0)
        setPlayheadMs(target, true);
}

QJsonArray TimelineWidget::markersToJson() const
{
    QJsonArray result;
    for (const Marker &marker : m_markers) {
        QJsonObject item;
        item.insert("timeMs", static_cast<double>(marker.timeMs));
        item.insert("label", marker.label);
        result.append(item);
    }
    return result;
}

void TimelineWidget::markersFromJson(const QJsonArray &markers)
{
    m_markers.clear();
    for (const QJsonValue &value : markers) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        Marker marker;
        marker.timeMs = m_timebase.snapMilliseconds(
            qMax<qint64>(0, static_cast<qint64>(item.value("timeMs").toDouble())));
        marker.label = item.value("label").toString();
        if (marker.label.isEmpty())
            marker.label = QString("Marker %1").arg(m_markers.size() + 1);
        m_markers.append(marker);
    }
    update();
    emit markersChanged();
}

void TimelineWidget::addClip(const QString &path)
{
    if (path.isEmpty()) return;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const QStringList audioExtensions = {
        "mp3", "wav", "aac", "flac", "ogg", "m4a", "aiff", "wma"
    };
    const int targetTrack = audioExtensions.contains(suffix) ? 3 : 0;

    qint64 insertionPoint = 0;
    for (const Clip &clip : m_clips) {
        if (clip.track == targetTrack) {
            insertionPoint = qMax(insertionPoint,
                clip.timelineStartMs + clip.durationMs);
        }
    }

    Clip clip;
    clip.id = m_nextClipId++;
    clip.path = path;
    clip.track = targetTrack;
    clip.timelineStartMs = insertionPoint;
    clip.durationMs = kDefaultClipDurationMs;
    clip.durationKnown = false;
    if (audioExtensions.contains(suffix) && suffix == "wav")
        clip.waveformPeaks = readWaveformPeaks(path);

    m_clips.append(clip);
    m_selectedClip = m_clips.size() - 1;
    m_selectedClipIds.clear();
    m_selectedClipIds.insert(clip.id);
    m_playheadMs = clip.timelineStartMs;
    updateCanvasSize();
    update();

    emit clipActivated(path);
    emit clipSelectionChanged(clip.id, clip.path, clip.track);
    emit playheadChanged(m_playheadMs);
    emit timelineEdited();

    if (QFileInfo::exists(path) && !audioExtensions.contains(suffix)
        && suffix != "png" && suffix != "jpg" && suffix != "jpeg"
        && suffix != "bmp" && suffix != "gif" && suffix != "tif"
        && suffix != "tiff" && suffix != "webp") {
        probeMediaDuration(clip.id, path);
    } else if (audioExtensions.contains(suffix) && QFileInfo::exists(path)) {
        probeMediaDuration(clip.id, path);
    }
}

void TimelineWidget::probeMediaDuration(int clipId, const QString &path)
{
    auto *probe = new QMediaPlayer(this);

    connect(probe, &QMediaPlayer::durationChanged, this,
        [this, probe, clipId](qint64 duration) {
            if (duration <= 0) return;

            const int index = indexForClipId(clipId);
            if (index >= 0) {
                m_clips[index].durationMs = duration;
                m_clips[index].sourceDurationMs = duration;
                m_clips[index].durationKnown = true;
                updateCanvasSize();
                update();
                emit timelineEdited();
            }

            probe->stop();
            probe->deleteLater();
        });

    connect(probe, &QMediaPlayer::errorOccurred, this,
        [probe](QMediaPlayer::Error, const QString &) {
            probe->stop();
            probe->deleteLater();
        });

    QTimer::singleShot(15000, probe, [probe]() {
        probe->stop();
        probe->deleteLater();
    });

    probe->setSource(QUrl::fromLocalFile(path));
}

int TimelineWidget::selectedClipId() const
{
    return m_selectedClip >= 0 && m_selectedClip < m_clips.size()
        ? m_clips.at(m_selectedClip).id : -1;
}

bool TimelineWidget::selectedClipInfo(ClipInfo *result) const
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;
    const Clip &clip = m_clips.at(m_selectedClip);
    if (result) {
        result->id = clip.id;
        result->path = clip.path;
        result->track = clip.track;
        result->timelineStartMs = clip.timelineStartMs;
        result->sourceInMs = clip.sourceInMs;
        result->durationMs = clip.durationMs;
        result->brightness = clip.brightness; result->contrast = clip.contrast;
        result->saturation = clip.saturation; result->shadows = clip.shadows;
        result->midtones = clip.midtones; result->highlights = clip.highlights;
        result->redCurve = clip.redCurve; result->greenCurve = clip.greenCurve;
        result->blueCurve = clip.blueCurve;
        result->scale = clip.scale; result->positionX = clip.positionX;
        result->positionY = clip.positionY;
        result->opacity = clip.opacity;
        result->blendMode = clip.blendMode;
        result->audioVolume = clip.audioVolume;
        result->audioMuted = clip.audioMuted;
        result->fadeInMs = clip.fadeInMs;
        result->fadeOutMs = clip.fadeOutMs;
    }
    return true;
}

bool TimelineWidget::setClipColorSettings(int clipId, int brightness, int contrast, int saturation,
                                              int shadows, int midtones, int highlights,
                                              int redCurve, int greenCurve, int blueCurve)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    clip.brightness = qBound(-100, brightness, 100);
    clip.contrast = qBound(-100, contrast, 100);
    clip.saturation = qBound(0, saturation, 200);
    clip.shadows = qBound(-100, shadows, 100);
    clip.midtones = qBound(-100, midtones, 100);
    clip.highlights = qBound(-100, highlights, 100);
    clip.redCurve = qBound(-100, redCurve, 100);
    clip.greenCurve = qBound(-100, greenCurve, 100);
    clip.blueCurve = qBound(-100, blueCurve, 100);
    emit timelineEdited();
    update();
    return true;
}

bool TimelineWidget::addBrightnessKeyframe(int clipId, qint64 timelineTimeMs, int value)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                            clip.durationMs);
    const qint64 snappedTime = m_timebase.snapMilliseconds(localTime);
    for (Keyframe &keyframe : clip.brightnessKeyframes) {
        if (keyframe.timeMs == snappedTime) {
            keyframe.value = qBound(-100, value, 100);
            update();
            emit timelineEdited();
            return true;
        }
    }
    clip.brightnessKeyframes.append({snappedTime, static_cast<double>(qBound(-100, value, 100))});
    std::sort(clip.brightnessKeyframes.begin(), clip.brightnessKeyframes.end(),
        [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::removeBrightnessKeyframeAt(int clipId, qint64 timelineTimeMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qMax<qint64>(0, timelineTimeMs - clip.timelineStartMs);
    int nearest = -1;
    qint64 distance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < clip.brightnessKeyframes.size(); ++i) {
        const qint64 candidate = qAbs(clip.brightnessKeyframes.at(i).timeMs - localTime);
        if (candidate < distance) { distance = candidate; nearest = i; }
    }
    if (nearest < 0 || distance > qMax<qint64>(80, frameDurationMs() * 2)) return false;
    clip.brightnessKeyframes.removeAt(nearest);
    update();
    emit timelineEdited();
    return true;
}

int TimelineWidget::brightnessKeyframeCount(int clipId) const
{
    const int index = indexForClipId(clipId);
    return index < 0 ? 0 : m_clips.at(index).brightnessKeyframes.size();
}

int TimelineWidget::evaluatedBrightness(int clipId, qint64 timelineTimeMs) const
{
    const int index = indexForClipId(clipId);
    if (index < 0) return 0;
    const Clip &clip = m_clips.at(index);
    if (clip.brightnessKeyframes.isEmpty()) return clip.brightness;
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                            clip.durationMs);
    const auto &frames = clip.brightnessKeyframes;
    if (localTime <= frames.first().timeMs) return qRound(frames.first().value);
    if (localTime >= frames.last().timeMs) return qRound(frames.last().value);
    for (int i = 1; i < frames.size(); ++i) {
        const Keyframe &right = frames.at(i);
        const Keyframe &left = frames.at(i - 1);
        if (localTime <= right.timeMs) {
            const double span = static_cast<double>(right.timeMs - left.timeMs);
            const double t = span <= 0.0 ? 0.0
                : static_cast<double>(localTime - left.timeMs) / span;
            return qBound(-100, qRound(left.value + (right.value - left.value) * t), 100);
        }
    }
    return clip.brightness;
}

bool TimelineWidget::addContrastKeyframe(int clipId, qint64 timelineTimeMs, int value)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                            clip.durationMs);
    const qint64 snappedTime = m_timebase.snapMilliseconds(localTime);
    for (Keyframe &keyframe : clip.contrastKeyframes) {
        if (keyframe.timeMs == snappedTime) {
            keyframe.value = qBound(-100, value, 100);
            update();
            emit timelineEdited();
            return true;
        }
    }
    clip.contrastKeyframes.append({snappedTime, static_cast<double>(qBound(-100, value, 100))});
    std::sort(clip.contrastKeyframes.begin(), clip.contrastKeyframes.end(),
        [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::removeContrastKeyframeAt(int clipId, qint64 timelineTimeMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qMax<qint64>(0, timelineTimeMs - clip.timelineStartMs);
    int nearest = -1;
    qint64 distance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < clip.contrastKeyframes.size(); ++i) {
        const qint64 candidate = qAbs(clip.contrastKeyframes.at(i).timeMs - localTime);
        if (candidate < distance) { distance = candidate; nearest = i; }
    }
    if (nearest < 0 || distance > qMax<qint64>(80, frameDurationMs() * 2)) return false;
    clip.contrastKeyframes.removeAt(nearest);
    update();
    emit timelineEdited();
    return true;
}

int TimelineWidget::contrastKeyframeCount(int clipId) const
{
    const int index = indexForClipId(clipId);
    return index < 0 ? 0 : m_clips.at(index).contrastKeyframes.size();
}

int TimelineWidget::evaluatedContrast(int clipId, qint64 timelineTimeMs) const
{
    const int index = indexForClipId(clipId);
    if (index < 0) return 0;
    const Clip &clip = m_clips.at(index);
    if (clip.contrastKeyframes.isEmpty()) return clip.contrast;
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                            clip.durationMs);
    const auto &frames = clip.contrastKeyframes;
    if (localTime <= frames.first().timeMs) return qRound(frames.first().value);
    if (localTime >= frames.last().timeMs) return qRound(frames.last().value);
    for (int i = 1; i < frames.size(); ++i) {
        const Keyframe &right = frames.at(i);
        const Keyframe &left = frames.at(i - 1);
        if (localTime <= right.timeMs) {
            const double span = static_cast<double>(right.timeMs - left.timeMs);
            const double t = span <= 0.0 ? 0.0
                : static_cast<double>(localTime - left.timeMs) / span;
            return qBound(-100, qRound(left.value + (right.value - left.value) * t), 100);
        }
    }
    return clip.contrast;
}





bool TimelineWidget::setClipBlendMode(int clipId, int blendMode)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    m_clips[index].blendMode = qBound(0, blendMode, 3);
    update();
    emit timelineEdited();
    return true;
}


bool TimelineWidget::setClipOpacity(int clipId, int opacity)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    m_clips[index].opacity = qBound(0, opacity, 100);
    update(); emit timelineEdited(); return true;
}
bool TimelineWidget::setClipAudioSettings(int clipId, int volume, int fadeInMs, int fadeOutMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    clip.audioVolume = qBound(0, volume, 100);
    clip.fadeInMs = qBound(0, fadeInMs, 5000);
    clip.fadeOutMs = qBound(0, fadeOutMs, 5000);
    emit timelineEdited();
    update();
    return true;
}

bool TimelineWidget::setClipAudioMuted(int clipId, bool muted)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    if (m_clips[index].audioMuted == muted) return true;
    m_clips[index].audioMuted = muted;
    emit timelineEdited();
    update();
    return true;
}

bool TimelineWidget::addAudioVolumeKeyframe(int clipId, qint64 timelineTimeMs, int volume)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = m_timebase.snapMilliseconds(qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs, clip.durationMs));
    const double value = qBound(0, volume, 100);
    for (Keyframe &keyframe : clip.audioVolumeKeyframes) {
        if (keyframe.timeMs == localTime) { keyframe.value = value; update(); emit timelineEdited(); return true; }
    }
    clip.audioVolumeKeyframes.append({localTime, value});
    std::sort(clip.audioVolumeKeyframes.begin(), clip.audioVolumeKeyframes.end(), [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
    update(); emit timelineEdited(); return true;
}

bool TimelineWidget::removeAudioVolumeKeyframeAt(int clipId, qint64 timelineTimeMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qMax<qint64>(0, timelineTimeMs - clip.timelineStartMs);
    int nearest = -1; qint64 distance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < clip.audioVolumeKeyframes.size(); ++i) {
        const qint64 candidate = qAbs(clip.audioVolumeKeyframes.at(i).timeMs - localTime);
        if (candidate < distance) { distance = candidate; nearest = i; }
    }
    if (nearest < 0 || distance > qMax<qint64>(80, frameDurationMs() * 2)) return false;
    clip.audioVolumeKeyframes.removeAt(nearest); update(); emit timelineEdited(); return true;
}

int TimelineWidget::audioVolumeKeyframeCount(int clipId) const
{
    const int index = indexForClipId(clipId);
    return index < 0 ? 0 : m_clips.at(index).audioVolumeKeyframes.size();
}

int TimelineWidget::evaluatedAudioVolume(int clipId, qint64 timelineTimeMs) const
{
    const int index = indexForClipId(clipId);
    if (index < 0) return 100;
    const Clip &clip = m_clips.at(index);
    const auto &frames = clip.audioVolumeKeyframes;
    if (frames.isEmpty()) return clip.audioVolume;
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs, clip.durationMs);
    if (localTime <= frames.first().timeMs) return qRound(frames.first().value);
    if (localTime >= frames.last().timeMs) return qRound(frames.last().value);
    for (int i = 1; i < frames.size(); ++i) {
        const Keyframe &right = frames.at(i), &left = frames.at(i - 1);
        if (localTime <= right.timeMs) {
            const double span = static_cast<double>(right.timeMs - left.timeMs);
            const double t = span <= 0.0 ? 0.0 : static_cast<double>(localTime - left.timeMs) / span;
            return qBound(0, qRound(left.value + (right.value - left.value) * t), 100);
        }
    }
    return clip.audioVolume;
}

bool TimelineWidget::addOpacityKeyframe(int clipId, qint64 timelineTimeMs, int opacity)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs, clip.durationMs);
    const qint64 snappedTime = m_timebase.snapMilliseconds(localTime);
    for (Keyframe &keyframe : clip.opacityKeyframes) {
        if (keyframe.timeMs == snappedTime) { keyframe.value = qBound(0, opacity, 100); update(); emit timelineEdited(); return true; }
    }
    clip.opacityKeyframes.append({snappedTime, static_cast<double>(qBound(0, opacity, 100))});
    std::sort(clip.opacityKeyframes.begin(), clip.opacityKeyframes.end(), [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
    update(); emit timelineEdited(); return true;
}
bool TimelineWidget::removeOpacityKeyframeAt(int clipId, qint64 timelineTimeMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qMax<qint64>(0, timelineTimeMs - clip.timelineStartMs);
    int nearest = -1; qint64 distance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < clip.opacityKeyframes.size(); ++i) {
        const qint64 candidate = qAbs(clip.opacityKeyframes.at(i).timeMs - localTime);
        if (candidate < distance) { distance = candidate; nearest = i; }
    }
    if (nearest < 0 || distance > qMax<qint64>(80, frameDurationMs() * 2)) return false;
    clip.opacityKeyframes.removeAt(nearest); update(); emit timelineEdited(); return true;
}
int TimelineWidget::opacityKeyframeCount(int clipId) const
{
    const int index = indexForClipId(clipId);
    return index < 0 ? 0 : m_clips.at(index).opacityKeyframes.size();
}
int TimelineWidget::evaluatedOpacity(int clipId, qint64 timelineTimeMs) const
{
    const int index = indexForClipId(clipId);
    if (index < 0) return 100;
    const Clip &clip = m_clips.at(index);
    const auto &frames = clip.opacityKeyframes;
    if (frames.isEmpty()) return clip.opacity;
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs, clip.durationMs);
    if (localTime <= frames.first().timeMs) return qRound(frames.first().value);
    if (localTime >= frames.last().timeMs) return qRound(frames.last().value);
    for (int i = 1; i < frames.size(); ++i) {
        const Keyframe &right = frames.at(i), &left = frames.at(i - 1);
        if (localTime <= right.timeMs) {
            const double span = static_cast<double>(right.timeMs - left.timeMs);
            const double t = span <= 0.0 ? 0.0 : static_cast<double>(localTime - left.timeMs) / span;
            return qBound(0, qRound(left.value + (right.value - left.value) * t), 100);
        }
    }
    return clip.opacity;
}

bool TimelineWidget::setClipTransformSettings(int clipId, int scale, int positionX, int positionY)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    clip.scale = qBound(10, scale, 300);
    clip.positionX = qBound(-100, positionX, 100);
    clip.positionY = qBound(-100, positionY, 100);
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::addTransformKeyframe(int clipId, qint64 timelineTimeMs,
                                           int scale, int positionX, int positionY)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                            clip.durationMs);
    const qint64 snappedTime = m_timebase.snapMilliseconds(localTime);
    for (Clip::TransformKeyframe &keyframe : clip.transformKeyframes) {
        if (keyframe.timeMs == snappedTime) {
            keyframe.scale = qBound(10, scale, 300);
            keyframe.x = qBound(-100, positionX, 100);
            keyframe.y = qBound(-100, positionY, 100);
            update(); emit timelineEdited(); return true;
        }
    }
    clip.transformKeyframes.append({snappedTime, static_cast<double>(qBound(10, scale, 300)),
        static_cast<double>(qBound(-100, positionX, 100)),
        static_cast<double>(qBound(-100, positionY, 100))});
    std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
        [](const Clip::TransformKeyframe &a, const Clip::TransformKeyframe &b) {
            return a.timeMs < b.timeMs;
        });
    update(); emit timelineEdited(); return true;
}

bool TimelineWidget::removeTransformKeyframeAt(int clipId, qint64 timelineTimeMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0) return false;
    Clip &clip = m_clips[index];
    const qint64 localTime = qMax<qint64>(0, timelineTimeMs - clip.timelineStartMs);
    int nearest = -1;
    qint64 distance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < clip.transformKeyframes.size(); ++i) {
        const qint64 candidate = qAbs(clip.transformKeyframes.at(i).timeMs - localTime);
        if (candidate < distance) { distance = candidate; nearest = i; }
    }
    if (nearest < 0 || distance > qMax<qint64>(80, frameDurationMs() * 2)) return false;
    clip.transformKeyframes.removeAt(nearest);
    update(); emit timelineEdited(); return true;
}

int TimelineWidget::transformKeyframeCount(int clipId) const
{
    const int index = indexForClipId(clipId);
    return index < 0 ? 0 : m_clips.at(index).transformKeyframes.size();
}

void TimelineWidget::evaluatedTransform(int clipId, qint64 timelineTimeMs,
                                         int *scale, int *positionX, int *positionY) const
{
    const int index = indexForClipId(clipId);
    if (index < 0) return;
    const Clip &clip = m_clips.at(index);
    double s = clip.scale, x = clip.positionX, y = clip.positionY;
    const auto &frames = clip.transformKeyframes;
    if (!frames.isEmpty()) {
        const qint64 localTime = qBound<qint64>(0, timelineTimeMs - clip.timelineStartMs,
                                                clip.durationMs);
        const Clip::TransformKeyframe *left = &frames.first();
        const Clip::TransformKeyframe *right = &frames.last();
        if (localTime <= frames.first().timeMs) right = left;
        else if (localTime >= frames.last().timeMs) left = right;
        else {
            for (int i = 1; i < frames.size(); ++i) {
                if (localTime <= frames.at(i).timeMs) {
                    left = &frames.at(i - 1); right = &frames.at(i); break;
                }
            }
        }
        const double span = static_cast<double>(right->timeMs - left->timeMs);
        const double t = span <= 0.0 ? 0.0
            : qBound(0.0, static_cast<double>(localTime - left->timeMs) / span, 1.0);
        s = left->scale + (right->scale - left->scale) * t;
        x = left->x + (right->x - left->x) * t;
        y = left->y + (right->y - left->y) * t;
    }
    if (scale) *scale = qBound(10, qRound(s), 300);
    if (positionX) *positionX = qBound(-100, qRound(x), 100);
    if (positionY) *positionY = qBound(-100, qRound(y), 100);
}

bool TimelineWidget::clipAtTime(qint64 positionMs, ClipInfo *result) const
{
    int bestIndex = -1;
    int bestTrack = -1;

    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip &clip = m_clips.at(i);
        if (clip.track >= 3) continue;

        const qint64 clipEnd = clip.timelineStartMs + clip.durationMs;
        if (positionMs >= clip.timelineStartMs && positionMs < clipEnd
            && clip.track > bestTrack) {
            bestIndex = i;
            bestTrack = clip.track;
        }
    }

    if (bestIndex < 0) return false;

    const Clip &clip = m_clips.at(bestIndex);
    if (result) {
        result->id = clip.id;
        result->path = clip.path;
        result->track = clip.track;
        result->timelineStartMs = clip.timelineStartMs;
        result->sourceInMs = clip.sourceInMs;
        result->durationMs = clip.durationMs;
        result->brightness = clip.brightness; result->contrast = clip.contrast;
        result->saturation = clip.saturation; result->shadows = clip.shadows;
        result->midtones = clip.midtones; result->highlights = clip.highlights;
        result->redCurve = clip.redCurve; result->greenCurve = clip.greenCurve;
        result->blueCurve = clip.blueCurve;
        result->scale = clip.scale; result->positionX = clip.positionX;
        result->positionY = clip.positionY;
    }
    return true;
}

bool TimelineWidget::clipAtTimeOnTrack(qint64 positionMs, int track, ClipInfo *result) const
{
    if (track < 0 || track >= kTrackCount) return false;

    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip &clip = m_clips.at(i);
        if (clip.track != track) continue;

        const qint64 clipEnd = clip.timelineStartMs + clip.durationMs;
        if (positionMs >= clip.timelineStartMs && positionMs < clipEnd) {
            if (result) {
                result->id = clip.id;
                result->path = clip.path;
                result->track = clip.track;
                result->timelineStartMs = clip.timelineStartMs;
                result->sourceInMs = clip.sourceInMs;
                result->durationMs = clip.durationMs;
        result->brightness = clip.brightness; result->contrast = clip.contrast;
        result->saturation = clip.saturation; result->shadows = clip.shadows;
        result->midtones = clip.midtones; result->highlights = clip.highlights;
        result->redCurve = clip.redCurve; result->greenCurve = clip.greenCurve;
        result->blueCurve = clip.blueCurve;
        result->scale = clip.scale; result->positionX = clip.positionX;
        result->positionY = clip.positionY;
            }
            return true;
        }
    }
    return false;
}

bool TimelineWidget::nextVideoClipAtOrAfter(qint64 positionMs, ClipInfo *result) const
{
    int bestIndex = -1;
    qint64 bestStart = std::numeric_limits<qint64>::max();

    for (int i = 0; i < m_clips.size(); ++i) {
        const Clip &clip = m_clips.at(i);
        if (clip.track >= 3 || clip.timelineStartMs < positionMs) continue;

        if (clip.timelineStartMs < bestStart) {
            bestIndex = i;
            bestStart = clip.timelineStartMs;
        }
    }

    if (bestIndex < 0) return false;

    const Clip &clip = m_clips.at(bestIndex);
    if (result) {
        result->id = clip.id;
        result->path = clip.path;
        result->track = clip.track;
        result->timelineStartMs = clip.timelineStartMs;
        result->sourceInMs = clip.sourceInMs;
        result->durationMs = clip.durationMs;
        result->brightness = clip.brightness; result->contrast = clip.contrast;
        result->saturation = clip.saturation; result->shadows = clip.shadows;
        result->midtones = clip.midtones; result->highlights = clip.highlights;
        result->redCurve = clip.redCurve; result->greenCurve = clip.greenCurve;
        result->blueCurve = clip.blueCurve;
        result->scale = clip.scale; result->positionX = clip.positionX;
        result->positionY = clip.positionY;
    }
    return true;
}

qint64 TimelineWidget::sequenceDurationMs() const
{
    qint64 result = 0;
    for (const Clip &clip : m_clips)
        result = qMax(result, clip.timelineStartMs + clip.durationMs);
    return result;
}

qint64 TimelineWidget::playheadMs() const
{
    return m_playheadMs;
}

void TimelineWidget::setPlayheadMs(qint64 positionMs, bool notify)
{
    m_playheadMs = m_timebase.snapMilliseconds(qMax<qint64>(0, positionMs));
    update();
    if (notify) emit playheadChanged(m_playheadMs);
}

int TimelineWidget::indexForClipId(int clipId) const
{
    for (int i = 0; i < m_clips.size(); ++i) {
        if (m_clips.at(i).id == clipId) return i;
    }
    return -1;
}

void TimelineWidget::clearClips()
{
    m_clips.clear();
    m_markers.clear();
    emit markersChanged();
    m_selectedClip = -1;
    m_nextClipId = 1;
    m_playheadMs = 0;
    m_dragMode = DragMode::None;
    updateCanvasSize();
    update();
    emit playheadChanged(0);
    emit timelineEdited();
}

bool TimelineWidget::splitSelectedClipAtPlayhead()
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;

    const Clip original = m_clips.at(m_selectedClip);
    if (isTrackLocked(original.track)) return false;
    const qint64 clipEnd = original.timelineStartMs + original.durationMs;
    if (m_playheadMs <= original.timelineStartMs + kMinimumClipDurationMs
        || m_playheadMs >= clipEnd - kMinimumClipDurationMs) {
        return false;
    }

    const qint64 leftDuration = m_playheadMs - original.timelineStartMs;
    Clip right = original;
    right.id = m_nextClipId++;
    right.timelineStartMs = m_playheadMs;
    right.sourceInMs += leftDuration;
    right.durationMs -= leftDuration;

    m_clips[m_selectedClip].durationMs = leftDuration;
    m_clips.insert(m_selectedClip + 1, right);
    ++m_selectedClip;

    updateCanvasSize();
    update();
    emit timelineEdited();
    return true;
}

int TimelineWidget::splitAllClipsAtPlayhead()
{
    const qint64 splitTime = m_timebase.snapMilliseconds(
        qMax<qint64>(0, m_playheadMs));
    int splitCount = 0;
    int lastRightClipId = -1;

    // Iterate backwards so inserting right-hand segments never invalidates
    // the indices of clips that are still waiting to be examined.
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        const Clip original = m_clips.at(i);
        if (isTrackLocked(original.track)) continue;

        const qint64 clipEnd = original.timelineStartMs + original.durationMs;
        if (splitTime <= original.timelineStartMs + kMinimumClipDurationMs
            || splitTime >= clipEnd - kMinimumClipDurationMs) continue;

        const qint64 leftDuration = splitTime - original.timelineStartMs;
        Clip right = original;
        right.id = m_nextClipId++;
        right.timelineStartMs = splitTime;
        right.sourceInMs += leftDuration;
        right.durationMs -= leftDuration;

        m_clips[i].durationMs = leftDuration;
        m_clips.insert(i + 1, right);
        lastRightClipId = right.id;
        ++splitCount;
    }

    if (splitCount == 0) return 0;

    m_selectedClip = indexForClipId(lastRightClipId);
    updateCanvasSize();
    update();
    emit timelineEdited();
    return splitCount;
}

bool TimelineWidget::deleteSelectedClip()
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;

    if (m_selectedClipIds.size() > 1) {
        // Treat a multi-delete as one operation: do not partially delete a
        // selection that includes a clip on a locked track.
        for (const Clip &clip : m_clips) {
            if (m_selectedClipIds.contains(clip.id) && isTrackLocked(clip.track))
                return false;
        }
        for (int i = m_clips.size() - 1; i >= 0; --i) {
            if (m_selectedClipIds.contains(m_clips.at(i).id))
                m_clips.removeAt(i);
        }
        m_selectedClip = -1;
        m_selectedClipIds.clear();
        m_dragGroupSnapshot.clear();
    } else {
        if (isTrackLocked(m_clips.at(m_selectedClip).track)) return false;
        m_selectedClipIds.clear();
        m_clips.removeAt(m_selectedClip);
        if (m_clips.isEmpty()) m_selectedClip = -1;
        else m_selectedClip = qMin(m_selectedClip, m_clips.size() - 1);
    }

    updateCanvasSize();
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::rippleDeleteSelectedClip()
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;

    QSet<int> ids = m_selectedClipIds;
    if (ids.isEmpty()) ids.insert(m_clips.at(m_selectedClip).id);

    qint64 rippleStart = std::numeric_limits<qint64>::max();
    qint64 rippleEnd = 0;
    for (const Clip &clip : m_clips) {
        if (!ids.contains(clip.id)) continue;
        if (isTrackLocked(clip.track)) return false;
        rippleStart = qMin(rippleStart, clip.timelineStartMs);
        rippleEnd = qMax(rippleEnd, clip.timelineStartMs + clip.durationMs);
    }
    if (rippleStart == std::numeric_limits<qint64>::max()
        || rippleEnd <= rippleStart) return false;

    const qint64 rippleDuration = rippleEnd - rippleStart;
    // Ripple is an all-or-nothing edit: never leave locked clips out of sync.
    for (const Clip &clip : m_clips) {
        if (!ids.contains(clip.id) && clip.timelineStartMs >= rippleEnd
            && isTrackLocked(clip.track)) return false;
    }

    for (int i = m_clips.size() - 1; i >= 0; --i) {
        if (ids.contains(m_clips.at(i).id))
            m_clips.removeAt(i);
    }
    for (Clip &clip : m_clips) {
        if (clip.timelineStartMs >= rippleEnd)
            clip.timelineStartMs = qMax<qint64>(0,
                m_timebase.snapMilliseconds(clip.timelineStartMs - rippleDuration));
    }

    m_selectedClip = -1;
    m_selectedClipIds.clear();
    m_dragGroupSnapshot.clear();
    m_playheadMs = rippleStart;
    updateCanvasSize();
    update();
    emit playheadChanged(m_playheadMs);
    emit timelineEdited();
    return true;
}

bool TimelineWidget::slipClip(int clipId, qint64 deltaMs)
{
    const int index = indexForClipId(clipId);
    if (index < 0 || deltaMs == 0) return false;
    Clip &clip = m_clips[index];
    if (!clip.durationKnown || clip.sourceDurationMs <= 0) return false;
    const qint64 snappedDelta = deltaMs < 0
        ? -m_timebase.snapMilliseconds(-deltaMs)
        : m_timebase.snapMilliseconds(deltaMs);
    const qint64 maximumSourceIn = qMax<qint64>(0, clip.sourceDurationMs - clip.durationMs);
    const qint64 nextSourceIn = qBound<qint64>(0, clip.sourceInMs + snappedDelta,
                                               maximumSourceIn);
    if (nextSourceIn == clip.sourceInMs) return false;
    clip.sourceInMs = nextSourceIn;
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::nudgeSelectedClip(int frameDirection)
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()
        || frameDirection == 0) return false;

    QSet<int> ids = m_selectedClipIds;
    if (ids.isEmpty()) ids.insert(m_clips.at(m_selectedClip).id);

    for (const Clip &clip : m_clips) {
        if (ids.contains(clip.id) && isTrackLocked(clip.track))
            return false;
    }

    qint64 delta = frameDirection < 0 ? -frameDurationMs() : frameDurationMs();
    if (delta < 0) {
        qint64 earliestStart = std::numeric_limits<qint64>::max();
        for (const Clip &clip : m_clips) {
            if (ids.contains(clip.id))
                earliestStart = qMin(earliestStart, clip.timelineStartMs);
        }
        if (earliestStart == std::numeric_limits<qint64>::max()) return false;
        delta = qMax<qint64>(delta, -earliestStart);
    }

    if (delta == 0) return false;
    for (Clip &clip : m_clips) {
        if (ids.contains(clip.id))
            clip.timelineStartMs = m_timebase.snapMilliseconds(
                qMax<qint64>(0, clip.timelineStartMs + delta));
    }

    updateCanvasSize();
    update();
    emit timelineEdited();
    return true;
}

bool TimelineWidget::duplicateSelectedClip()
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;

    QSet<int> ids = m_selectedClipIds;
    if (ids.isEmpty()) ids.insert(m_clips.at(m_selectedClip).id);

    QVector<Clip> originals;
    qint64 groupStart = std::numeric_limits<qint64>::max();
    qint64 groupEnd = 0;
    for (const Clip &clip : m_clips) {
        if (!ids.contains(clip.id)) continue;
        if (isTrackLocked(clip.track)) return false;
        originals.append(clip);
        groupStart = qMin(groupStart, clip.timelineStartMs);
        groupEnd = qMax(groupEnd, clip.timelineStartMs + clip.durationMs);
    }
    if (originals.isEmpty()) return false;

    const qint64 offset = m_timebase.snapMilliseconds(groupEnd - groupStart);
    QVector<Clip> duplicates;
    duplicates.reserve(originals.size());
    for (const Clip &original : originals) {
        Clip duplicate = original;
        duplicate.id = m_nextClipId++;
        duplicate.timelineStartMs = m_timebase.snapMilliseconds(
            original.timelineStartMs + offset);
        duplicates.append(duplicate);
    }

    m_selectedClipIds.clear();
    for (const Clip &duplicate : duplicates) {
        m_clips.append(duplicate);
        m_selectedClipIds.insert(duplicate.id);
    }
    m_selectedClip = m_clips.size() - 1;
    const Clip &lastDuplicate = m_clips.at(m_selectedClip);
    m_playheadMs = lastDuplicate.timelineStartMs;

    updateCanvasSize();
    update();
    emit playheadChanged(m_playheadMs);
    emit clipActivated(lastDuplicate.path);
    emit clipSelectionChanged(lastDuplicate.id, lastDuplicate.path, lastDuplicate.track);
    emit timelineEdited();
    return true;
}

QJsonArray TimelineWidget::toJson() const
{
    QJsonArray result;
    for (const Clip &clip : m_clips) {
        QJsonObject item;
        item.insert("id", clip.id);
        item.insert("path", clip.path);
        item.insert("track", clip.track);
        item.insert("timelineStartMs", static_cast<double>(clip.timelineStartMs));
        item.insert("sourceInMs", static_cast<double>(clip.sourceInMs));
        item.insert("durationMs", static_cast<double>(clip.durationMs));
        item.insert("durationKnown", clip.durationKnown);
        item.insert("sourceDurationMs", static_cast<double>(clip.sourceDurationMs));
        QJsonObject color;
        color.insert("brightness", clip.brightness);
        color.insert("contrast", clip.contrast);
        color.insert("saturation", clip.saturation);
        color.insert("shadows", clip.shadows);
        color.insert("midtones", clip.midtones);
        color.insert("highlights", clip.highlights);
        color.insert("redCurve", clip.redCurve);
        color.insert("greenCurve", clip.greenCurve);
        color.insert("blueCurve", clip.blueCurve);
        item.insert("color", color);
        QJsonArray keyframes;
        for (const Keyframe &keyframe : clip.brightnessKeyframes) {
            QJsonObject frame;
            frame.insert("timeMs", static_cast<double>(keyframe.timeMs));
            frame.insert("value", keyframe.value);
            keyframes.append(frame);
        }
        item.insert("brightnessKeyframes", keyframes);
        QJsonArray contrastKeyframes;
        for (const Keyframe &keyframe : clip.contrastKeyframes) {
            QJsonObject frame;
            frame.insert("timeMs", static_cast<double>(keyframe.timeMs));
            frame.insert("value", keyframe.value);
            contrastKeyframes.append(frame);
        }
        item.insert("contrastKeyframes", contrastKeyframes);
        item.insert("scale", clip.scale);
        item.insert("positionX", clip.positionX);
        item.insert("positionY", clip.positionY);
        item.insert("opacity", clip.opacity);
        item.insert("blendMode", clip.blendMode);
        item.insert("audioVolume", clip.audioVolume);
        item.insert("audioMuted", clip.audioMuted);
        item.insert("fadeInMs", clip.fadeInMs);
        item.insert("fadeOutMs", clip.fadeOutMs);
        QJsonArray audioVolumeKeyframes;
        for (const Keyframe &keyframe : clip.audioVolumeKeyframes) {
            QJsonObject frame; frame.insert("timeMs", static_cast<double>(keyframe.timeMs));
            frame.insert("value", keyframe.value); audioVolumeKeyframes.append(frame);
        }
        item.insert("audioVolumeKeyframes", audioVolumeKeyframes);
        QJsonArray opacityKeyframes;
        for (const Keyframe &keyframe : clip.opacityKeyframes) {
            QJsonObject frame; frame.insert("timeMs", static_cast<double>(keyframe.timeMs));
            frame.insert("value", keyframe.value); opacityKeyframes.append(frame);
        }
        item.insert("opacityKeyframes", opacityKeyframes);
        QJsonArray transformKeyframes;
        for (const Clip::TransformKeyframe &keyframe : clip.transformKeyframes) {
            QJsonObject frame;
            frame.insert("timeMs", static_cast<double>(keyframe.timeMs));
            frame.insert("scale", keyframe.scale);
            frame.insert("x", keyframe.x);
            frame.insert("y", keyframe.y);
            transformKeyframes.append(frame);
        }
        item.insert("transformKeyframes", transformKeyframes);
        result.append(item);
    }
    return result;
}

void TimelineWidget::fromJson(const QJsonArray &clips)
{
    m_clips.clear();
    m_selectedClip = -1;
    m_nextClipId = 1;
    m_playheadMs = 0;

    qint64 legacyTrackEnd[kTrackCount] = {0, 0, 0, 0, 0};

    for (const QJsonValue &value : clips) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const QString path = item.value("path").toString();
        const int track = item.value("track").toInt(0);
        if (path.isEmpty() || track < 0 || track >= kTrackCount) continue;

        Clip clip;
        clip.id = item.value("id").toInt(m_nextClipId);
        if (clip.id <= 0) clip.id = m_nextClipId;
        m_nextClipId = qMax(m_nextClipId, clip.id + 1);
        clip.path = path;
        clip.track = track;
        if (track >= 3 && QFileInfo(path).suffix().compare(
                "wav", Qt::CaseInsensitive) == 0 && QFileInfo::exists(path)) {
            clip.waveformPeaks = readWaveformPeaks(path);
        }
        clip.timelineStartMs = item.contains("timelineStartMs")
            ? static_cast<qint64>(item.value("timelineStartMs").toDouble())
            : legacyTrackEnd[track];
        clip.sourceInMs = static_cast<qint64>(item.value("sourceInMs").toDouble());
        clip.durationMs = item.contains("durationMs")
            ? static_cast<qint64>(item.value("durationMs").toDouble())
            : kDefaultClipDurationMs;
        clip.durationMs = qMax(kMinimumClipDurationMs, clip.durationMs);
        clip.durationKnown = item.value("durationKnown").toBool(false);
        clip.sourceDurationMs = qMax<qint64>(0,
            static_cast<qint64>(item.value("sourceDurationMs").toDouble()));
        if (clip.durationKnown && clip.sourceDurationMs <= 0)
            clip.sourceDurationMs = clip.sourceInMs + clip.durationMs;
        const QJsonObject color = item.value("color").toObject();
        clip.brightness = color.value("brightness").toInt(0);
        clip.contrast = color.value("contrast").toInt(0);
        clip.saturation = color.value("saturation").toInt(100);
        clip.shadows = color.value("shadows").toInt(0);
        clip.midtones = color.value("midtones").toInt(0);
        clip.highlights = color.value("highlights").toInt(0);
        clip.redCurve = color.value("redCurve").toInt(0);
        clip.greenCurve = color.value("greenCurve").toInt(0);
        clip.blueCurve = color.value("blueCurve").toInt(0);
        const QJsonArray keyframes = item.value("brightnessKeyframes").toArray();
        for (const QJsonValue &frameValue : keyframes) {
            if (!frameValue.isObject()) continue;
            const QJsonObject frame = frameValue.toObject();
            Keyframe keyframe;
            keyframe.timeMs = qBound<qint64>(0,
                m_timebase.snapMilliseconds(static_cast<qint64>(frame.value("timeMs").toDouble())),
                clip.durationMs);
            keyframe.value = qBound(-100.0, frame.value("value").toDouble(), 100.0);
            clip.brightnessKeyframes.append(keyframe);
        }
        std::sort(clip.brightnessKeyframes.begin(), clip.brightnessKeyframes.end(),
            [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
        const QJsonArray contrastKeyframes = item.value("contrastKeyframes").toArray();
        for (const QJsonValue &frameValue : contrastKeyframes) {
            if (!frameValue.isObject()) continue;
            const QJsonObject frame = frameValue.toObject();
            Keyframe keyframe;
            keyframe.timeMs = qBound<qint64>(0,
                m_timebase.snapMilliseconds(static_cast<qint64>(frame.value("timeMs").toDouble())),
                clip.durationMs);
            keyframe.value = qBound(-100.0, frame.value("value").toDouble(), 100.0);
            clip.contrastKeyframes.append(keyframe);
        }
        std::sort(clip.contrastKeyframes.begin(), clip.contrastKeyframes.end(),
            [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
        clip.scale = qBound(10, item.value("scale").toInt(100), 300);
        clip.positionX = qBound(-100, item.value("positionX").toInt(0), 100);
        clip.positionY = qBound(-100, item.value("positionY").toInt(0), 100);
        clip.opacity = qBound(0, item.value("opacity").toInt(100), 100);
        clip.blendMode = qBound(0, item.value("blendMode").toInt(0), 3);
        clip.audioVolume = qBound(0, item.value("audioVolume").toInt(100), 100);
        clip.audioMuted = item.value("audioMuted").toBool(false);
        clip.fadeInMs = qBound(0, item.value("fadeInMs").toInt(0), 5000);
        clip.fadeOutMs = qBound(0, item.value("fadeOutMs").toInt(0), 5000);
        const QJsonArray audioVolumeKeyframes = item.value("audioVolumeKeyframes").toArray();
        for (const QJsonValue &frameValue : audioVolumeKeyframes) {
            if (!frameValue.isObject()) continue;
            const QJsonObject frame = frameValue.toObject();
            Keyframe keyframe;
            keyframe.timeMs = qBound<qint64>(0, m_timebase.snapMilliseconds(static_cast<qint64>(frame.value("timeMs").toDouble())), clip.durationMs);
            keyframe.value = qBound(0.0, frame.value("value").toDouble(100.0), 100.0);
            clip.audioVolumeKeyframes.append(keyframe);
        }
        std::sort(clip.audioVolumeKeyframes.begin(), clip.audioVolumeKeyframes.end(), [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
        const QJsonArray opacityKeyframes = item.value("opacityKeyframes").toArray();
        for (const QJsonValue &frameValue : opacityKeyframes) {
            if (!frameValue.isObject()) continue;
            const QJsonObject frame = frameValue.toObject();
            Keyframe keyframe;
            keyframe.timeMs = qBound<qint64>(0, m_timebase.snapMilliseconds(static_cast<qint64>(frame.value("timeMs").toDouble())), clip.durationMs);
            keyframe.value = qBound(0.0, frame.value("value").toDouble(100.0), 100.0);
            clip.opacityKeyframes.append(keyframe);
        }
        std::sort(clip.opacityKeyframes.begin(), clip.opacityKeyframes.end(), [](const Keyframe &a, const Keyframe &b) { return a.timeMs < b.timeMs; });
        const QJsonArray transformKeyframes = item.value("transformKeyframes").toArray();
        for (const QJsonValue &frameValue : transformKeyframes) {
            if (!frameValue.isObject()) continue;
            const QJsonObject frame = frameValue.toObject();
            Clip::TransformKeyframe keyframe;
            keyframe.timeMs = qBound<qint64>(0,
                m_timebase.snapMilliseconds(static_cast<qint64>(frame.value("timeMs").toDouble())),
                clip.durationMs);
            keyframe.scale = qBound(10.0, frame.value("scale").toDouble(100.0), 300.0);
            keyframe.x = qBound(-100.0, frame.value("x").toDouble(0.0), 100.0);
            keyframe.y = qBound(-100.0, frame.value("y").toDouble(0.0), 100.0);
            clip.transformKeyframes.append(keyframe);
        }
        std::sort(clip.transformKeyframes.begin(), clip.transformKeyframes.end(),
            [](const Clip::TransformKeyframe &a, const Clip::TransformKeyframe &b) {
                return a.timeMs < b.timeMs;
            });



        legacyTrackEnd[track] = qMax(legacyTrackEnd[track],
            clip.timelineStartMs + clip.durationMs);
        m_clips.append(clip);
    }

    if (!m_clips.isEmpty()) m_selectedClip = 0;
    updateCanvasSize();
    update();
}

QRect TimelineWidget::clipRect(int index) const
{
    if (index < 0 || index >= m_clips.size()) return {};

    const Clip &clip = m_clips.at(index);
    const int x = kTimelineStartX
        + qRound(static_cast<double>(clip.timelineStartMs) * m_pixelsPerSecond / 1000.0);
    const int y = kTrackTop + clip.track * (kTrackHeight + kTrackGap);
    const int width = qMax(kClipMinimumWidth,
        qRound(static_cast<double>(clip.durationMs) * m_pixelsPerSecond / 1000.0));
    return QRect(x, y, width, kTrackHeight);
}

int TimelineWidget::clipIndexAt(const QPoint &position) const
{
    for (int i = m_clips.size() - 1; i >= 0; --i) {
        if (clipRect(i).contains(position)) return i;
    }
    return -1;
}

int TimelineWidget::trackAt(int y) const
{
    const int track = (y - kTrackTop + kTrackHeight / 2)
        / (kTrackHeight + kTrackGap);
    return qBound(0, track, kTrackCount - 1);
}

qint64 TimelineWidget::timeAt(int x) const
{
    const int relativeX = qMax(0, x - kTimelineStartX);
    return qMax<qint64>(0, qRound64(
        static_cast<double>(relativeX) * 1000.0 / m_pixelsPerSecond));
}

void TimelineWidget::updateCanvasSize()
{
    qint64 furthestEnd = 0;
    for (const Clip &clip : m_clips) {
        furthestEnd = qMax(furthestEnd,
            clip.timelineStartMs + clip.durationMs);
    }

    const int contentWidth = kTimelineStartX
        + qRound(static_cast<double>(furthestEnd + 10000)
                 * m_pixelsPerSecond / 1000.0);
    setMinimumWidth(qMax(1100, contentWidth));
    updateGeometry();
}

QString TimelineWidget::formatTime(qint64 milliseconds) const
{
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds) / 1000;
    return QString("%1:%2")
        .arg(totalSeconds / 60, 2, 10, QChar('0'))
        .arg(totalSeconds % 60, 2, 10, QChar('0'));
}

void TimelineWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#10151b"));
    p.fillRect(QRect(0, 0, width(), kHeaderHeight), QColor("#171e26"));

    p.setPen(QColor("#81909e"));
    p.drawText(QRect(12, 0, 108, kHeaderHeight), Qt::AlignVCenter, "TRACKS");

    for (const Marker &marker : m_markers) {
        const int markerX = kTimelineStartX
            + qRound(static_cast<double>(marker.timeMs) * m_pixelsPerSecond / 1000.0);
        p.setPen(QPen(QColor("#d78cff"), 1, Qt::DashLine));
        p.drawLine(markerX, kHeaderHeight, markerX, height() - 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#d78cff"));
        p.drawPolygon(QPolygon({QPoint(markerX - 5, kHeaderHeight),
                                QPoint(markerX + 5, kHeaderHeight),
                                QPoint(markerX, kHeaderHeight + 9)}));
        p.setPen(QColor("#f1d8ff"));
        p.drawText(markerX + 7, kHeaderHeight + 14, marker.label);
    }

    const qint64 visibleEndMs = qMax<qint64>(60000,
        static_cast<qint64>(width() - kTimelineStartX) * 1000 / m_pixelsPerSecond);
    for (qint64 seconds = 0; seconds <= visibleEndMs / 1000; seconds += 1) {
        const int x = kTimelineStartX + static_cast<int>(seconds * m_pixelsPerSecond);
        const bool major = (seconds % 5 == 0);
        p.setPen(QPen(major ? QColor("#46545f") : QColor("#26313b"),
                      major ? 1.4 : 1.0));
        p.drawLine(x, kHeaderHeight, x, height() - 8);
        if (major) {
            p.setPen(QColor("#a8b5bf"));
            p.drawText(x + 5, 22, formatTime(seconds * 1000));
        }
    }

    const QStringList tracks = {
        "VIDEO 1", "VIDEO 2", "VIDEO 3", "AUDIO 1", "AUDIO 2"
    };
    for (int track = 0; track < tracks.size(); ++track) {
        const int y = kTrackTop + track * (kTrackHeight + kTrackGap);
        p.fillRect(QRect(8, y, kTrackLabelWidth - 8, kTrackHeight),
                   QColor("#1b242e"));
        p.fillRect(QRect(kTrackLabelWidth, y, width() - kTrackLabelWidth - 8,
                         kTrackHeight), QColor("#151c24"));
        p.setPen(track < 3 ? QColor("#c1ccd5") : QColor("#a2b3bf"));
        p.drawText(QRect(16, y, 56, kTrackHeight),
                   Qt::AlignVCenter | Qt::AlignLeft, tracks.at(track));
        const QRect lockRect(75, y + 6, 20, 22);
        const QRect muteRect(99, y + 6, 20, 22);
        p.setPen(QPen(m_trackLocked[track] ? QColor("#d78cff") : QColor("#596875"), 1));
        p.setBrush(m_trackLocked[track] ? QColor("#49305f") : QColor("#202a34"));
        p.drawRoundedRect(lockRect, 3, 3);
        p.setPen(m_trackLocked[track] ? QColor("#ffffff") : QColor("#aab7c2"));
        p.drawText(lockRect, Qt::AlignCenter, m_trackLocked[track] ? "L" : "l");
        p.setPen(QPen(m_trackMuted[track] ? QColor("#e6a36f") : QColor("#596875"), 1));
        p.setBrush(m_trackMuted[track] ? QColor("#59402d") : QColor("#202a34"));
        p.drawRoundedRect(muteRect, 3, 3);
        p.setPen(m_trackMuted[track] ? QColor("#ffffff") : QColor("#aab7c2"));
        p.drawText(muteRect, Qt::AlignCenter, m_trackMuted[track] ? "M" : "m");
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        const QRect bounds = clipRect(i);
        const Clip &clip = m_clips.at(i);
        const bool selected = m_selectedClipIds.contains(clip.id)
            || (m_selectedClipIds.isEmpty() && i == m_selectedClip);
        const bool audio = clip.track >= 3;
        const QColor fill = audio
            ? (selected ? QColor("#6a512b") : QColor("#47391f"))
            : (selected ? QColor("#146f76") : QColor("#174b55"));
        const QColor outline = audio ? QColor("#d7ad62") : QColor("#2c9b9b");

        p.setPen(QPen(selected ? QColor("#fff0b5") : outline, selected ? 2 : 1));
        p.setBrush(fill);
        p.drawRoundedRect(bounds, 4, 4);
        p.fillRect(QRect(bounds.left() + 2, bounds.top() + 3, 3,
                         bounds.height() - 6), outline);

        const QString name = QFileInfo(clip.path).fileName();
        const QString durationLabel = clip.durationKnown
            ? formatTime(clip.durationMs)
            : QString("~%1").arg(formatTime(clip.durationMs));
        p.setPen(QColor("#f2f6f7"));
        p.drawText(QRect(bounds.left() + 10, bounds.top() + 1,
                         qMax(0, bounds.width() - 18), 16),
            Qt::AlignVCenter | Qt::AlignLeft,
            p.fontMetrics().elidedText(
                name + "  [" + durationLabel + "]",
                Qt::ElideRight, qMax(0, bounds.width() - 18)));

        if (audio && !clip.waveformPeaks.isEmpty() && bounds.width() > 8) {
            const int centerY = bounds.top() + 25;
            const int amplitude = qMax(2, (bounds.height() - 22) / 2);
            p.setPen(QPen(selected ? QColor("#ffe0a0") : QColor("#d7ad62"), 1));
            const int peakCount = static_cast<int>(clip.waveformPeaks.size());
            for (int x = bounds.left() + 4; x < bounds.right() - 3; ++x) {
                const double relative = static_cast<double>(x - bounds.left() - 4)
                    / qMax(1, bounds.width() - 8);
                const int sampleIndex = std::clamp(
                    static_cast<int>(relative * peakCount), 0, peakCount - 1);
                const int peakHeight = std::max(1,
                    qRound(clip.waveformPeaks.at(sampleIndex) * amplitude));
                p.drawLine(x, centerY - peakHeight, x, centerY + peakHeight);
            }
        }

        if (audio && (clip.fadeInMs > 0 || clip.fadeOutMs > 0)) {
            const int fadeInWidth = qBound(0,
                qRound(static_cast<double>(clip.fadeInMs) * m_pixelsPerSecond / 1000.0),
                qMax(0, bounds.width() - 8));
            const int fadeOutWidth = qBound(0,
                qRound(static_cast<double>(clip.fadeOutMs) * m_pixelsPerSecond / 1000.0),
                qMax(0, bounds.width() - 8));
            const int centerY = bounds.top() + 25;
            const int amplitude = qMax(2, (bounds.height() - 22) / 2);
            p.setPen(QPen(QColor("#fff0b5"), 2, Qt::DashLine));
            if (fadeInWidth > 0) {
                p.drawLine(bounds.left() + 4, centerY + amplitude,
                           bounds.left() + 4 + fadeInWidth, centerY);
            }
            if (fadeOutWidth > 0) {
                p.drawLine(bounds.right() - 4, centerY + amplitude,
                           bounds.right() - 4 - fadeOutWidth, centerY);
            }
        }

        if (audio && !clip.audioVolumeKeyframes.isEmpty()
            && clip.durationMs > 0 && bounds.width() > 12) {
            const int centerY = bounds.top() + 25;
            const int amplitude = qMax(2, (bounds.height() - 22) / 2);
            const int leftX = bounds.left() + 4;
            const int usableWidth = qMax(1, bounds.width() - 8);
            auto automationPoint = [&](qint64 timeMs, double volume) {
                const double position = qBound(0.0,
                    static_cast<double>(timeMs) / clip.durationMs, 1.0);
                const int x = leftX + qRound(position * usableWidth);
                const int y = centerY + amplitude
                    - qRound(qBound(0.0, volume, 100.0) * amplitude * 2.0 / 100.0);
                return QPoint(x, y);
            };
            p.setPen(QPen(QColor("#b99aff"), 2));
            QPoint previous = automationPoint(0, clip.audioVolume);
            for (const Keyframe &keyframe : clip.audioVolumeKeyframes) {
                const QPoint current = automationPoint(keyframe.timeMs, keyframe.value);
                p.drawLine(previous, current);
                previous = current;
            }
            // Keep the final automation value visible through the end of the clip.
            const QPoint clipEnd = automationPoint(clip.durationMs,
                clip.audioVolumeKeyframes.last().value);
            p.drawLine(previous, clipEnd);
            p.setBrush(QColor("#d8c5ff"));
            for (const Keyframe &keyframe : clip.audioVolumeKeyframes) {
                const QPoint point = automationPoint(keyframe.timeMs, keyframe.value);
                p.drawPolygon(QPolygon({QPoint(point.x(), point.y() - 4),
                                        QPoint(point.x() + 4, point.y()),
                                        QPoint(point.x(), point.y() + 4),
                                        QPoint(point.x() - 4, point.y())}));
            }
        }

        if (selected) {
            p.setPen(QPen(QColor("#f4d18c"), 2));
            p.drawLine(bounds.left() + kTrimHandleWidth, bounds.top() + 5,
                       bounds.left() + kTrimHandleWidth, bounds.bottom() - 5);
            p.drawLine(bounds.right() - kTrimHandleWidth, bounds.top() + 5,
                       bounds.right() - kTrimHandleWidth, bounds.bottom() - 5);
        }
    }

    const int playheadX = kTimelineStartX
        + qRound(static_cast<double>(m_playheadMs) * m_pixelsPerSecond / 1000.0);
    p.setPen(QPen(QColor("#f0c879"), 2));
    p.drawLine(playheadX, 0, playheadX, height() - 6);
    p.setBrush(QColor("#f0c879"));
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygon({QPoint(playheadX - 7, 0),
                            QPoint(playheadX + 7, 0),
                            QPoint(playheadX, 10)}));

    if (m_clips.isEmpty()) {
        p.setPen(QColor("#667481"));
        p.drawText(QRect(kTimelineStartX, kTrackTop,
                         width() - kTimelineStartX - 20, 32),
                   Qt::AlignVCenter,
                   "Import media, then add a clip. Drag clips to move; drag their edges to trim.");
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        const int controlTrack = (pos.y() - kTrackTop) / (kTrackHeight + kTrackGap);
        if (pos.x() >= 74 && pos.x() < 124 && pos.y() >= kTrackTop
            && controlTrack >= 0 && controlTrack < kTrackCount
            && (pos.y() - kTrackTop) % (kTrackHeight + kTrackGap) < kTrackHeight) {
            if (pos.x() < 98) m_trackLocked[controlTrack] = !m_trackLocked[controlTrack];
            else m_trackMuted[controlTrack] = !m_trackMuted[controlTrack];
            update();
            emit trackStateChanged(controlTrack);
            emit timelineEdited();
            event->accept();
            return;
        }
        const int index = clipIndexAt(pos);
        m_playheadMs = timeAt(pos.x());
        emit playheadChanged(m_playheadMs);

        if (index >= 0) {
            const int clickedId = m_clips.at(index).id;
            const bool slipGesture = event->modifiers().testFlag(Qt::ControlModifier)
                && event->modifiers().testFlag(Qt::AltModifier);
            const bool multiSelect = event->modifiers().testFlag(Qt::ControlModifier)
                && !event->modifiers().testFlag(Qt::AltModifier);
            const bool removingFromSelection =
                multiSelect && m_selectedClipIds.contains(clickedId);

            if (multiSelect) {
                if (removingFromSelection)
                    m_selectedClipIds.remove(clickedId);
                else
                    m_selectedClipIds.insert(clickedId);
                if (m_selectedClipIds.isEmpty())
                    m_selectedClipIds.insert(clickedId);
            } else if (!m_selectedClipIds.contains(clickedId)) {
                m_selectedClipIds.clear();
                m_selectedClipIds.insert(clickedId);
            }

            // Keep the Inspector focused on a clip that is still selected.
            // Ctrl-clicking a selected clip only changes selection; it must
            // not accidentally start dragging the clip that was removed.
            int activeIndex = index;
            if (removingFromSelection && m_selectedClipIds.size() > 0
                && m_selectedClipIds.contains(clickedId)) {
                activeIndex = index;
            } else if (removingFromSelection) {
                m_dragMode = DragMode::None;
                for (int i = m_clips.size() - 1; i >= 0; --i) {
                    if (m_selectedClipIds.contains(m_clips.at(i).id)) {
                        activeIndex = i;
                        break;
                    }
                }
            }

            m_selectedClip = activeIndex;
            m_dragOrigin = pos;
            m_dragSnapshot = m_clips.at(activeIndex);
            m_rollPartnerSnapshot = Clip{};
            m_rollPartnerSnapshot.id = -1;
            m_dragGroupSnapshot.clear();
            for (const Clip &candidate : m_clips) {
                if (m_selectedClipIds.contains(candidate.id))
                    m_dragGroupSnapshot.append(candidate);
            }

            if (!removingFromSelection) {
                const QRect bounds = clipRect(activeIndex);
                const bool insideClipBody = pos.x() - bounds.left() > kTrimHandleWidth
                    && bounds.right() - pos.x() > kTrimHandleWidth;
                const bool groupMove = m_selectedClipIds.size() > 1 && insideClipBody;
                if (slipGesture && insideClipBody) {
                    m_dragMode = DragMode::Slip;
                } else if (groupMove) {
                    m_dragMode = DragMode::Move;
                } else if (pos.x() - bounds.left() <= kTrimHandleWidth) {
                    m_dragMode = DragMode::TrimLeft;
                } else if (bounds.right() - pos.x() <= kTrimHandleWidth) {
                    if (event->modifiers().testFlag(Qt::AltModifier)) {
                        const qint64 editPoint = m_dragSnapshot.timelineStartMs
                            + m_dragSnapshot.durationMs;
                        for (const Clip &candidate : m_clips) {
                            if (candidate.id != m_dragSnapshot.id
                                && candidate.track == m_dragSnapshot.track
                                && candidate.timelineStartMs == editPoint) {
                                m_rollPartnerSnapshot = candidate;
                                break;
                            }
                        }
                    }
                    m_dragMode = m_rollPartnerSnapshot.id >= 0
                        ? DragMode::RollTrimRight
                        : (event->modifiers().testFlag(Qt::ShiftModifier)
                            ? DragMode::RippleTrimRight : DragMode::TrimRight);
                } else {
                    m_dragMode = DragMode::Move;
                }
            }

            const Clip &activeClip = m_clips.at(activeIndex);
            emit clipActivated(activeClip.path);
            emit clipSelectionChanged(activeClip.id, activeClip.path, activeClip.track);
        } else {
            m_selectedClip = -1;
            m_selectedClipIds.clear();
            m_dragGroupSnapshot.clear();
            m_dragMode = DragMode::None;
            emit clipSelectionChanged(-1, QString(), -1);
        }

        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}


void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        if (pos.y() >= kHeaderHeight && pos.y() <= kHeaderHeight + 34) {
            int nearestIndex = -1;
            int nearestDistance = 13;
            for (int i = 0; i < m_markers.size(); ++i) {
                const int markerX = kTimelineStartX
                    + qRound(static_cast<double>(m_markers.at(i).timeMs)
                             * m_pixelsPerSecond / 1000.0);
                const int distance = qAbs(markerX - pos.x());
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestIndex = i;
                }
            }

            if (nearestIndex >= 0) {
                bool accepted = false;
                const QString label = QInputDialog::getText(
                    this, "Rename Timeline Marker", "Marker name:",
                    QLineEdit::Normal, m_markers.at(nearestIndex).label, &accepted);
                if (accepted) {
                    const QString trimmedLabel = label.trimmed();
                    if (!trimmedLabel.isEmpty()) {
                        m_markers[nearestIndex].label = trimmedLabel;
                        update();
                        emit markersChanged();
                        emit timelineEdited();
                    }
                }
                event->accept();
                return;
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragMode == DragMode::None || m_selectedClip < 0
        || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint pos = event->position().toPoint();
    const int deltaPixels = pos.x() - m_dragOrigin.x();
    qint64 deltaMs = qRound64(
        static_cast<double>(deltaPixels) * 1000.0 / m_pixelsPerSecond);
    deltaMs = qRound64(static_cast<double>(deltaMs) / frameDurationMs())
        * frameDurationMs();

    Clip &clip = m_clips[m_selectedClip];
    if (isTrackLocked(clip.track)) return;
    const qint64 snapToleranceMs = qMax<qint64>(
        frameDurationMs(), qRound64(8.0 * 1000.0 / m_pixelsPerSecond));

    // Move a Ctrl-selected group together while preserving each clip's
    // relative timing and track. Group movement is horizontal for safety.
    if (m_dragMode == DragMode::Move && m_dragGroupSnapshot.size() > 1) {
        qint64 groupStart = std::numeric_limits<qint64>::max();
        qint64 groupEnd = 0;
        for (const Clip &item : m_dragGroupSnapshot) {
            if (isTrackLocked(item.track)) return;
            groupStart = qMin(groupStart, item.timelineStartMs);
            groupEnd = qMax(groupEnd, item.timelineStartMs + item.durationMs);
        }
        qint64 proposedStart = qMax<qint64>(0, groupStart + deltaMs);
        qint64 proposedEnd = groupEnd + (proposedStart - groupStart);
        qint64 bestDelta = proposedStart - groupStart;
        qint64 bestDistance = snapToleranceMs + 1;
        auto considerSnap = [&](qint64 edge) {
            const qint64 distance = qAbs(edge - proposedStart);
            if (distance <= snapToleranceMs && distance < bestDistance) {
                bestDistance = distance;
                bestDelta = edge - groupStart;
            }
            const qint64 endDistance = qAbs(edge - proposedEnd);
            if (endDistance <= snapToleranceMs && endDistance < bestDistance) {
                bestDistance = endDistance;
                bestDelta = edge - groupEnd;
            }
        };
        for (const Clip &other : m_clips) {
            if (m_selectedClipIds.contains(other.id)) continue;
            considerSnap(other.timelineStartMs);
            considerSnap(other.timelineStartMs + other.durationMs);
        }
        for (const Marker &marker : m_markers)
            considerSnap(marker.timeMs);
        bestDelta = qMax<qint64>(-groupStart, bestDelta);

        // A grouped drag may also change tracks. Preserve each clip's
        // relative track offset and reject the entire move if any destination
        // is outside the timeline or locked.
        const int trackDelta = trackAt(pos.y()) - m_dragSnapshot.track;
        bool canMoveGroup = true;
        for (const Clip &snapshot : m_dragGroupSnapshot) {
            const int destinationTrack = snapshot.track + trackDelta;
            if (destinationTrack < 0 || destinationTrack >= kTrackCount
                || isTrackLocked(destinationTrack)) {
                canMoveGroup = false;
                break;
            }
        }
        if (!canMoveGroup) return;

        for (const Clip &snapshot : m_dragGroupSnapshot) {
            const int targetIndex = indexForClipId(snapshot.id);
            if (targetIndex >= 0) {
                m_clips[targetIndex].timelineStartMs = snapshot.timelineStartMs + bestDelta;
                m_clips[targetIndex].track = snapshot.track + trackDelta;
            }
        }
        updateCanvasSize();
        update();
        return;
    }

    // Snap the moving edge to nearby edges of other clips. The selected
    // clip is excluded so it can never snap to itself.
    const auto snapToClipEdges = [this, snapToleranceMs](qint64 movingTime,
                                                         int excludedIndex) {
        qint64 bestTime = movingTime;
        qint64 bestDistance = snapToleranceMs + 1;
        for (int i = 0; i < m_clips.size(); ++i) {
            if (i == excludedIndex) continue;
            const Clip &other = m_clips.at(i);
            const qint64 edges[] = {
                other.timelineStartMs,
                other.timelineStartMs + other.durationMs
            };
            for (qint64 edge : edges) {
                const qint64 distance = qAbs(edge - movingTime);
                if (distance <= snapToleranceMs && distance < bestDistance) {
                    bestDistance = distance;
                    bestTime = edge;
                }
            }
        }
        // Timeline markers are useful alignment targets when arranging clips.
        for (const Marker &marker : m_markers) {
            const qint64 distance = qAbs(marker.timeMs - movingTime);
            if (distance <= snapToleranceMs && distance < bestDistance) {
                bestDistance = distance;
                bestTime = marker.timeMs;
            }
        }
        return bestTime;
    };

    if (m_dragMode == DragMode::Move) {
        qint64 proposedStart = qMax<qint64>(
            0, m_dragSnapshot.timelineStartMs + deltaMs);
        const qint64 proposedEnd = proposedStart + m_dragSnapshot.durationMs;
        const qint64 snappedStart = snapToClipEdges(proposedStart, m_selectedClip);
        const qint64 snappedEnd = snapToClipEdges(proposedEnd, m_selectedClip);

        const qint64 startDistance = qAbs(snappedStart - proposedStart);
        const qint64 endDistance = qAbs(snappedEnd - proposedEnd);
        if (startDistance <= endDistance && snappedStart != proposedStart) {
            proposedStart = snappedStart;
        } else if (snappedEnd != proposedEnd) {
            proposedStart = snappedEnd - m_dragSnapshot.durationMs;
        }

        clip.timelineStartMs = qMax<qint64>(0, proposedStart);
        const int destinationTrack = trackAt(pos.y());
        if (!isTrackLocked(destinationTrack)) clip.track = destinationTrack;
    } else if (m_dragMode == DragMode::TrimLeft) {
        const qint64 minimumDelta = -m_dragSnapshot.sourceInMs;
        const qint64 maximumDelta =
            m_dragSnapshot.durationMs - kMinimumClipDurationMs;
        deltaMs = qBound(minimumDelta, deltaMs, maximumDelta);
        qint64 proposedStart = m_dragSnapshot.timelineStartMs + deltaMs;
        const qint64 snappedStart = snapToClipEdges(proposedStart, m_selectedClip);
        if (qAbs(snappedStart - proposedStart) <= snapToleranceMs) {
            proposedStart = snappedStart;
        }
        deltaMs = qBound(minimumDelta,
            proposedStart - m_dragSnapshot.timelineStartMs, maximumDelta);
        clip.timelineStartMs = m_dragSnapshot.timelineStartMs + deltaMs;
        clip.sourceInMs = m_dragSnapshot.sourceInMs + deltaMs;
        clip.durationMs = m_dragSnapshot.durationMs - deltaMs;
    } else if (m_dragMode == DragMode::Slip) {
        if (!m_dragSnapshot.durationKnown || m_dragSnapshot.sourceDurationMs <= 0) return;
        const qint64 maximumSourceIn = qMax<qint64>(
            0, m_dragSnapshot.sourceDurationMs - m_dragSnapshot.durationMs);
        clip.sourceInMs = qBound<qint64>(0,
            m_dragSnapshot.sourceInMs + deltaMs, maximumSourceIn);
    } else if (m_dragMode == DragMode::RollTrimRight) {
        const int partnerIndex = indexForClipId(m_rollPartnerSnapshot.id);
        if (partnerIndex < 0 || isTrackLocked(m_rollPartnerSnapshot.track)
            || m_rollPartnerSnapshot.track != m_dragSnapshot.track) return;
        // A roll edit moves one shared cut: the outgoing clip gains exactly
        // the duration that the incoming clip loses, preserving the sequence end.
        const qint64 minimumDelta = qMax(
            kMinimumClipDurationMs - m_dragSnapshot.durationMs,
            -m_rollPartnerSnapshot.sourceInMs);
        const qint64 maximumDelta =
            m_rollPartnerSnapshot.durationMs - kMinimumClipDurationMs;
        deltaMs = qBound(minimumDelta, deltaMs, maximumDelta);
        const qint64 rolledDelta = deltaMs < 0
            ? -m_timebase.snapMilliseconds(-deltaMs)
            : m_timebase.snapMilliseconds(deltaMs);
        const qint64 safeDelta = qBound(minimumDelta, rolledDelta, maximumDelta);
        clip.durationMs = m_dragSnapshot.durationMs + safeDelta;
        Clip &partner = m_clips[partnerIndex];
        partner.timelineStartMs = m_rollPartnerSnapshot.timelineStartMs + safeDelta;
        partner.sourceInMs = m_rollPartnerSnapshot.sourceInMs + safeDelta;
        partner.durationMs = m_rollPartnerSnapshot.durationMs - safeDelta;
    } else if (m_dragMode == DragMode::TrimRight
               || m_dragMode == DragMode::RippleTrimRight) {
        qint64 proposedEnd = m_dragSnapshot.timelineStartMs
            + m_dragSnapshot.durationMs + deltaMs;
        const qint64 snappedEnd = snapToClipEdges(proposedEnd, m_selectedClip);
        if (qAbs(snappedEnd - proposedEnd) <= snapToleranceMs)
            proposedEnd = snappedEnd;
        const qint64 newDuration = qMax(kMinimumClipDurationMs,
            proposedEnd - m_dragSnapshot.timelineStartMs);
        const qint64 durationDelta = newDuration - m_dragSnapshot.durationMs;
        if (m_dragMode == DragMode::RippleTrimRight && durationDelta != 0) {
            const qint64 oldEnd = m_dragSnapshot.timelineStartMs
                + m_dragSnapshot.durationMs;
            // Validate every downstream edit before moving anything: ripple
            // trimming must never partially modify a locked track.
            for (const Clip &other : m_clips) {
                if (other.id == m_dragSnapshot.id
                    || other.track != m_dragSnapshot.track
                    || other.timelineStartMs < oldEnd) continue;
                if (isTrackLocked(other.track)) return;
            }
            for (Clip &other : m_clips) {
                if (other.id == m_dragSnapshot.id
                    || other.track != m_dragSnapshot.track
                    || other.timelineStartMs < oldEnd) continue;
                other.timelineStartMs = m_timebase.snapMilliseconds(
                    qMax<qint64>(0, other.timelineStartMs + durationDelta));
            }
        }
        clip.durationMs = newDuration;
    }

    updateCanvasSize();
    update();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragMode != DragMode::None) {
        m_dragMode = DragMode::None;
        emit timelineEdited();
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        if (event->key() == Qt::Key_Left)
            goToPreviousMarker();
        else
            goToNextMarker();
        event->accept();
        return;
    }

    if (event->modifiers().testFlag(Qt::AltModifier)
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        const int direction = event->key() == Qt::Key_Left ? -1 : 1;
        nudgeSelectedClip(direction);
        event->accept();
        return;
    }

    const qint64 stepMs = event->modifiers().testFlag(Qt::ShiftModifier)
        ? 1000 : frameDurationMs();

    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const qint64 direction = event->key() == Qt::Key_Left ? -1 : 1;
        setPlayheadMs(m_playheadMs + direction * stepMs, true);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_M) {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            removeNearestMarker();
        else
            addMarker();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Home) {
        setPlayheadMs(0, true);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_End) {
        setPlayheadMs(sequenceDurationMs(), true);
        event->accept();
        return;
    }

    // Page Up/Down jump between edit points across all timeline tracks.
    if (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) {
        const bool forward = event->key() == Qt::Key_PageDown;
        qint64 target = forward
            ? std::numeric_limits<qint64>::max() : -1;
        for (const Clip &clip : m_clips) {
            const qint64 editPoints[] = {
                clip.timelineStartMs,
                clip.timelineStartMs + clip.durationMs
            };
            for (qint64 point : editPoints) {
                if (forward && point > m_playheadMs && point < target)
                    target = point;
                else if (!forward && point < m_playheadMs && point > target)
                    target = point;
            }
        }
        if ((forward && target != std::numeric_limits<qint64>::max())
            || (!forward && target >= 0)) {
            setPlayheadMs(target, true);
        }
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (deleteSelectedClip()) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}
