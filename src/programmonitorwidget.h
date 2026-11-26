#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <array>

class QPaintEvent;

class ProgramMonitorWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit ProgramMonitorWidget(QWidget *parent = nullptr);

    void setFrame(int track, const QImage &frame);
    void clearFrame(int track);
    void clearAllFrames();
    void setCaption(const QString &caption);
    void setColorCorrection(int brightness, int contrast, int saturation);
    void setColorWheels(int shadows, int midtones, int highlights);
    void setRgbCurves(int red, int green, int blue);
    void setTrackColorGrade(int track, int brightness, int contrast, int saturation,
                            int shadows, int midtones, int highlights,
                            int red, int green, int blue);
    void setTrackTransform(int track, int scale, int positionX, int positionY);
    void setTrackOpacity(int track, int opacity);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildCorrectedFrames();

    std::array<QImage, 3> m_frames;
    std::array<QImage, 3> m_correctedFrames;
    struct ColorGrade {
        int brightness = 0, contrast = 0, saturation = 100;
        int shadows = 0, midtones = 0, highlights = 0;
        int red = 0, green = 0, blue = 0;
    };
    std::array<ColorGrade, 3> m_colorGrades;
    struct TrackTransform { int scale = 100; int x = 0; int y = 0; int opacity = 100; };
    std::array<TrackTransform, 3> m_transforms;
    QString m_caption;
};
