#include "programmonitorwidget.h"

#include <QColor>

#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <cmath>

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
    rebuildCorrectedFrame(track);
    update();
}

void ProgramMonitorWidget::clearFrame(int track)
{
    if (track < 0 || track >= static_cast<int>(m_frames.size()))
        return;

    m_frames[static_cast<size_t>(track)] = QImage();
    m_correctedFrames[static_cast<size_t>(track)] = QImage();
    update();
}

void ProgramMonitorWidget::clearAllFrames()
{
    for (QImage &frame : m_frames)
        frame = QImage();
    for (QImage &frame : m_correctedFrames)
        frame = QImage();
    update();
}

void ProgramMonitorWidget::setCaption(const QString &caption)
{
    m_caption = caption;
    update();
}

void ProgramMonitorWidget::setColorCorrection(int brightness, int contrast, int saturation)
{
    for (int track = 0; track < kLayerCount; ++track) {
        const ColorGrade &grade = m_colorGrades[static_cast<size_t>(track)];
        setTrackColorGrade(track, brightness, contrast, saturation,
            grade.shadows, grade.midtones, grade.highlights,
            grade.red, grade.green, grade.blue);
    }
}

void ProgramMonitorWidget::setColorWheels(int shadows, int midtones, int highlights)
{
    for (int track = 0; track < 3; ++track) {
        const ColorGrade &grade = m_colorGrades[static_cast<size_t>(track)];
        setTrackColorGrade(track, grade.brightness, grade.contrast, grade.saturation,
            shadows, midtones, highlights, grade.red, grade.green, grade.blue);
    }
}

void ProgramMonitorWidget::setRgbCurves(int red, int green, int blue)
{
    for (int track = 0; track < 3; ++track) {
        const ColorGrade &grade = m_colorGrades[static_cast<size_t>(track)];
        setTrackColorGrade(track, grade.brightness, grade.contrast, grade.saturation,
            grade.shadows, grade.midtones, grade.highlights, red, green, blue);
    }
}

void ProgramMonitorWidget::setTrackColorGrade(int track, int brightness, int contrast,
    int saturation, int shadows, int midtones, int highlights, int red, int green, int blue)
{
    if (track < 0 || track >= kLayerCount) return;
    ColorGrade &grade = m_colorGrades[static_cast<size_t>(track)];
    const ColorGrade next{
        qBound(-100, brightness, 100), qBound(-100, contrast, 100),
        qBound(0, saturation, 200), qBound(-100, shadows, 100),
        qBound(-100, midtones, 100), qBound(-100, highlights, 100),
        qBound(-100, red, 100), qBound(-100, green, 100),
        qBound(-100, blue, 100)
    };
    if (grade.brightness == next.brightness && grade.contrast == next.contrast
        && grade.saturation == next.saturation && grade.shadows == next.shadows
        && grade.midtones == next.midtones && grade.highlights == next.highlights
        && grade.red == next.red && grade.green == next.green && grade.blue == next.blue)
        return;
    grade = next;
    rebuildCorrectedFrame(track);
    update();
}


void ProgramMonitorWidget::setTrackTransform(int track, int scale, int positionX, int positionY)
{
    if (track < 0 || track >= 3) return;
    TrackTransform &transform = m_transforms[static_cast<size_t>(track)];
    const TrackTransform next{qBound(10, scale, 300),
        qBound(-100, positionX, 100), qBound(-100, positionY, 100)};
    if (transform.scale == next.scale && transform.x == next.x && transform.y == next.y)
        return;
    transform = next;
    update();
}


void ProgramMonitorWidget::setTrackOpacity(int track, int opacity)
{
    if (track < 0 || track >= 3) return;
    TrackTransform &transform = m_transforms[static_cast<size_t>(track)];
    const int nextOpacity = qBound(0, opacity, 100);
    if (transform.opacity == nextOpacity) return;
    transform.opacity = nextOpacity;
    update();
}


