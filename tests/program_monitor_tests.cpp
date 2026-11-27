#include "programmonitorwidget.h"

#include <QApplication>
#include <QColor>
#include <QImage>

#include <cassert>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    ProgramMonitorWidget monitor;
    monitor.resize(480, 270);

    QImage redFrame(8, 8, QImage::Format_ARGB32);
    redFrame.fill(qRgb(240, 20, 10));
    monitor.setFrame(0, redFrame);

    QImage rendered(monitor.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor redPixel = rendered.pixelColor(rendered.width() / 2,
                                                 rendered.height() / 2);
    assert(redPixel.red() > 200 && redPixel.green() < 40 && redPixel.blue() < 30);

    QImage blueFrame(8, 8, QImage::Format_ARGB32);
    blueFrame.fill(qRgb(10, 30, 240));
    monitor.setFrame(1, blueFrame);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor bluePixel = rendered.pixelColor(rendered.width() / 2,
                                                  rendered.height() / 2);
    assert(bluePixel.blue() > 200 && bluePixel.red() < 40);

    monitor.clearFrame(1);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor restoredRed = rendered.pixelColor(rendered.width() / 2,
                                                     rendered.height() / 2);
    assert(restoredRed.red() > 200 && restoredRed.blue() < 30);

    monitor.setTrackColorGrade(0, -50, 0, 100, 0, 0, 0, 0, 0, 0);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor gradedPixel = rendered.pixelColor(rendered.width() / 2,
                                                    rendered.height() / 2);
    assert(gradedPixel.red() < restoredRed.red());

    // Opacity must affect the composited program output, not the cached source frame.
    monitor.setTrackOpacity(0, 0);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor transparentPixel = rendered.pixelColor(rendered.width() / 2,
                                                         rendered.height() / 2);
    assert(transparentPixel.red() < 15 && transparentPixel.green() < 15
           && transparentPixel.blue() < 15);

    monitor.setTrackOpacity(0, 100);
    monitor.setTrackTransform(0, 100, 100, 0);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor movedPixel = rendered.pixelColor(rendered.width() / 2,
                                                   rendered.height() / 2);
    assert(movedPixel.red() < 15 && movedPixel.green() < 15
           && movedPixel.blue() < 15);

    monitor.setTrackTransform(0, 100, 0, 0);
    rendered.fill(Qt::black);
    monitor.render(&rendered);
    const QColor restoredPixel = rendered.pixelColor(rendered.width() / 2,
                                                      rendered.height() / 2);
    assert(restoredPixel.red() > 150);

    return 0;
}
