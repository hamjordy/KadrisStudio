#include "programmonitorwidget.h"

#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QRectF>
#include <QStringList>

ProgramMonitorWidget::ProgramMonitorWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("programMonitor");
    setMinimumSize(480, 270);
    setAutoFillBackground(false);
}

void ProgramMonitorWidget::setFrame(int track, const QImage &frame)
{
    if (track < 0 || track >= static_cast<int>(m_frames.size()) || frame.isNull())
        return;

    m_frames[static_cast<size_t>(track)] = frame;
    update();
}

void ProgramMonitorWidget::clearFrame(int track)
{
    if (track < 0 || track >= static_cast<int>(m_frames.size()))
        return;

    m_frames[static_cast<size_t>(track)] = QImage();
    update();
}

void ProgramMonitorWidget::clearAllFrames()
{
    for (QImage &frame : m_frames)
        frame = QImage();
    update();
}

void ProgramMonitorWidget::setCaption(const QString &caption)
{
    m_caption = caption;
    update();
}

void ProgramMonitorWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor("#05090e"));

    const QRectF target = QRectF(rect()).adjusted(8, 8, -8, -8);
    bool drewFrame = false;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Lower-numbered tracks are painted first; higher tracks composite above them.
    for (const QImage &frame : m_frames) {
        if (frame.isNull()) continue;

        const QSize scaled = frame.size().scaled(
            target.size().toSize(), Qt::KeepAspectRatio);
        const QRectF destination(
            target.center().x() - scaled.width() / 2.0,
            target.center().y() - scaled.height() / 2.0,
            scaled.width(), scaled.height());
        painter.drawImage(destination, frame);
        drewFrame = true;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setPen(QPen(QColor("#263b48"), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    if (!drewFrame) {
        QFont heading = painter.font();
        heading.setPointSize(10);
        heading.setWeight(QFont::DemiBold);
        painter.setFont(heading);
        painter.setPen(QColor("#93a4b1"));
        painter.drawText(rect().adjusted(20, 20, -20, -20),
                         Qt::AlignCenter,
                         m_caption.isEmpty() ? QStringLiteral("KADRIS STUDIO") : m_caption);
    }

    QStringList activeTracks;
    for (size_t i = 0; i < m_frames.size(); ++i) {
        if (!m_frames[i].isNull())
            activeTracks.append(QString("V%1").arg(static_cast<int>(i) + 1));
    }

    if (!activeTracks.isEmpty()) {
        QFont labelFont = painter.font();
        labelFont.setPointSize(8);
        painter.setFont(labelFont);
        const QString label = QString("VIDEO LAYERS  %1").arg(activeTracks.join("  "));
        const QRect labelRect(rect().left() + 16, rect().bottom() - 34, 210, 22);
        painter.fillRect(labelRect.adjusted(-6, -2, 6, 2), QColor(4, 12, 18, 175));
        painter.setPen(QColor(220, 232, 236, 190));
        painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}
