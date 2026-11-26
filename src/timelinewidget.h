#pragma once

#include <QJsonArray>
#include <QPoint>
#include <QRect>
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
    };

    void addClip(const QString &path);
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
    bool clipAtTimeOnTrack(qint64 positionMs, int track, ClipInfo *clip) const;
    bool nextVideoClipAtOrAfter(qint64 positionMs, ClipInfo *clip) const;
    qint64 sequenceDurationMs() const;
    qint64 playheadMs() const;
    void setPlayheadMs(qint64 positionMs, bool notify = false);
    bool splitSelectedClipAtPlayhead();
    bool deleteSelectedClip();
    bool duplicateSelectedClip();
    bool rippleDeleteSelectedClip();
    QJsonArray toJson() const;
    void fromJson(const QJsonArray &clips);

signals:
    void clipActivated(const QString &path);
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
    struct Clip {
        int id = 0;
        QString path;
        int track = 0;
        qint64 timelineStartMs = 0;
        qint64 sourceInMs = 0;
        qint64 durationMs = 5000;
        bool durationKnown = false;
    };

    enum class DragMode { None, Move, TrimLeft, TrimRight };

    QRect clipRect(int index) const;
    int clipIndexAt(const QPoint &position) const;
    int trackAt(int y) const;
    qint64 timeAt(int x) const;
    void updateCanvasSize();
    void probeMediaDuration(int clipId, const QString &path);
    int indexForClipId(int clipId) const;
    QString formatTime(qint64 milliseconds) const;

    struct Marker { qint64 timeMs = 0; QString label; };
    QVector<Clip> m_clips;
    QVector<Marker> m_markers;
    bool m_trackLocked[5] = {false, false, false, false, false};
    bool m_trackMuted[5] = {false, false, false, false, false};
    int m_selectedClip = -1;
    int m_nextClipId = 1;
    qint64 m_playheadMs = 0;
    int m_pixelsPerSecond = 80;

    DragMode m_dragMode = DragMode::None;
    QPoint m_dragOrigin;
    Clip m_dragSnapshot;
};
