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

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::array<QImage, 3> m_frames;
    QString m_caption;
};
