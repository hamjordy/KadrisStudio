#include "timelinewidget.h"
#include <QFileInfo>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QSizePolicy>
#include <QStringList>

namespace {
constexpr int kTrackTop = 42;
constexpr int kTrackHeight = 32;
constexpr int kTrackGap = 8;
constexpr int kClipStart = 142;
constexpr int kClipStep = 198;
constexpr int kClipWidth = 184;
}

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("timelineCanvas");
    setMinimumSize(1000, 252);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void TimelineWidget::addClip(const QString &path)
{
    if (path.isEmpty()) return;
    m_clips.append({path, 0});
    setMinimumWidth(qMax(1000, kClipStart + m_clips.size() * kClipStep + 24));
    updateGeometry();
    update();
}

void TimelineWidget::clearClips()
{
    m_clips.clear();
    m_selectedClip = -1;
    setMinimumWidth(1000);
    updateGeometry();
    update();
}

QJsonArray TimelineWidget::toJson() const
{
    QJsonArray result;
    for (const Clip &clip : m_clips) {
        QJsonObject item;
        item.insert("path", clip.path);
        item.insert("track", clip.track);
        result.append(item);
    }
    return result;
}

void TimelineWidget::fromJson(const QJsonArray &clips)
{
    m_clips.clear();
    m_selectedClip = -1;
    for (const QJsonValue &value : clips) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const QString path = item.value("path").toString();
        const int track = item.value("track").toInt(0);
        if (!path.isEmpty() && track >= 0 && track < 5)
            m_clips.append({path, track});
    }
    setMinimumWidth(qMax(1000, kClipStart + m_clips.size() * kClipStep + 24));
    updateGeometry();
    update();
}

QRect TimelineWidget::clipRect(int index) const
{
    if (index < 0 || index >= m_clips.size()) return {};
    const int track = m_clips.at(index).track;
    return QRect(kClipStart + index * kClipStep,
                 kTrackTop + track * (kTrackHeight + kTrackGap),
                 kClipWidth, kTrackHeight);
}

void TimelineWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#10151b"));
    p.fillRect(QRect(0, 0, width(), 32), QColor("#171e26"));
    p.setPen(QColor("#81909e"));
    p.drawText(QRect(12, 0, 108, 32), Qt::AlignVCenter, "TRACKS");

    for (int x = kClipStart; x < width(); x += 50) {
        p.setPen(QColor("#26313b"));
        p.drawLine(x, 32, x, height() - 8);
        if ((x - kClipStart) % 100 == 0) {
            p.setPen(QColor("#82909c"));
            p.drawText(x + 3, 21, QString::number((x - kClipStart) / 10) + "s");
        }
    }

    const QStringList tracks = {"VIDEO 1", "VIDEO 2", "VIDEO 3", "AUDIO 1", "AUDIO 2"};
    for (int i = 0; i < tracks.size(); ++i) {
        const int y = kTrackTop + i * (kTrackHeight + kTrackGap);
        p.fillRect(QRect(8, y, 110, kTrackHeight), QColor("#1b242e"));
        p.fillRect(QRect(126, y, width() - 134, kTrackHeight), QColor("#151c24"));
        p.setPen(QColor("#b3c0cc"));
        p.drawText(QRect(16, y, 100, kTrackHeight), Qt::AlignVCenter, tracks.at(i));
    }

    for (int i = 0; i < m_clips.size(); ++i) {
        const QRect bounds = clipRect(i);
        const bool selected = i == m_selectedClip;
        p.setPen(QPen(selected ? QColor("#63e3d6") : QColor("#2c9b9b"), 1));
        p.setBrush(selected ? QColor("#146f76") : QColor("#174b55"));
        p.drawRoundedRect(bounds, 4, 4);
        const QString name = QFileInfo(m_clips.at(i).path).fileName();
        p.setPen(QColor("#eef7f7"));
        p.drawText(bounds.adjusted(8, 0, -6, 0), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(name, Qt::ElideRight, bounds.width() - 16));
    }

    if (m_clips.isEmpty()) {
        p.setPen(QColor("#667481"));
        p.drawText(QRect(kClipStart, kTrackTop, width() - kClipStart - 20, 32),
                   Qt::AlignVCenter, "Add media from the Media panel to begin.");
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        for (int i = 0; i < m_clips.size(); ++i) {
            if (clipRect(i).contains(event->pos())) {
                m_selectedClip = i;
                update();
                emit clipActivated(m_clips.at(i).path);
                return;
            }
        }
        m_selectedClip = -1;
        update();
    }
    QWidget::mousePressEvent(event);
}
