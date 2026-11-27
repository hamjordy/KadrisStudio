#pragma once
#include <QMainWindow>
#include <QComboBox>
#include <QElapsedTimer>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <array>

class QAudioOutput;
class QCheckBox;
class QLabel;
class QMediaPlayer;
class QProcess;
class QProgressDialog;
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
    void autosaveProject();
    void openRecoveryCenter();
    void importMedia();
    void relinkSelectedMedia();
    void collectProjectMedia();
    void exportVideo();
    void setSequenceFrameRate();
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
    QJsonObject captureTimelineState() const;
    void restoreTimelineState(const QJsonObject &state);

private:
    void buildInterface();
    void buildMenus();
    void addMediaPath(const QString &path);
    void loadPreview(const QString &path);
    bool writeProject(const QString &path);
    bool createProjectBackup(const QString &path);
    bool readProject(const QString &path);
    void updateWindowTitle();
    QString untitledRecoveryPath() const;
    void recoverUntitledProject();
    void updatePosition(qint64 position);
    void updateDuration(qint64 duration);
    QString formatTime(qint64 milliseconds) const;
    void updateSequenceTimeLabel();
    bool openSelectedNestedSequence();
    bool returnToParentSequence();

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
    QTimer *m_autosaveTimer = nullptr;
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
    QSlider *m_clipOpacitySlider = nullptr;
    QComboBox *m_clipBlendModeCombo = nullptr;
    QSlider *m_clipAudioVolumeSlider = nullptr;
    QCheckBox *m_clipAudioMuteCheckBox = nullptr;
    QSlider *m_clipFadeInSlider = nullptr;
    QSlider *m_clipFadeOutSlider = nullptr;
    int m_selectedTimelineClipId = -1;
    QVector<QJsonObject> m_undoHistory;
    QVector<QJsonObject> m_redoHistory;
    QJsonObject m_lastTimelineState;
    bool m_restoringTimeline = false;
    bool m_projectDirty = false;
    struct NestedEditContext {
        QJsonObject parentState;
        int clipId = -1;
        QString name;
    };
    QVector<NestedEditContext> m_nestedEditStack;
    int m_sequenceFpsNumerator = 25;
    int m_sequenceFpsDenominator = 1;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QProcess *m_exportProcess = nullptr;
    QProgressDialog *m_exportProgress = nullptr;
    bool m_exportCancelled = false;
};
