#pragma once
#include <QJsonArray>
#include <QWidget>
#include <QVector>

class QMouseEvent;
class QPaintEvent;
class QRect;

class TimelineWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget *parent = nullptr);
    void addClip(const QString &path);
    void clearClips();
    QJsonArray toJson() const;
    void fromJson(const QJsonArray &clips);

signals:
    void clipActivated(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    struct Clip { QString path; int track = 0; };
    QRect clipRect(int index) const;
    QVector<Clip> m_clips;
    int m_selectedClip = -1;
};
