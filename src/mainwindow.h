#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
#include <QStringList>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTimer;
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
    void toggleSequencePlayback();
    void stopSequencePlayback(bool resetToStart = true);
    void tickSequencePlayback();
    void activateSequencePosition();
    void handleSequenceMediaStatus();

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
    void updateSequenceTimeLabel();

    QString m_projectPath;
    QStringList m_mediaPaths;
    QTreeWidget *m_mediaList = nullptr;
    QLabel *m_inspectorLabel = nullptr;
    QLabel *m_sourceLabel = nullptr;
    QLabel *m_timeLabel = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QSlider *m_seekSlider = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_sequenceButton = nullptr;
    QLabel *m_sequenceTimeLabel = nullptr;
    QTimer *m_sequenceTimer = nullptr;
    QElapsedTimer m_sequenceClock;
    bool m_sequencePlaying = false;
    bool m_waitingForSequenceMedia = false;
    int m_activeSequenceClipId = -1;
    qint64 m_sequencePositionMs = 0;
    qint64 m_pendingSequenceSourceMs = 0;
    qint64 m_activeSequenceClipEndMs = 0;
    TimelineWidget *m_timeline = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
};
