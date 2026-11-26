#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
#include <QStringList>
#include <QJsonArray>
#include <QVector>
#include <array>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTimer;
class QVideoSink;
class ProgramMonitorWidget;
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
    void refreshPausedTimelinePreview();
    void stepSequenceFrame(int direction);
    void syncVideoTracks();
    void handleVideoTrackStatus(int track);
    void syncAudioTracks();
    void handleAudioTrackStatus(int track);
    void applyAudioMixerState();
    void undoTimelineEdit();
    void redoTimelineEdit();
    void resetTimelineHistory();

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
    ProgramMonitorWidget *m_monitor = nullptr;
    QVideoSink *m_sourceSink = nullptr;
    QSlider *m_seekSlider = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_sequenceButton = nullptr;
    QLabel *m_sequenceTimeLabel = nullptr;
    QTimer *m_sequenceTimer = nullptr;
    QElapsedTimer m_sequenceClock;
    qint64 m_sequenceClockOriginMs = 0;
    bool m_sequencePlaying = false;
    qint64 m_sequencePositionMs = 0;
    std::array<QMediaPlayer *, 3> m_videoTrackPlayers{};
    std::array<QVideoSink *, 3> m_videoTrackSinks{};
    std::array<int, 3> m_activeVideoClipIds{{-1, -1, -1}};
    std::array<bool, 3> m_waitingForVideoTrack{{false, false, false}};
    std::array<bool, 3> m_failedVideoTrack{{false, false, false}};
    std::array<QMediaPlayer *, 2> m_audioTrackPlayers{};
    std::array<QAudioOutput *, 2> m_audioTrackOutputs{};
    std::array<int, 2> m_activeAudioClipIds{{-1, -1}};
    std::array<bool, 2> m_waitingForAudioTrack{{false, false}};
    std::array<bool, 2> m_failedAudioTrack{{false, false}};
    std::array<QSlider *, 2> m_audioTrackFaders{};
    std::array<float, 2> m_audioTrackGains{{1.0f, 1.0f}};
    std::array<bool, 2> m_audioTrackMuted{{false, false}};
    std::array<bool, 2> m_audioTrackSolo{{false, false}};
    QSlider *m_masterVolumeSlider = nullptr;
    float m_masterGain = 1.0f;
    TimelineWidget *m_timeline = nullptr;
    std::array<QSlider *, 9> m_clipColorSliders{};
    std::array<QSlider *, 3> m_clipTransformSliders{};
    int m_selectedTimelineClipId = -1;
    QVector<QJsonArray> m_undoHistory;
    QVector<QJsonArray> m_redoHistory;
    QJsonArray m_lastTimelineState;
    bool m_restoringTimeline = false;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
};
