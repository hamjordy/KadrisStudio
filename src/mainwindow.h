#pragma once
#include <QMainWindow>
#include <QStringList>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QTreeWidget;
class QVideoWidget;
class TimelineWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    void importMedia();
    void addSelectedMediaToTimeline();
    void playPause();
    void stopPlayback();

private:
    void buildInterface();
    void buildMenus();
    void addMediaPath(const QString &path);
    void loadPreview(const QString &path);
    bool writeProject(const QString &path);
    bool readProject(const QString &path);
    void updateWindowTitle();
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    QString formatTime(qint64 milliseconds) const;

    QString m_projectPath;
    QStringList m_mediaPaths;
    QTreeWidget *m_mediaList = nullptr;
    QLabel *m_inspectorLabel = nullptr;
    QLabel *m_sourceLabel = nullptr;
    QLabel *m_timeLabel = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QSlider *m_seekSlider = nullptr;
    QPushButton *m_playButton = nullptr;
    TimelineWidget *m_timeline = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
};
