#pragma once

#include "frame_timebase.h"

#include <QJsonArray>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>
#include <QWidget>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

class TimelineWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget *parent = nullptr);

    struct ClipInfo {
        int id = -1;
        QString path;
        int track = 0;
        qint64 timelineStartMs = 0;
        qint64 sourceInMs = 0;
        qint64 durationMs = 0;
        int brightness = 0, contrast = 0, saturation = 100;
        int shadows = 0, midtones = 0, highlights = 0;
        int redCurve = 0, greenCurve = 0, blueCurve = 0;
        int keyframeCount = 0;
        int scale = 100, positionX = 0, positionY = 0;
        int opacity = 100;
        int blendMode = 0;
        int audioVolume = 100;
        bool audioMuted = false;
        QVector<float> waveformPeaks;
        int fadeInMs = 0, fadeOutMs = 0;
    };

    void addClip(const QString &path);
    void setFrameRate(int numerator, int denominator);
    int frameRateNumerator() const;
    int frameRateDenominator() const;
    int replaceMediaPath(const QString &oldPath, const QString &newPath);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void addMarker();
    void removeNearestMarker();
    bool removeMarkerAtTime(qint64 timeMs);
    bool isTrackLocked(int track) const;
    bool isTrackMuted(int track) const;
    QJsonArray trackStatesToJson() const;
    void trackStatesFromJson(const QJsonArray &states);
    void goToNextMarker();
    void goToPreviousMarker();
    QJsonArray markersToJson() const;
    void markersFromJson(const QJsonArray &markers);
    void clearClips();
    bool clipAtTime(qint64 positionMs, ClipInfo *clip) const;
    int selectedClipId() const;
    bool selectedClipInfo(ClipInfo *clip) const;
    bool setClipColorSettings(int clipId, int brightness, int contrast, int saturation,
                              int shadows, int midtones, int highlights,
                              int redCurve, int greenCurve, int blueCurve);
    bool addBrightnessKeyframe(int clipId, qint64 timelineTimeMs, int value);
    bool addContrastKeyframe(int clipId, qint64 timelineTimeMs, int value);
    bool setClipTransformSettings(int clipId, int scale, int positionX, int positionY);
    bool setClipOpacity(int clipId, int opacity);
    bool setClipBlendMode(int clipId, int blendMode);
    bool setClipAudioSettings(int clipId, int volume, int fadeInMs, int fadeOutMs);
    bool setClipAudioMuted(int clipId, bool muted);
    bool addAudioVolumeKeyframe(int clipId, qint64 timelineTimeMs, int volume);
    bool removeAudioVolumeKeyframeAt(int clipId, qint64 timelineTimeMs);
    int audioVolumeKeyframeCount(int clipId) const;
    int evaluatedAudioVolume(int clipId, qint64 timelineTimeMs) const;
    bool addOpacityKeyframe(int clipId, qint64 timelineTimeMs, int opacity);
    bool removeOpacityKeyframeAt(int clipId, qint64 timelineTimeMs);
    int opacityKeyframeCount(int clipId) const;
    int evaluatedOpacity(int clipId, qint64 timelineTimeMs) const;
    bool addTransformKeyframe(int clipId, qint64 timelineTimeMs, int scale, int positionX, int positionY);
    bool removeTransformKeyframeAt(int clipId, qint64 timelineTimeMs);
    int transformKeyframeCount(int clipId) const;
    void evaluatedTransform(int clipId, qint64 timelineTimeMs, int *scale, int *positionX, int *positionY) const;
    bool removeBrightnessKeyframeAt(int clipId, qint64 timelineTimeMs);
    bool removeContrastKeyframeAt(int clipId, qint64 timelineTimeMs);
    int brightnessKeyframeCount(int clipId) const;
    int contrastKeyframeCount(int clipId) const;
    int evaluatedBrightness(int clipId, qint64 timelineTimeMs) const;
    int evaluatedContrast(int clipId, qint64 timelineTimeMs) const;
    bool clipAtTimeOnTrack(qint64 positionMs, int track, ClipInfo *clip) const;
    bool nextVideoClipAtOrAfter(qint64 positionMs, ClipInfo *clip) const;
    qint64 sequenceDurationMs() const;
    qint64 playheadMs() const;
    void setPlayheadMs(qint64 positionMs, bool notify = false);
    bool splitSelectedClipAtPlayhead();
    int splitAllClipsAtPlayhead();
    bool deleteSelectedClip();
    bool duplicateSelectedClip();
    bool rippleDeleteSelectedClip();
    bool nudgeSelectedClip(int frameDirection);
    QJsonArray toJson() const;
    void fromJson(const QJsonArray &clips);

signals:
    void clipActivated(const QString &path);
    void clipSelectionChanged(int clipId, const QString &path, int track);
    void playheadChanged(qint64 positionMs);
    void timelineEdited();
    void markersChanged();
    void trackStateChanged(int track);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct Keyframe { qint64 timeMs = 0; double value = 0.0; };

    struct Clip {
        int id = 0;
        QString path;
        int track = 0;
        qint64 timelineStartMs = 0;
        qint64 sourceInMs = 0;
        qint64 durationMs = 5000;
        bool durationKnown = false;
        int brightness = 0, contrast = 0, saturation = 100;
        int shadows = 0, midtones = 0, highlights = 0;
        int redCurve = 0, greenCurve = 0, blueCurve = 0;
        QVector<Keyframe> brightnessKeyframes;
        QVector<Keyframe> contrastKeyframes;
        int opacity = 100;
        int blendMode = 0;
        int audioVolume = 100;
        bool audioMuted = false;
        QVector<float> waveformPeaks;
        int fadeInMs = 0, fadeOutMs = 0;
        QVector<Keyframe> audioVolumeKeyframes;
        QVector<Keyframe> opacityKeyframes;
        struct TransformKeyframe { qint64 timeMs = 0; double scale = 100.0; double x = 0.0; double y = 0.0; };
        int scale = 100, positionX = 0, positionY = 0;
        QVector<TransformKeyframe> transformKeyframes;
    };

    enum class DragMode { None, Move, TrimLeft, TrimRight, RippleTrimRight, RollTrimRight };

    QRect clipRect(int index) const;
    int clipIndexAt(const QPoint &position) const;
    int trackAt(int y) const;
    qint64 timeAt(int x) const;
    void updateCanvasSize();
    void probeMediaDuration(int clipId, const QString &path);
    int indexForClipId(int clipId) const;
    QString formatTime(qint64 milliseconds) const;
    qint64 frameDurationMs() const;

    struct Marker { qint64 timeMs = 0; QString label; };
    QVector<Clip> m_clips;
    QVector<Marker> m_markers;
    bool m_trackLocked[5] = {false, false, false, false, false};
    bool m_trackMuted[5] = {false, false, false, false, false};
    int m_selectedClip = -1;
    int m_nextClipId = 1;
    qint64 m_playheadMs = 0;
    int m_pixelsPerSecond = 80;
    FrameTimebase m_timebase;

    DragMode m_dragMode = DragMode::None;
    QPoint m_dragOrigin;
    Clip m_dragSnapshot;
    Clip m_rollPartnerSnapshot;
    QSet<int> m_selectedClipIds;
    QVector<Clip> m_dragGroupSnapshot;
};
