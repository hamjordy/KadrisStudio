#include "timelinewidget.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QSizePolicy>
#include <QStringList>
#include <limits>
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
constexpr int kPixelsPerSecond = 80;
constexpr int kClipMinimumWidth = 18;
constexpr qint64 kDefaultClipDurationMs = 5000;
constexpr qint64 kMinimumClipDurationMs = 250;
constexpr qint64 kSnapIntervalMs = 100;
constexpr int kTrimHandleWidth = 8;
constexpr int kTrackCount = 5;
}

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("timelineCanvas");
    setMinimumSize(1100, 270);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
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

    m_clips.append(clip);
    m_selectedClip = m_clips.size() - 1;
    m_playheadMs = clip.timelineStartMs;
    updateCanvasSize();
    update();

    emit clipActivated(path);
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
    m_playheadMs = qMax<qint64>(0, positionMs);
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

bool TimelineWidget::deleteSelectedClip()
{
    if (m_selectedClip < 0 || m_selectedClip >= m_clips.size()) return false;

    m_clips.removeAt(m_selectedClip);
    if (m_clips.isEmpty()) m_selectedClip = -1;
    else m_selectedClip = qMin(m_selectedClip, m_clips.size() - 1);

    updateCanvasSize();
    update();
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
        clip.timelineStartMs = item.contains("timelineStartMs")
            ? static_cast<qint64>(item.value("timelineStartMs").toDouble())
            : legacyTrackEnd[track];
        clip.sourceInMs = static_cast<qint64>(item.value("sourceInMs").toDouble());
        clip.durationMs = item.contains("durationMs")
            ? static_cast<qint64>(item.value("durationMs").toDouble())
            : kDefaultClipDurationMs;
        clip.durationMs = qMax(kMinimumClipDurationMs, clip.durationMs);
        clip.durationKnown = item.value("durationKnown").toBool(false);

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
        + qRound(static_cast<double>(clip.timelineStartMs) * kPixelsPerSecond / 1000.0);
    const int y = kTrackTop + clip.track * (kTrackHeight + kTrackGap);
    const int width = qMax(kClipMinimumWidth,
        qRound(static_cast<double>(clip.durationMs) * kPixelsPerSecond / 1000.0));
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
        static_cast<double>(relativeX) * 1000.0 / kPixelsPerSecond));
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
                 * kPixelsPerSecond / 1000.0);
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

    const qint64 visibleEndMs = qMax<qint64>(60000,
        static_cast<qint64>(width() - kTimelineStartX) * 1000 / kPixelsPerSecond);
    for (qint64 seconds = 0; seconds <= visibleEndMs / 1000; seconds += 1) {
        const int x = kTimelineStartX + static_cast<int>(seconds * kPixelsPerSecond);
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
        p.drawText(QRect(16, y, 100, kTrackHeight),
                   Qt::AlignVCenter | Qt::AlignLeft, tracks.at(track));
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        const QRect bounds = clipRect(i);
        const Clip &clip = m_clips.at(i);
        const bool selected = (i == m_selectedClip);
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
        p.drawText(bounds.adjusted(10, 0, -6, 0),
            Qt::AlignVCenter | Qt::AlignLeft,
            p.fontMetrics().elidedText(
                name + "  [" + durationLabel + "]",
                Qt::ElideRight, qMax(0, bounds.width() - 18)));

        if (selected) {
            p.setPen(QPen(QColor("#f4d18c"), 2));
            p.drawLine(bounds.left() + kTrimHandleWidth, bounds.top() + 5,
                       bounds.left() + kTrimHandleWidth, bounds.bottom() - 5);
            p.drawLine(bounds.right() - kTrimHandleWidth, bounds.top() + 5,
                       bounds.right() - kTrimHandleWidth, bounds.bottom() - 5);
        }
    }

    const int playheadX = kTimelineStartX
        + qRound(static_cast<double>(m_playheadMs) * kPixelsPerSecond / 1000.0);
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
        const int index = clipIndexAt(pos);
        m_playheadMs = timeAt(pos.x());
        emit playheadChanged(m_playheadMs);

        if (index >= 0) {
            m_selectedClip = index;
            m_dragOrigin = pos;
            m_dragSnapshot = m_clips.at(index);
            const QRect bounds = clipRect(index);

            if (pos.x() - bounds.left() <= kTrimHandleWidth) {
                m_dragMode = DragMode::TrimLeft;
            } else if (bounds.right() - pos.x() <= kTrimHandleWidth) {
                m_dragMode = DragMode::TrimRight;
            } else {
                m_dragMode = DragMode::Move;
            }

            emit clipActivated(m_clips.at(index).path);
        } else {
            m_selectedClip = -1;
            m_dragMode = DragMode::None;
        }

        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
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
        static_cast<double>(deltaPixels) * 1000.0 / kPixelsPerSecond);
    deltaMs = qRound64(static_cast<double>(deltaMs) / kSnapIntervalMs)
        * kSnapIntervalMs;

    Clip &clip = m_clips[m_selectedClip];
    if (m_dragMode == DragMode::Move) {
        clip.timelineStartMs = qMax<qint64>(
            0, m_dragSnapshot.timelineStartMs + deltaMs);
        clip.track = trackAt(pos.y());
    } else if (m_dragMode == DragMode::TrimLeft) {
        const qint64 minimumDelta = -m_dragSnapshot.sourceInMs;
        const qint64 maximumDelta =
            m_dragSnapshot.durationMs - kMinimumClipDurationMs;
        deltaMs = qBound(minimumDelta, deltaMs, maximumDelta);
        clip.timelineStartMs = m_dragSnapshot.timelineStartMs + deltaMs;
        clip.sourceInMs = m_dragSnapshot.sourceInMs + deltaMs;
        clip.durationMs = m_dragSnapshot.durationMs - deltaMs;
    } else if (m_dragMode == DragMode::TrimRight) {
        clip.durationMs = qMax(kMinimumClipDurationMs,
                               m_dragSnapshot.durationMs + deltaMs);
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
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (deleteSelectedClip()) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}