void ProgramMonitorWidget::setTrackBlendMode(int track, int blendMode)
{
    if (track < 0 || track >= 3) return;
    TrackTransform &transform = m_transforms[static_cast<size_t>(track)];
    const int nextMode = qBound(0, blendMode, 3);
    if (transform.blendMode == nextMode) return;
    transform.blendMode = nextMode;
    update();
}


void ProgramMonitorWidget::rebuildCorrectedFrames()
{
    for (int track = 0; track < static_cast<int>(m_frames.size()); ++track)
        rebuildCorrectedFrame(track);
}

void ProgramMonitorWidget::setImageEffects(int blur, int sharpen, int vignette)
{
    m_blur = qBound(0, blur, 100);
    m_sharpen = qBound(0, sharpen, 100);
    m_vignette = qBound(0, vignette, 100);
    rebuildCorrectedFrames();
    update();
}

void ProgramMonitorWidget::rebuildCorrectedFrame(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= static_cast<int>(m_frames.size())) return;
    if (m_frames[static_cast<size_t>(frameIndex)].isNull()) {
        m_correctedFrames[static_cast<size_t>(frameIndex)] = QImage();
        return;
    }
        const ColorGrade &grade = m_colorGrades[static_cast<size_t>(frameIndex)];
        const double contrastFactor = (259.0 * (grade.contrast + 255.0))
            / (255.0 * (259.0 - grade.contrast));
        QImage image = m_frames[static_cast<size_t>(frameIndex)].convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = line[x];
                int red = qBound(0, qRound(contrastFactor * (qRed(pixel) - 128) + 128)
                                     + grade.brightness, 255);
                int green = qBound(0, qRound(contrastFactor * (qGreen(pixel) - 128) + 128)
                                       + grade.brightness, 255);
                int blue = qBound(0, qRound(contrastFactor * (qBlue(pixel) - 128) + 128)
                                      + grade.brightness, 255);
                const auto applyCurve = [](int channel, int amount) {
                    const double gamma = std::pow(2.0, -amount / 100.0);
                    const double normalized = channel / 255.0;
                    return qBound(0, qRound(std::pow(normalized, gamma) * 255.0), 255);
                };
                red = applyCurve(red, grade.red);
                green = applyCurve(green, grade.green);
                blue = applyCurve(blue, grade.blue);
                const double luminance = (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255.0;
                const double shadowWeight = qBound(0.0, (0.65 - luminance) / 0.65, 1.0);
                const double highlightWeight = qBound(0.0, (luminance - 0.35) / 0.65, 1.0);
                const double midtoneWeight = qBound(0.0, 1.0 - std::abs(luminance - 0.5) / 0.5, 1.0);
                const double warmth = (grade.shadows * shadowWeight
                    + grade.midtones * midtoneWeight
                    + grade.highlights * highlightWeight) / 100.0;
                red = qBound(0, red + qRound(warmth * 42.0), 255);
                blue = qBound(0, blue - qRound(warmth * 42.0), 255);
                if (grade.saturation != 100) {
                    QColor color(red, green, blue);
                    int hue = 0, sat = 0, value = 0, alpha = 255;
                    color.getHsv(&hue, &sat, &value, &alpha);
                    color.setHsv(hue, qBound(0, sat * grade.saturation / 100, 255),
                                 value, alpha);
                    red = color.red(); green = color.green(); blue = color.blue();
                }
                line[x] = qRgba(red, green, blue, qAlpha(pixel));
            }
        }
        if (m_blur > 0 || m_sharpen > 0) {
            const QImage source = image;
            for (int y = 1; y < image.height() - 1; ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 1; x < image.width() - 1; ++x) {
                    const QRgb center = source.pixel(x, y);
                    int sums[3] = {0, 0, 0};
                    int neighbors[3] = {0, 0, 0};
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const QRgb p = source.pixel(x + dx, y + dy);
                            sums[0] += qRed(p); sums[1] += qGreen(p); sums[2] += qBlue(p);
                            if (std::abs(dx) + std::abs(dy) == 1) {
                                neighbors[0] += qRed(p); neighbors[1] += qGreen(p); neighbors[2] += qBlue(p);
                            }
                        }
                    }
                    int channels[3] = {qRed(center), qGreen(center), qBlue(center)};
                    for (int c = 0; c < 3; ++c) {
                        const double blurred = (sums[c] + channels[c]) / 9.0;
                        const double blurMix = m_blur / 100.0;
                        const double sharpenMix = m_sharpen / 100.0;
                        const double sharpened = channels[c] * 2.0 - neighbors[c] / 4.0;
                        channels[c] = qBound(0, qRound(channels[c] * (1.0 - blurMix)
                            + blurred * blurMix + (sharpened - channels[c]) * sharpenMix), 255);
                    }
                    line[x] = qRgba(channels[0], channels[1], channels[2], qAlpha(center));
                }
            }
        }
        if (m_vignette > 0) {
            const double cx = (image.width() - 1) / 2.0;
            const double cy = (image.height() - 1) / 2.0;
            const double maxDistance = std::sqrt(cx * cx + cy * cy);
            for (int y = 0; y < image.height(); ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const double distance = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    const double amount = (m_vignette / 100.0) * qBound(0.0, distance / maxDistance, 1.0) * 0.72;
                    const QRgb p = line[x];
                    line[x] = qRgba(qRound(qRed(p) * (1.0 - amount)),
                        qRound(qGreen(p) * (1.0 - amount)),
                        qRound(qBlue(p) * (1.0 - amount)), qAlpha(p));
                }
            }
        }
        m_correctedFrames[static_cast<size_t>(frameIndex)] = image;
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
    for (size_t trackIndex = 0; trackIndex < m_correctedFrames.size(); ++trackIndex) {
        const QImage &frame = m_correctedFrames[trackIndex];
        if (frame.isNull()) continue;

        const QSize scaled = frame.size().scaled(
            target.size().toSize(), Qt::KeepAspectRatio);
        const TrackTransform &transform = m_transforms[trackIndex];
        const double width = scaled.width() * transform.scale / 100.0;
        const double height = scaled.height() * transform.scale / 100.0;
        const QPointF center(target.center().x() + target.width() * transform.x / 100.0,
                             target.center().y() + target.height() * transform.y / 100.0);
        const QRectF destination(center.x() - width / 2.0, center.y() - height / 2.0,
                                 width, height);
        painter.setCompositionMode(transform.blendMode == 1
            ? QPainter::CompositionMode_Multiply
            : transform.blendMode == 2
                ? QPainter::CompositionMode_Screen
                : transform.blendMode == 3
                    ? QPainter::CompositionMode_Overlay
                    : QPainter::CompositionMode_SourceOver);
        painter.setOpacity(transform.opacity / 100.0);
        painter.drawImage(destination, frame);
        painter.setOpacity(1.0);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        drewFrame = true;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setPen(QPen(QColor("#65419b"), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    if (!drewFrame) {
        QFont heading = painter.font();
        heading.setPointSize(10);
        heading.setWeight(QFont::DemiBold);
        painter.setFont(heading);
        painter.setPen(QColor("#c6a5f5"));
        painter.drawText(rect().adjusted(20, 20, -20, -20),
                         Qt::AlignCenter,
                         m_caption.isEmpty() ? QStringLiteral("KADRIS STUDIO") : m_caption);
    }

    QStringList activeTracks;
    for (size_t i = 0; i < m_frames.size(); ++i) {
        if (!m_frames[i].isNull())
            activeTracks.append(QString("L%1").arg(static_cast<int>(i) + 1));
    }

    if (!activeTracks.isEmpty()) {
        QFont labelFont = painter.font();
        labelFont.setPointSize(8);
        painter.setFont(labelFont);
        const QString label = QString("VIDEO LAYERS  %1").arg(activeTracks.join("  "));
        const QRect labelRect(rect().left() + 16, rect().bottom() - 34, 210, 22);
        painter.fillRect(labelRect.adjusted(-6, -2, 6, 2), QColor(24, 16, 36, 205));
        painter.setPen(QColor(234, 220, 255, 220));
        painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}
