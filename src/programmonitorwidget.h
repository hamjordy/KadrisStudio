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

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildCorrectedFrames();

    std::array<QImage, 3> m_frames;
    std::array<QImage, 3> m_correctedFrames;
    int m_brightness = 0;
    int m_contrast = 0;
    int m_saturation = 100;
    QString m_caption;
};
