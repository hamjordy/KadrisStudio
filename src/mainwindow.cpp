#include "mainwindow.h"
#include "timelinewidget.h"
#include "programmonitorwidget.h"
#include "frame_timebase.h"
#include "project_media_paths.h"
#include "project_media_collection.h"
#include "ffmpeg_export.h"

#include <QAction>
#include <QCheckBox>
#include <QAudioOutput>
#include <QDockWidget>
#include <QDialog>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QMenuBar>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QProgressDialog>
#include <QPointer>
#include <QScrollArea>
#include <QSlider>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVideoSink>
#include <QVideoFrame>
#include <QUrl>

#include <cmath>

namespace {
float faderToLinearGain(int value)
{
    if (value <= 0) return 0.0f;
    const double decibels = (static_cast<double>(value) / 100.0 - 1.0) * 60.0;
    return static_cast<float>(std::pow(10.0, decibels / 20.0));
}

QString faderToLabel(int value)
{
    if (value <= 0) return QStringLiteral("-INF dB");
    const double decibels = (static_cast<double>(value) / 100.0 - 1.0) * 60.0;
    return QStringLiteral("%1 dB").arg(decibels, 0, 'f', 1);
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    m_sourceSink = new QVideoSink(this);
    m_player->setVideoSink(m_sourceSink);

    connect(m_sourceSink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame &frame) {
            if (!m_sequencePlaying && m_monitor && frame.isValid())
                m_monitor->setFrame(0, frame.toImage());
        });

    for (int track = 0; track < 3; ++track) {
        m_videoTrackPlayers[track] = new QMediaPlayer(this);
        m_videoTrackSinks[track] = new QVideoSink(this);
        m_videoTrackPlayers[track]->setVideoSink(m_videoTrackSinks[track]);

        connect(m_videoTrackSinks[track], &QVideoSink::videoFrameChanged, this,
            [this, track](const QVideoFrame &frame) {
                if (m_monitor && frame.isValid()
                    && (m_sequencePlaying || m_activeVideoClipIds[track] >= 0))
                    m_monitor->setFrame(track, frame.toImage());
            });
        connect(m_videoTrackPlayers[track], &QMediaPlayer::mediaStatusChanged, this,
            [this, track](QMediaPlayer::MediaStatus) {
                handleVideoTrackStatus(track);
            });
        connect(m_videoTrackPlayers[track], &QMediaPlayer::errorOccurred, this,
            [this, track](QMediaPlayer::Error, const QString &message) {
                m_waitingForVideoTrack[track] = false;
                m_failedVideoTrack[track] = true;
                if (m_monitor) m_monitor->clearFrame(track);
                statusBar()->showMessage(
                    QString("Video track %1: %2").arg(track + 1).arg(message), 6000);
            });
    }

    for (int track = 0; track < 2; ++track) {
        m_audioTrackPlayers[track] = new QMediaPlayer(this);
        m_audioTrackOutputs[track] = new QAudioOutput(this);
        m_audioTrackOutputs[track]->setVolume(1.0f);
        m_audioTrackPlayers[track]->setAudioOutput(m_audioTrackOutputs[track]);

        connect(m_audioTrackPlayers[track], &QMediaPlayer::mediaStatusChanged, this,
            [this, track](QMediaPlayer::MediaStatus) {
                handleAudioTrackStatus(track);
            });
        connect(m_audioTrackPlayers[track], &QMediaPlayer::errorOccurred, this,
            [this, track](QMediaPlayer::Error, const QString &message) {
                m_waitingForAudioTrack[track] = false;
                m_failedAudioTrack[track] = true;
                statusBar()->showMessage(
                    QString("Audio track %1: %2").arg(track + 1).arg(message), 7000);
            });
    }

    buildInterface();
    applyAudioMixerState();
    buildMenus();

    connect(m_player, &QMediaPlayer::positionChanged, this, &MainWindow::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MainWindow::updateDuration);
    connect(m_player, &QMediaPlayer::errorOccurred, this,
        [this](QMediaPlayer::Error, const QString &message) {
            statusBar()->showMessage(QString("Source playback error: %1").arg(message), 8000);
        });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
        [this](QMediaPlayer::PlaybackState state) {
            m_playButton->setText(state == QMediaPlayer::PlayingState ? "Pause" : "Source Play");
        });

    m_sequenceTimer = new QTimer(this);
    m_sequenceTimer->setInterval(16);
    connect(m_sequenceTimer, &QTimer::timeout, this, &MainWindow::tickSequencePlayback);

    // Keep a crash-recovery snapshot beside the saved project once per minute.
    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setInterval(60000);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosaveProject);
    m_autosaveTimer->start();

    // Offer recovery of an unsaved session from the previous application run.
    QTimer::singleShot(0, this, &MainWindow::recoverUntitledProject);

    connect(m_timeline, &TimelineWidget::clipActivated, this,
        [this](const QString &path) {
            m_inspectorLabel->setText(QString("Timeline clip: %1\n\n"
                "Use the timeline controls to move, trim, split, or remove this clip.")
                .arg(QFileInfo(path).fileName()));
        });
    connect(m_timeline, &TimelineWidget::clipSelectionChanged, this,
        [this](int clipId, const QString &path, int track) {
            m_selectedTimelineClipId = clipId;
            if (clipId < 0) {
                m_inspectorLabel->setText("No timeline clip selected. Color controls are stored per clip.");
                return;
            }
            TimelineWidget::ClipInfo clip;
            if (!m_timeline->selectedClipInfo(&clip)) return;
            const QSignalBlocker b0(m_clipColorSliders[0]);
            const QSignalBlocker b1(m_clipColorSliders[1]);
            const QSignalBlocker b2(m_clipColorSliders[2]);
            const QSignalBlocker b3(m_clipColorSliders[3]);
            const QSignalBlocker b4(m_clipColorSliders[4]);
            const QSignalBlocker b5(m_clipColorSliders[5]);
            const QSignalBlocker b6(m_clipColorSliders[6]);
            const QSignalBlocker b7(m_clipColorSliders[7]);
            const QSignalBlocker b8(m_clipColorSliders[8]);
            const QSignalBlocker t0(m_clipTransformSliders[0]);
            const QSignalBlocker t1(m_clipTransformSliders[1]);
            const QSignalBlocker t2(m_clipTransformSliders[2]);
            const QSignalBlocker opacityBlocker(m_clipOpacitySlider);
            const QSignalBlocker blendBlocker(m_clipBlendModeCombo);
            const QSignalBlocker audioVolumeBlocker(m_clipAudioVolumeSlider);
            const QSignalBlocker clipAudioMuteBlocker(m_clipAudioMuteCheckBox);
            const QSignalBlocker fadeInBlocker(m_clipFadeInSlider);
            const QSignalBlocker fadeOutBlocker(m_clipFadeOutSlider);
            m_clipColorSliders[0]->setValue(clip.brightness);
            m_clipColorSliders[1]->setValue(clip.contrast);
            m_clipColorSliders[2]->setValue(clip.saturation);
            m_clipColorSliders[3]->setValue(clip.shadows);
            m_clipColorSliders[4]->setValue(clip.midtones);
            m_clipColorSliders[5]->setValue(clip.highlights);
            m_clipColorSliders[6]->setValue(clip.redCurve);
            m_clipColorSliders[7]->setValue(clip.greenCurve);
            m_clipColorSliders[8]->setValue(clip.blueCurve);
            m_clipTransformSliders[0]->setValue(clip.scale);
            m_clipTransformSliders[1]->setValue(clip.positionX);
            m_clipTransformSliders[2]->setValue(clip.positionY);
            m_clipOpacitySlider->setValue(clip.opacity);
            m_clipBlendModeCombo->setCurrentIndex(clip.blendMode);
            m_clipAudioVolumeSlider->setValue(clip.audioVolume);
            m_clipAudioMuteCheckBox->setChecked(clip.audioMuted);
            m_clipFadeInSlider->setValue(clip.fadeInMs);
            m_clipFadeOutSlider->setValue(clip.fadeOutMs);
            if (clip.track < 3)
                m_monitor->setTrackTransform(clip.track, clip.scale, clip.positionX, clip.positionY);
            if (clip.track < 3) m_monitor->setTrackOpacity(clip.track, clip.opacity);
            if (clip.track < 3) m_monitor->setTrackBlendMode(clip.track, clip.blendMode);
            if (clip.track < 3) {
                m_monitor->setTrackColorGrade(clip.track, clip.brightness, clip.contrast,
                    clip.saturation, clip.shadows, clip.midtones, clip.highlights,
                    clip.redCurve, clip.greenCurve, clip.blueCurve);
            }
            m_inspectorLabel->setText(QString("Selected clip: %1\\nClip ID: %2\\nTrack: %3\\n\\nColor settings are saved with this clip.")
                .arg(QFileInfo(path).fileName()).arg(clipId)
                .arg(track < 3 ? QString("V%1").arg(track + 1) : QString("A%1").arg(track - 2)));
        });
    connect(m_timeline, &TimelineWidget::playheadChanged, this,
        [this](qint64 position) {
            m_sequencePositionMs = position;
            updateSequenceTimeLabel();
            if (m_sequencePlaying) {
                // A manual timeline seek must reload every active track at its new source in-point.
                m_sequenceClockOriginMs = position;
                if (m_sequenceClock.isValid()) m_sequenceClock.restart();
                else m_sequenceClock.start();

                for (int track = 0; track < 3; ++track) {
                    m_videoTrackPlayers[track]->stop();
                    m_videoTrackPlayers[track]->setSource(QUrl());
                    m_activeVideoClipIds[track] = -1;
                    m_waitingForVideoTrack[track] = false;
                    m_failedVideoTrack[track] = false;
                    m_monitor->clearFrame(track);
                }
                for (int track = 0; track < 2; ++track) {
                    m_audioTrackPlayers[track]->stop();
                    m_audioTrackPlayers[track]->setSource(QUrl());
                    m_activeAudioClipIds[track] = -1;
                    m_waitingForAudioTrack[track] = false;
                    m_failedAudioTrack[track] = false;
                }
                syncVideoTracks();
                syncAudioTracks();
            } else {
                refreshPausedTimelinePreview();
            }
        });
    connect(m_timeline, &TimelineWidget::trackStateChanged, this,
        [this](int track) {
            if (track >= 0 && track < 3 && m_sequencePlaying) syncVideoTracks();
            if (track >= 3 && track < 5) applyAudioMixerState();
            statusBar()->showMessage(QString("%1 %2")
                .arg(track < 3 ? "VIDEO" : "AUDIO")
                .arg(track < 3 ? track + 1 : track - 2) + " track settings updated", 2500);
        });
        m_lastTimelineState = captureTimelineState();
    connect(m_timeline, &TimelineWidget::timelineEdited, this,
        [this]() {
            const QJsonObject currentState = captureTimelineState();
            if (!m_restoringTimeline && currentState != m_lastTimelineState) {
                m_projectDirty = true;
                m_undoHistory.append(m_lastTimelineState);
                if (m_undoHistory.size() > 100) m_undoHistory.removeFirst();
                m_redoHistory.clear();
                m_lastTimelineState = currentState;
            }
            if (m_sequencePlaying) {
                stopSequencePlayback(false);
                statusBar()->showMessage("Timeline changed — playback stopped", 4000);
            }
        });

    updateWindowTitle();
    statusBar()->showMessage("Ready — create a project or import media");
    resize(1440, 900);
}

void MainWindow::buildInterface()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 10, 12, 10);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel("PROGRAM MONITOR");
    title->setObjectName("sectionTitle");
    m_sourceLabel = new QLabel("No source loaded");
    m_sourceLabel->setObjectName("mutedLabel");
    header->addWidget(title);
    header->addStretch();
    header->addWidget(m_sourceLabel);
    layout->addLayout(header);

    m_monitor = new ProgramMonitorWidget(central);
    m_monitor->setMinimumSize(480, 270);
    m_monitor->setCaption("Source and timeline preview");
    layout->addWidget(m_monitor, 1);

    auto *transport = new QHBoxLayout();
    m_playButton = new QPushButton("Source Play");
    m_playButton->setObjectName("primaryButton");
    auto *stopSource = new QPushButton("Stop Source");
    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setRange(0, 0);
    m_timeLabel = new QLabel("00:00 / 00:00");
    m_timeLabel->setObjectName("timecodeLabel");

    m_sequenceButton = new QPushButton("Play Timeline");
    m_sequenceButton->setObjectName("primaryButton");
    auto *stopSequence = new QPushButton("Stop Timeline");
    m_sequenceTimeLabel = new QLabel("00:00:00:00");
    m_sequenceTimeLabel->setObjectName("timecodeLabel");

    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::playPause);
    connect(stopSource, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    connect(m_seekSlider, &QSlider::sliderMoved, this,
        [this](int value) { m_player->setPosition(value); });
    connect(m_sequenceButton, &QPushButton::clicked,
            this, &MainWindow::toggleSequencePlayback);
    connect(stopSequence, &QPushButton::clicked, this,
            [this]() { stopSequencePlayback(true); });

    transport->addWidget(m_playButton);
    transport->addWidget(stopSource);
    transport->addWidget(m_seekSlider, 1);
    transport->addWidget(m_timeLabel);
    layout->addLayout(transport);

    auto *sequenceTransport = new QHBoxLayout();
    auto *sequenceLabel = new QLabel("SEQUENCE");
    sequenceLabel->setObjectName("sectionTitle");
    sequenceTransport->addWidget(sequenceLabel);
    sequenceTransport->addWidget(m_sequenceButton);
    sequenceTransport->addWidget(stopSequence);
    sequenceTransport->addStretch();
    sequenceTransport->addWidget(new QLabel("TIMELINE POSITION"));
    sequenceTransport->addWidget(m_sequenceTimeLabel);
    layout->addLayout(sequenceTransport);
    setCentralWidget(central);

    auto *mediaDock = new QDockWidget("Media", this);
    mediaDock->setObjectName("mediaDock");
    mediaDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto *mediaPanel = new QWidget(mediaDock);
    auto *mediaLayout = new QVBoxLayout(mediaPanel);
    auto *buttons = new QHBoxLayout();
    auto *importButton = new QPushButton("Import");
    auto *addButton = new QPushButton("Add to timeline");
    connect(importButton, &QPushButton::clicked, this, &MainWindow::importMedia);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addSelectedMediaToTimeline);
    buttons->addWidget(importButton);
    buttons->addWidget(addButton);
    mediaLayout->addLayout(buttons);

    m_mediaList = new QTreeWidget(mediaPanel);
    m_mediaList->setHeaderLabels({"Name", "Format"});
    m_mediaList->setRootIsDecorated(false);
    m_mediaList->setAlternatingRowColors(true);
    m_mediaList->setColumnWidth(0, 180);
    connect(m_mediaList, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem *item, int) {
            if (item) loadPreview(item->data(0, Qt::UserRole).toString());
        });
    mediaLayout->addWidget(m_mediaList, 1);
    mediaDock->setWidget(mediaPanel);
    addDockWidget(Qt::LeftDockWidgetArea, mediaDock);

    auto *inspectorDock = new QDockWidget("Inspector", this);
    inspectorDock->setObjectName("inspectorDock");
    auto *inspector = new QWidget(inspectorDock);
    auto *inspectorLayout = new QVBoxLayout(inspector);
    auto *inspectorHeading = new QLabel("SOURCE INFORMATION");
    inspectorHeading->setObjectName("sectionTitle");
    m_inspectorLabel = new QLabel("Select a media item to load it in the source monitor.");
    m_inspectorLabel->setObjectName("mutedLabel");
    m_inspectorLabel->setWordWrap(true);
    m_inspectorLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    inspectorLayout->addWidget(inspectorHeading);
    inspectorLayout->addWidget(m_inspectorLabel);

    auto *colorHeading = new QLabel("COLOR CORRECTION");
    colorHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(colorHeading);
    auto *brightnessSlider = new QSlider(Qt::Horizontal);
    auto *contrastSlider = new QSlider(Qt::Horizontal);
    auto *saturationSlider = new QSlider(Qt::Horizontal);
    auto addMonitorSlider = [this, inspectorLayout, brightnessSlider, contrastSlider, saturationSlider](const QString &name,
            QSlider *slider, int minimum, int maximum, int initial) {
        auto *label = new QLabel(QString("%1: %2").arg(name).arg(initial));
        label->setObjectName("mutedLabel");
        slider->setRange(minimum, maximum);
        slider->setValue(initial);
        connect(slider, &QSlider::valueChanged, this,
            [this, label, name, brightnessSlider, contrastSlider, saturationSlider](int value) {
                label->setText(QString("%1: %2").arg(name).arg(value));
                m_monitor->setColorCorrection(brightnessSlider->value(),
                    contrastSlider->value(), saturationSlider->value());
            });
        inspectorLayout->addWidget(label);
        inspectorLayout->addWidget(slider);
    };
    addMonitorSlider("Brightness", brightnessSlider, -100, 100, 0);
    addMonitorSlider("Contrast", contrastSlider, -100, 100, 0);
    addMonitorSlider("Saturation", saturationSlider, 0, 200, 100);
    auto *wheelsHeading = new QLabel("COLOR WHEELS — WARM / COOL");
    wheelsHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(wheelsHeading);
    auto *shadowWheel = new QSlider(Qt::Horizontal);
    auto *midtoneWheel = new QSlider(Qt::Horizontal);
    auto *highlightWheel = new QSlider(Qt::Horizontal);
    auto addWheelSlider = [this, inspectorLayout, shadowWheel, midtoneWheel, highlightWheel](
            const QString &name, QSlider *slider) {
        auto *label = new QLabel(name + ": 0");
        label->setObjectName("mutedLabel");
        slider->setRange(-100, 100);
        slider->setValue(0);
        connect(slider, &QSlider::valueChanged, this,
            [this, label, name, shadowWheel, midtoneWheel, highlightWheel](int value) {
                label->setText(QString("%1: %2").arg(name).arg(value));
                m_monitor->setColorWheels(shadowWheel->value(),
                    midtoneWheel->value(), highlightWheel->value());
            });
        inspectorLayout->addWidget(label);
        inspectorLayout->addWidget(slider);
    };
    addWheelSlider("Shadows", shadowWheel);
    addWheelSlider("Midtones", midtoneWheel);
    addWheelSlider("Highlights", highlightWheel);

    auto *curvesHeading = new QLabel("RGB TONE CURVES");
    curvesHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(curvesHeading);
    auto *redCurve = new QSlider(Qt::Horizontal);
    auto *greenCurve = new QSlider(Qt::Horizontal);
    auto *blueCurve = new QSlider(Qt::Horizontal);
    m_clipColorSliders = {{brightnessSlider, contrastSlider, saturationSlider,
        shadowWheel, midtoneWheel, highlightWheel, redCurve, greenCurve, blueCurve}};
    for (QSlider *slider : m_clipColorSliders) {
        connect(slider, &QSlider::valueChanged, this, [this](int) {
            if (m_selectedTimelineClipId < 0) return;
            m_timeline->setClipColorSettings(m_selectedTimelineClipId,
                m_clipColorSliders[0]->value(), m_clipColorSliders[1]->value(),
                m_clipColorSliders[2]->value(), m_clipColorSliders[3]->value(),
                m_clipColorSliders[4]->value(), m_clipColorSliders[5]->value(),
                m_clipColorSliders[6]->value(), m_clipColorSliders[7]->value(),
                m_clipColorSliders[8]->value());
            TimelineWidget::ClipInfo clip;
            if (m_timeline->selectedClipInfo(&clip) && clip.track < 3) {
                m_monitor->setTrackColorGrade(clip.track,
                    m_clipColorSliders[0]->value(), m_clipColorSliders[1]->value(),
                    m_clipColorSliders[2]->value(), m_clipColorSliders[3]->value(),
                    m_clipColorSliders[4]->value(), m_clipColorSliders[5]->value(),
                    m_clipColorSliders[6]->value(), m_clipColorSliders[7]->value(),
                    m_clipColorSliders[8]->value());
            }
        });
    }
    auto addCurveSlider = [this, inspectorLayout, redCurve, greenCurve, blueCurve](
            const QString &name, QSlider *slider) {
        auto *label = new QLabel(name + " curve: 0");
        label->setObjectName("mutedLabel");
        slider->setRange(-100, 100);
        slider->setValue(0);
        connect(slider, &QSlider::valueChanged, this,
            [this, label, name, redCurve, greenCurve, blueCurve](int value) {
                label->setText(QString("%1 curve: %2").arg(name).arg(value));
                m_monitor->setRgbCurves(redCurve->value(),
                    greenCurve->value(), blueCurve->value());
            });
        inspectorLayout->addWidget(label);
        inspectorLayout->addWidget(slider);
    };
    addCurveSlider("Red", redCurve);
    addCurveSlider("Green", greenCurve);
    addCurveSlider("Blue", blueCurve);

    auto *resetColor = new QPushButton("Reset Color Correction");
    connect(resetColor, &QPushButton::clicked, this,
        [brightnessSlider, contrastSlider, saturationSlider, shadowWheel, midtoneWheel, highlightWheel, redCurve, greenCurve, blueCurve]() {
            brightnessSlider->setValue(0);
            contrastSlider->setValue(0);
            saturationSlider->setValue(100);
            shadowWheel->setValue(0);
            midtoneWheel->setValue(0);
            highlightWheel->setValue(0);
            redCurve->setValue(0);
            greenCurve->setValue(0);
            blueCurve->setValue(0);
        });
    inspectorLayout->addWidget(resetColor);

    auto *keyframeHeading = new QLabel("ANIMATION — BRIGHTNESS");
    keyframeHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(keyframeHeading);
    auto *keyframeCountLabel = new QLabel("Brightness keyframes: 0");
    keyframeCountLabel->setObjectName("mutedLabel");
    inspectorLayout->addWidget(keyframeCountLabel);
    auto *addKeyframeButton = new QPushButton("Add / Update Keyframe");
    auto *removeKeyframeButton = new QPushButton("Remove Nearest Keyframe");
    addKeyframeButton->setToolTip("Store the current brightness at the timeline playhead");
    removeKeyframeButton->setToolTip("Remove the nearest brightness keyframe to the playhead");
    connect(addKeyframeButton, &QPushButton::clicked, this,
        [this, keyframeCountLabel, brightnessSlider]() {
            if (m_selectedTimelineClipId < 0) {
                statusBar()->showMessage("Select a video clip before adding keyframes", 3500);
                return;
            }
            if (m_timeline->addBrightnessKeyframe(m_selectedTimelineClipId,
                    m_timeline->playheadMs(), brightnessSlider->value())) {
                keyframeCountLabel->setText(QString("Brightness keyframes: %1")
                    .arg(m_timeline->brightnessKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Brightness keyframe added at the playhead", 3000);
                refreshPausedTimelinePreview();
            }
        });
    connect(removeKeyframeButton, &QPushButton::clicked, this,
        [this, keyframeCountLabel]() {
            if (m_selectedTimelineClipId < 0) return;
            if (m_timeline->removeBrightnessKeyframeAt(
                    m_selectedTimelineClipId, m_timeline->playheadMs())) {
                keyframeCountLabel->setText(QString("Brightness keyframes: %1")
                    .arg(m_timeline->brightnessKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Nearest brightness keyframe removed", 3000);
                refreshPausedTimelinePreview();
            } else {
                statusBar()->showMessage("No brightness keyframe close to the playhead", 3000);
            }
        });
    inspectorLayout->addWidget(addKeyframeButton);
    inspectorLayout->addWidget(removeKeyframeButton);
    auto *contrastKeyframeHeading = new QLabel("ANIMATION — CONTRAST");
    contrastKeyframeHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(contrastKeyframeHeading);
    auto *contrastKeyframeCountLabel = new QLabel("Contrast keyframes: 0");
    contrastKeyframeCountLabel->setObjectName("mutedLabel");
    inspectorLayout->addWidget(contrastKeyframeCountLabel);
    auto *addContrastKeyframeButton = new QPushButton("Add / Update Contrast Keyframe");
    auto *removeContrastKeyframeButton = new QPushButton("Remove Nearest Contrast Keyframe");
    connect(addContrastKeyframeButton, &QPushButton::clicked, this,
        [this, contrastKeyframeCountLabel, contrastSlider]() {
            if (m_selectedTimelineClipId < 0) {
                statusBar()->showMessage("Select a video clip before adding keyframes", 3500);
                return;
            }
            if (m_timeline->addContrastKeyframe(m_selectedTimelineClipId,
                    m_timeline->playheadMs(), contrastSlider->value())) {
                contrastKeyframeCountLabel->setText(QString("Contrast keyframes: %1")
                    .arg(m_timeline->contrastKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Contrast keyframe added at the playhead", 3000);
                refreshPausedTimelinePreview();
            }
        });
    connect(removeContrastKeyframeButton, &QPushButton::clicked, this,
        [this, contrastKeyframeCountLabel]() {
            if (m_selectedTimelineClipId < 0) return;
            if (m_timeline->removeContrastKeyframeAt(
                    m_selectedTimelineClipId, m_timeline->playheadMs())) {
                contrastKeyframeCountLabel->setText(QString("Contrast keyframes: %1")
                    .arg(m_timeline->contrastKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Nearest contrast keyframe removed", 3000);
                refreshPausedTimelinePreview();
            } else {
                statusBar()->showMessage("No contrast keyframe close to the playhead", 3000);
            }
        });
    inspectorLayout->addWidget(addContrastKeyframeButton);
    inspectorLayout->addWidget(removeContrastKeyframeButton);

    auto *transformHeading = new QLabel("TRANSFORM");
    transformHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(transformHeading);
    auto *scaleSlider = new QSlider(Qt::Horizontal);
    auto *positionXSlider = new QSlider(Qt::Horizontal);
    auto *positionYSlider = new QSlider(Qt::Horizontal);
    m_clipTransformSliders = {{scaleSlider, positionXSlider, positionYSlider}};
    const QStringList transformNames = {"Scale (%)", "Position X", "Position Y"};
    const int transformMinimums[] = {10, -100, -100};
    const int transformMaximums[] = {300, 100, 100};
    const int transformDefaults[] = {100, 0, 0};
    for (int i = 0; i < 3; ++i) {
        auto *label = new QLabel(QString("%1: %2").arg(transformNames.at(i)).arg(transformDefaults[i]));
        label->setObjectName("mutedLabel");
        m_clipTransformSliders[static_cast<size_t>(i)]->setRange(
            transformMinimums[i], transformMaximums[i]);
        m_clipTransformSliders[static_cast<size_t>(i)]->setValue(transformDefaults[i]);
        connect(m_clipTransformSliders[static_cast<size_t>(i)], &QSlider::valueChanged,
            this, [this, label, i, transformNames](int value) {
                label->setText(QString("%1: %2").arg(transformNames.at(i)).arg(value));
                if (m_selectedTimelineClipId < 0) return;
                m_timeline->setClipTransformSettings(m_selectedTimelineClipId,
                    m_clipTransformSliders[0]->value(), m_clipTransformSliders[1]->value(),
                    m_clipTransformSliders[2]->value());
                TimelineWidget::ClipInfo clip;
                if (m_timeline->selectedClipInfo(&clip) && clip.track < 3)
                    m_monitor->setTrackTransform(clip.track, clip.scale, clip.positionX, clip.positionY);
            });
        inspectorLayout->addWidget(label);
        inspectorLayout->addWidget(m_clipTransformSliders[static_cast<size_t>(i)]);
    }
    auto *opacityHeading = new QLabel("OPACITY");
    opacityHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(opacityHeading);
    m_clipOpacitySlider = new QSlider(Qt::Horizontal);
    m_clipOpacitySlider->setRange(0, 100);
    m_clipOpacitySlider->setValue(100);
    auto *opacityLabel = new QLabel("Opacity: 100%");
    opacityLabel->setObjectName("mutedLabel");
    connect(m_clipOpacitySlider, &QSlider::valueChanged, this, [this, opacityLabel](int value) {
        opacityLabel->setText(QString("Opacity: %1%").arg(value));
        if (m_selectedTimelineClipId < 0) return;
        m_timeline->setClipOpacity(m_selectedTimelineClipId, value);
        TimelineWidget::ClipInfo clip;
        if (m_timeline->selectedClipInfo(&clip) && clip.track < 3)
            m_monitor->setTrackOpacity(clip.track, value);
    });
    inspectorLayout->addWidget(opacityLabel);
    inspectorLayout->addWidget(m_clipOpacitySlider);
    auto *blendModeHeading = new QLabel("BLEND MODE");
    blendModeHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(blendModeHeading);
    m_clipBlendModeCombo = new QComboBox();
    m_clipBlendModeCombo->addItems({"Normal", "Multiply", "Screen", "Overlay"});
    connect(m_clipBlendModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int mode) {
        if (m_selectedTimelineClipId < 0) return;
        if (!m_timeline->setClipBlendMode(m_selectedTimelineClipId, mode)) return;
        TimelineWidget::ClipInfo clip;
        if (m_timeline->selectedClipInfo(&clip) && clip.track < 3)
            m_monitor->setTrackBlendMode(clip.track, mode);
    });
    inspectorLayout->addWidget(m_clipBlendModeCombo);
    auto *audioClipHeading = new QLabel("CLIP AUDIO");
    audioClipHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(audioClipHeading);
    auto *audioVolumeLabel = new QLabel("Clip volume: 100%");
    audioVolumeLabel->setObjectName("mutedLabel");
    m_clipAudioVolumeSlider = new QSlider(Qt::Horizontal);
    m_clipAudioVolumeSlider->setRange(0, 100);
    m_clipAudioVolumeSlider->setValue(100);
    connect(m_clipAudioVolumeSlider, &QSlider::valueChanged, this, [this, audioVolumeLabel](int) {
        audioVolumeLabel->setText(QString("Clip volume: %1%").arg(m_clipAudioVolumeSlider->value()));
        if (m_selectedTimelineClipId < 0) return;
        m_timeline->setClipAudioSettings(m_selectedTimelineClipId, m_clipAudioVolumeSlider->value(),
            m_clipFadeInSlider->value(), m_clipFadeOutSlider->value());
        applyAudioMixerState();
    });
    inspectorLayout->addWidget(audioVolumeLabel);
    inspectorLayout->addWidget(m_clipAudioVolumeSlider);
    m_clipAudioMuteCheckBox = new QCheckBox("Mute this clip");
    connect(m_clipAudioMuteCheckBox, &QCheckBox::toggled, this, [this](bool muted) {
        if (m_selectedTimelineClipId >= 0) {
            m_timeline->setClipAudioMuted(m_selectedTimelineClipId, muted);
            applyAudioMixerState();
        }
    });
    inspectorLayout->addWidget(m_clipAudioMuteCheckBox);
    auto *fadeInLabel = new QLabel("Fade in: 0 ms");
    fadeInLabel->setObjectName("mutedLabel");
    m_clipFadeInSlider = new QSlider(Qt::Horizontal);
    m_clipFadeInSlider->setRange(0, 5000);
    m_clipFadeInSlider->setSingleStep(100);
    m_clipFadeInSlider->setPageStep(500);
    connect(m_clipFadeInSlider, &QSlider::valueChanged, this, [this, fadeInLabel](int value) {
        fadeInLabel->setText(QString("Fade in: %1 ms").arg(value));
        if (m_selectedTimelineClipId >= 0)
            m_timeline->setClipAudioSettings(m_selectedTimelineClipId, m_clipAudioVolumeSlider->value(), value, m_clipFadeOutSlider->value());
    });
    inspectorLayout->addWidget(fadeInLabel);
    inspectorLayout->addWidget(m_clipFadeInSlider);
    auto *fadeOutLabel = new QLabel("Fade out: 0 ms");
    fadeOutLabel->setObjectName("mutedLabel");
    m_clipFadeOutSlider = new QSlider(Qt::Horizontal);
    m_clipFadeOutSlider->setRange(0, 5000);
    m_clipFadeOutSlider->setSingleStep(100);
    m_clipFadeOutSlider->setPageStep(500);
    connect(m_clipFadeOutSlider, &QSlider::valueChanged, this, [this, fadeOutLabel](int value) {
        fadeOutLabel->setText(QString("Fade out: %1 ms").arg(value));
        if (m_selectedTimelineClipId >= 0)
            m_timeline->setClipAudioSettings(m_selectedTimelineClipId, m_clipAudioVolumeSlider->value(), m_clipFadeInSlider->value(), value);
    });
    inspectorLayout->addWidget(fadeOutLabel);
    inspectorLayout->addWidget(m_clipFadeOutSlider);
    auto *audioKeyframeCountLabel = new QLabel("Volume keyframes: 0");
    audioKeyframeCountLabel->setObjectName("mutedLabel");
    inspectorLayout->addWidget(audioKeyframeCountLabel);
    auto *addAudioKeyframeButton = new QPushButton("Add / Update Volume Keyframe");
    auto *removeAudioKeyframeButton = new QPushButton("Remove Nearest Volume Keyframe");
    connect(addAudioKeyframeButton, &QPushButton::clicked, this, [this, audioKeyframeCountLabel]() {
        if (m_selectedTimelineClipId < 0) return;
        if (m_timeline->addAudioVolumeKeyframe(m_selectedTimelineClipId, m_timeline->playheadMs(), m_clipAudioVolumeSlider->value())) {
            audioKeyframeCountLabel->setText(QString("Volume keyframes: %1").arg(m_timeline->audioVolumeKeyframeCount(m_selectedTimelineClipId)));
            applyAudioMixerState();
        }
    });
    connect(removeAudioKeyframeButton, &QPushButton::clicked, this, [this, audioKeyframeCountLabel]() {
        if (m_selectedTimelineClipId < 0) return;
        if (m_timeline->removeAudioVolumeKeyframeAt(m_selectedTimelineClipId, m_timeline->playheadMs())) {
            audioKeyframeCountLabel->setText(QString("Volume keyframes: %1").arg(m_timeline->audioVolumeKeyframeCount(m_selectedTimelineClipId)));
            applyAudioMixerState();
        }
    });
    inspectorLayout->addWidget(addAudioKeyframeButton);
    inspectorLayout->addWidget(removeAudioKeyframeButton);
    auto *opacityKeyframeHeading = new QLabel("ANIMATION — OPACITY");
    opacityKeyframeHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(opacityKeyframeHeading);
    auto *opacityKeyframeCountLabel = new QLabel("Opacity keyframes: 0");
    opacityKeyframeCountLabel->setObjectName("mutedLabel");
    inspectorLayout->addWidget(opacityKeyframeCountLabel);
    auto *addOpacityKeyframeButton = new QPushButton("Add / Update Opacity Keyframe");
    auto *removeOpacityKeyframeButton = new QPushButton("Remove Nearest Opacity Keyframe");
    connect(addOpacityKeyframeButton, &QPushButton::clicked, this, [this, opacityKeyframeCountLabel]() {
        if (m_selectedTimelineClipId < 0) return;
        if (m_timeline->addOpacityKeyframe(m_selectedTimelineClipId, m_timeline->playheadMs(),
                m_clipOpacitySlider->value())) {
            opacityKeyframeCountLabel->setText(QString("Opacity keyframes: %1")
                .arg(m_timeline->opacityKeyframeCount(m_selectedTimelineClipId)));
            refreshPausedTimelinePreview();
        }
    });
    connect(removeOpacityKeyframeButton, &QPushButton::clicked, this, [this, opacityKeyframeCountLabel]() {
        if (m_selectedTimelineClipId < 0) return;
        if (m_timeline->removeOpacityKeyframeAt(m_selectedTimelineClipId, m_timeline->playheadMs())) {
            opacityKeyframeCountLabel->setText(QString("Opacity keyframes: %1")
                .arg(m_timeline->opacityKeyframeCount(m_selectedTimelineClipId)));
            refreshPausedTimelinePreview();
        }
    });
    inspectorLayout->addWidget(addOpacityKeyframeButton);
    inspectorLayout->addWidget(removeOpacityKeyframeButton);
    auto *transformKeyframeHeading = new QLabel("ANIMATION — TRANSFORM");
    transformKeyframeHeading->setObjectName("sectionTitle");
    inspectorLayout->addWidget(transformKeyframeHeading);
    auto *transformKeyframeCountLabel = new QLabel("Transform keyframes: 0");
    transformKeyframeCountLabel->setObjectName("mutedLabel");
    inspectorLayout->addWidget(transformKeyframeCountLabel);
    auto *addTransformKeyframeButton = new QPushButton("Add / Update Transform Keyframe");
    auto *removeTransformKeyframeButton = new QPushButton("Remove Nearest Transform Keyframe");
    connect(addTransformKeyframeButton, &QPushButton::clicked, this,
        [this, transformKeyframeCountLabel]() {
            if (m_selectedTimelineClipId < 0) {
                statusBar()->showMessage("Select a video clip before adding keyframes", 3500);
                return;
            }
            if (m_timeline->addTransformKeyframe(m_selectedTimelineClipId,
                    m_timeline->playheadMs(), m_clipTransformSliders[0]->value(),
                    m_clipTransformSliders[1]->value(), m_clipTransformSliders[2]->value())) {
                transformKeyframeCountLabel->setText(QString("Transform keyframes: %1")
                    .arg(m_timeline->transformKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Transform keyframe added at the playhead", 3000);
                refreshPausedTimelinePreview();
            }
        });
    connect(removeTransformKeyframeButton, &QPushButton::clicked, this,
        [this, transformKeyframeCountLabel]() {
            if (m_selectedTimelineClipId < 0) return;
            if (m_timeline->removeTransformKeyframeAt(
                    m_selectedTimelineClipId, m_timeline->playheadMs())) {
                transformKeyframeCountLabel->setText(QString("Transform keyframes: %1")
                    .arg(m_timeline->transformKeyframeCount(m_selectedTimelineClipId)));
                statusBar()->showMessage("Transform keyframe removed", 3000);
                refreshPausedTimelinePreview();
            } else {
                statusBar()->showMessage("No transform keyframe close to the playhead", 3000);
            }
        });
    inspectorLayout->addWidget(addTransformKeyframeButton);
    inspectorLayout->addWidget(removeTransformKeyframeButton);
    inspectorLayout->addStretch(1);
    inspectorDock->setWidget(inspector);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

    auto *mixerDock = new QDockWidget("Audio Mixer", this);
    mixerDock->setObjectName("audioMixerDock");
    mixerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                               | Qt::BottomDockWidgetArea);
    mixerDock->setMinimumHeight(230);
    auto *mixerPanel = new QWidget(mixerDock);
    auto *mixerLayout = new QHBoxLayout(mixerPanel);
    mixerLayout->setContentsMargins(8, 8, 8, 8);
    mixerLayout->setSpacing(10);

    for (int track = 0; track < 2; ++track) {
        auto *strip = new QGroupBox(QString("AUDIO %1").arg(track + 1), mixerPanel);
        strip->setMinimumWidth(100);
        auto *stripLayout = new QVBoxLayout(strip);
        stripLayout->setAlignment(Qt::AlignHCenter);

        auto *gainLabel = new QLabel("0.0 dB", strip);
        gainLabel->setObjectName("timecodeLabel");
        gainLabel->setAlignment(Qt::AlignCenter);

        auto *fader = new QSlider(Qt::Vertical, strip);
        fader->setRange(0, 100);
        fader->setValue(100);
        fader->setTickPosition(QSlider::TicksBothSides);
        fader->setTickInterval(10);
        fader->setMinimumHeight(120);
        fader->setToolTip("Track level: -60 dB to 0 dB");
        m_audioTrackFaders[track] = fader;

        auto *switches = new QHBoxLayout();
        auto *muteButton = new QPushButton("M", strip);
        muteButton->setObjectName("mixerMuteButton");
        muteButton->setCheckable(true);
        muteButton->setToolTip("Mute this audio track");
        auto *soloButton = new QPushButton("S", strip);
        soloButton->setObjectName("mixerSoloButton");
        soloButton->setCheckable(true);
        soloButton->setToolTip("Solo this audio track");
        switches->addWidget(muteButton);
        switches->addWidget(soloButton);

        stripLayout->addWidget(gainLabel);
        stripLayout->addWidget(fader, 1, Qt::AlignHCenter);
        stripLayout->addLayout(switches);
        mixerLayout->addWidget(strip);

        connect(fader, &QSlider::valueChanged, this,
            [this, track, gainLabel](int value) {
                m_audioTrackGains[track] = faderToLinearGain(value);
                gainLabel->setText(faderToLabel(value));
                applyAudioMixerState();
            });
        connect(muteButton, &QPushButton::toggled, this,
            [this, track](bool muted) {
                m_audioTrackMuted[track] = muted;
                applyAudioMixerState();
            });
        connect(soloButton, &QPushButton::toggled, this,
            [this, track](bool solo) {
                m_audioTrackSolo[track] = solo;
                applyAudioMixerState();
            });
    }

    auto *masterStrip = new QGroupBox("MASTER", mixerPanel);
    masterStrip->setMinimumWidth(110);
    auto *masterLayout = new QVBoxLayout(masterStrip);
    masterLayout->setAlignment(Qt::AlignHCenter);
    auto *masterGainLabel = new QLabel("0.0 dB", masterStrip);
    masterGainLabel->setObjectName("timecodeLabel");
    masterGainLabel->setAlignment(Qt::AlignCenter);
    m_masterVolumeSlider = new QSlider(Qt::Vertical, masterStrip);
    m_masterVolumeSlider->setRange(0, 100);
    m_masterVolumeSlider->setValue(100);
    m_masterVolumeSlider->setTickPosition(QSlider::TicksBothSides);
    m_masterVolumeSlider->setTickInterval(10);
    m_masterVolumeSlider->setMinimumHeight(120);
    m_masterVolumeSlider->setToolTip("Master level: -60 dB to 0 dB");
    masterLayout->addWidget(masterGainLabel);
    masterLayout->addWidget(m_masterVolumeSlider, 1, Qt::AlignHCenter);
    mixerLayout->addWidget(masterStrip);

    connect(m_masterVolumeSlider, &QSlider::valueChanged, this,
        [this, masterGainLabel](int value) {
            m_masterGain = faderToLinearGain(value);
            masterGainLabel->setText(faderToLabel(value));
            applyAudioMixerState();
        });

    mixerDock->setWidget(mixerPanel);
    addDockWidget(Qt::RightDockWidgetArea, mixerDock);
    splitDockWidget(inspectorDock, mixerDock, Qt::Vertical);
    resizeDocks({inspectorDock, mixerDock}, {240, 260}, Qt::Vertical);

    auto *timelineDock = new QDockWidget("Timeline", this);
    timelineDock->setObjectName("timelineDock");
    auto *timelinePanel = new QWidget(timelineDock);
    auto *timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(8, 6, 8, 8);
    timelineLayout->setSpacing(6);

    auto *timelineTools = new QHBoxLayout();
    auto *splitButton = new QPushButton("Split at playhead");
    auto *deleteButton = new QPushButton("Delete clip");
    auto *timelineHint = new QLabel("Drag to move  •  Drag clip edges to trim  •  Click to position playhead");
    timelineHint->setObjectName("mutedLabel");
    connect(splitButton, &QPushButton::clicked, this, [this]() {
        if (!m_timeline->splitSelectedClipAtPlayhead())
            statusBar()->showMessage("Place the playhead inside the selected clip to split it", 4000);
        else
            statusBar()->showMessage("Clip split at playhead", 3000);
    });
    connect(deleteButton, &QPushButton::clicked, this, [this]() {
        if (!m_timeline->deleteSelectedClip())
            statusBar()->showMessage("Select a timeline clip first", 3000);
        else
            statusBar()->showMessage("Timeline clip deleted", 3000);
    });
    auto *zoomOutButton = new QPushButton("−");
    zoomOutButton->setToolTip("Zoom timeline out");
    auto *zoomResetButton = new QPushButton("100%");
    zoomResetButton->setToolTip("Reset timeline zoom");
    auto *zoomInButton = new QPushButton("+");
    zoomInButton->setToolTip("Zoom timeline in");
    connect(zoomOutButton, &QPushButton::clicked, m_timeline, &TimelineWidget::zoomOut);
    connect(zoomResetButton, &QPushButton::clicked, m_timeline, &TimelineWidget::resetZoom);
    connect(zoomInButton, &QPushButton::clicked, m_timeline, &TimelineWidget::zoomIn);
    timelineTools->addWidget(splitButton);
    timelineTools->addWidget(deleteButton);
    timelineTools->addSpacing(12);
    timelineTools->addWidget(new QLabel("ZOOM"));
    timelineTools->addWidget(zoomOutButton);
    timelineTools->addWidget(zoomResetButton);
    timelineTools->addWidget(zoomInButton);
    timelineTools->addSpacing(12);
    timelineTools->addWidget(timelineHint);
    timelineTools->addStretch();
    timelineLayout->addLayout(timelineTools);

    auto *scroll = new QScrollArea(timelinePanel);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_timeline = new TimelineWidget(scroll);
    scroll->setWidget(m_timeline);
    timelineLayout->addWidget(scroll, 1);
    timelineDock->setWidget(timelinePanel);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock);

    auto *markerDock = new QDockWidget("Timeline Markers", this);
    markerDock->setObjectName("timelineMarkersDock");
    markerDock->setMinimumWidth(210);
    auto *markerList = new QListWidget(markerDock);
    markerList->setObjectName("timelineMarkerList");
    markerList->setToolTip("Double-click a marker to move the playhead to it; right-click to remove it");
    markerList->setContextMenuPolicy(Qt::CustomContextMenu);
    markerDock->setWidget(markerList);
    addDockWidget(Qt::BottomDockWidgetArea, markerDock);
    splitDockWidget(timelineDock, markerDock, Qt::Horizontal);

    const auto refreshMarkerList = [this, markerList]() {
        markerList->clear();
        const QJsonArray markers = m_timeline->markersToJson();
        for (const QJsonValue &value : markers) {
            const QJsonObject marker = value.toObject();
            const qint64 timeMs = static_cast<qint64>(marker.value("timeMs").toDouble());
            const qint64 totalSeconds = timeMs / 1000;
            const QString time = QString("%1:%2")
                .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
                .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
            auto *item = new QListWidgetItem(
                QString("%1   %2").arg(time, marker.value("label").toString()), markerList);
            item->setData(Qt::UserRole, timeMs);
            if (timeMs == m_timeline->playheadMs()) {
                item->setBackground(QColor("#49305f"));
                item->setForeground(QColor("#ffffff"));
            }
        }
    };
    connect(m_timeline, &TimelineWidget::markersChanged, this, refreshMarkerList);
    connect(m_timeline, &TimelineWidget::playheadChanged, this,
        [markerList](qint64 positionMs) {
            for (int i = 0; i < markerList->count(); ++i) {
                auto *item = markerList->item(i);
                const bool active = item->data(Qt::UserRole).toLongLong() == positionMs;
                item->setBackground(active ? QColor("#49305f") : QColor());
                item->setForeground(active ? QColor("#ffffff") : QColor());
            }
        });
    connect(markerList, &QListWidget::customContextMenuRequested, this,
        [this, markerList](const QPoint &position) {
            auto *item = markerList->itemAt(position);
            if (!item) return;
            QMenu menu(markerList);
            QAction *removeAction = menu.addAction("Remove Marker");
            if (menu.exec(markerList->mapToGlobal(position)) == removeAction) {
                const qint64 timeMs = item->data(Qt::UserRole).toLongLong();
                if (m_timeline->removeMarkerAtTime(timeMs))
                    statusBar()->showMessage("Timeline marker removed", 3000);
            }
        });
    connect(markerList, &QListWidget::itemDoubleClicked, this,
        [this](QListWidgetItem *item) {
            if (!item) return;
            m_timeline->setPlayheadMs(item->data(Qt::UserRole).toLongLong(), true);
        });
    refreshMarkerList();

    resizeDocks({mediaDock, inspectorDock}, {260, 290}, Qt::Horizontal);
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu("&File");
    auto *newAction = file->addAction("&New Project");
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);
    auto *openAction = file->addAction("&Open Project...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);
    file->addSeparator();
    auto *saveAction = file->addAction("&Save Project");
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveProject);
    auto *saveAsAction = file->addAction("Save Project &As...");
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);
    auto *recoveryAction = file->addAction("Recovery Center...");
    connect(recoveryAction, &QAction::triggered, this, &MainWindow::openRecoveryCenter);
    file->addSeparator();
    auto *exportAction = file->addAction("Export Video...");
    exportAction->setShortcut(QKeySequence("Ctrl+Shift+E"));
    exportAction->setStatusTip("Render the current timeline to an MP4 video");
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportVideo);
    file->addSeparator();
    auto *exitAction = file->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, [this]() { close(); });

    QMenu *edit = menuBar()->addMenu("&Edit");
    auto *undoAction = edit->addAction("Undo");
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setShortcutContext(Qt::ApplicationShortcut);
    undoAction->setStatusTip("Undo the last timeline or Inspector change");
    connect(undoAction, &QAction::triggered, this, &MainWindow::undoTimelineEdit);
    auto *redoAction = edit->addAction("Redo");
    redoAction->setShortcuts({QKeySequence("Ctrl+Shift+Z"), QKeySequence("Ctrl+Y")});
    redoAction->setShortcutContext(Qt::ApplicationShortcut);
    redoAction->setStatusTip("Redo the last undone timeline or Inspector change");
    connect(redoAction, &QAction::triggered, this, &MainWindow::redoTimelineEdit);

    QMenu *view = menuBar()->addMenu("&View");
    auto *zoomInAction = view->addAction("Zoom Timeline In");
    zoomInAction->setShortcut(QKeySequence("Ctrl++"));
    connect(zoomInAction, &QAction::triggered, m_timeline, &TimelineWidget::zoomIn);
    auto *zoomOutAction = view->addAction("Zoom Timeline Out");
    zoomOutAction->setShortcut(QKeySequence("Ctrl+-"));
    connect(zoomOutAction, &QAction::triggered, m_timeline, &TimelineWidget::zoomOut);
    auto *zoomResetAction = view->addAction("Reset Timeline Zoom");
    zoomResetAction->setShortcut(QKeySequence("Ctrl+0"));
    connect(zoomResetAction, &QAction::triggered, m_timeline, &TimelineWidget::resetZoom);

    QMenu *sequence = menuBar()->addMenu("&Sequence");
    auto *settingsAction = sequence->addAction("Sequence Settings...");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::setSequenceFrameRate);
    sequence->addSeparator();
    auto *previousFrameAction = sequence->addAction("Previous Frame");
    previousFrameAction->setShortcut(QKeySequence("Left"));
    connect(previousFrameAction, &QAction::triggered, this,
            [this]() { stepSequenceFrame(-1); });
    auto *nextFrameAction = sequence->addAction("Next Frame");
    nextFrameAction->setShortcut(QKeySequence("Right"));
    connect(nextFrameAction, &QAction::triggered, this,
            [this]() { stepSequenceFrame(1); });

    auto *reverseStepAction = sequence->addAction("J - Previous Frame");
    reverseStepAction->setShortcut(QKeySequence("J"));
    reverseStepAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(reverseStepAction, &QAction::triggered, this, [this]() {
        if (m_sequencePlaying)
            toggleSequencePlayback();
        stepSequenceFrame(-1);
    });
    auto *pauseAction = sequence->addAction("K - Pause Timeline");
    pauseAction->setShortcut(QKeySequence("K"));
    pauseAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(pauseAction, &QAction::triggered, this, [this]() {
        if (m_sequencePlaying)
            toggleSequencePlayback();
    });
    auto *playAction = sequence->addAction("L - Play Timeline");
    playAction->setShortcut(QKeySequence("L"));
    playAction->setShortcutContext(Qt::ApplicationShortcut);
    connect(playAction, &QAction::triggered, this, [this]() {
        if (!m_sequencePlaying)
            toggleSequencePlayback();
    });
    auto *splitAllAction = sequence->addAction("Add Edit to All Tracks");
    splitAllAction->setShortcut(QKeySequence("Ctrl+Shift+K"));
    connect(splitAllAction, &QAction::triggered, this, [this]() {
        const int count = m_timeline->splitAllClipsAtPlayhead();
        if (count == 0)
            statusBar()->showMessage(
                "No eligible clips at the playhead on unlocked tracks", 3500);
        else
            statusBar()->showMessage(
                QString("Added an edit to %1 clip(s) across unlocked tracks").arg(count),
                3500);
    });
    auto *rippleDeleteAction = sequence->addAction("Ripple Delete Selected Clip");
    rippleDeleteAction->setShortcut(QKeySequence("Shift+Delete"));
    connect(rippleDeleteAction, &QAction::triggered, this, [this]() {
        if (!m_timeline->rippleDeleteSelectedClip())
            statusBar()->showMessage("Select a clip on an unlocked track to ripple delete", 3500);
        else
            statusBar()->showMessage("Clip removed and following unlocked clips rippled left", 3500);
    });

    auto *duplicateClipAction = sequence->addAction("Duplicate Selected Clip");
    duplicateClipAction->setShortcut(QKeySequence("Ctrl+D"));
    connect(duplicateClipAction, &QAction::triggered, this, [this]() {
        if (!m_timeline->duplicateSelectedClip())
            statusBar()->showMessage("Select an unlocked timeline clip to duplicate", 3500);
        else
            statusBar()->showMessage("Clip duplicated after the original", 3000);
    });

    auto *addMarkerAction = sequence->addAction("Add Timeline Marker");
    addMarkerAction->setShortcut(QKeySequence("M"));
    connect(addMarkerAction, &QAction::triggered, m_timeline, &TimelineWidget::addMarker);
    auto *removeMarkerAction = sequence->addAction("Remove Nearest Marker");
    removeMarkerAction->setShortcut(QKeySequence("Shift+M"));
    connect(removeMarkerAction, &QAction::triggered, m_timeline, &TimelineWidget::removeNearestMarker);

    auto *previousMarkerAction = sequence->addAction("Previous Marker");
    previousMarkerAction->setShortcut(QKeySequence("Ctrl+Alt+Left"));
    connect(previousMarkerAction, &QAction::triggered,
            m_timeline, &TimelineWidget::goToPreviousMarker);
    auto *nextMarkerAction = sequence->addAction("Next Marker");
    nextMarkerAction->setShortcut(QKeySequence("Ctrl+Alt+Right"));
    connect(nextMarkerAction, &QAction::triggered,
            m_timeline, &TimelineWidget::goToNextMarker);

    QMenu *media = menuBar()->addMenu("&Media");
    auto *importAction = media->addAction("&Import Media...");
    importAction->setShortcut(QKeySequence("Ctrl+I"));
    connect(importAction, &QAction::triggered, this, &MainWindow::importMedia);
    auto *relinkAction = media->addAction("&Relink Selected Media...");
    relinkAction->setShortcut(QKeySequence("Ctrl+Shift+R"));
    connect(relinkAction, &QAction::triggered, this, &MainWindow::relinkSelectedMedia);
    media->addSeparator();
    auto *collectMediaAction = media->addAction("&Collect Project Media...");
    collectMediaAction->setToolTip("Copy all referenced media into a Media folder beside the project");
    connect(collectMediaAction, &QAction::triggered, this, &MainWindow::collectProjectMedia);

    auto *toolbar = addToolBar("Main");
    toolbar->setObjectName("mainToolbar");
    toolbar->setMovable(false);
    toolbar->addAction(importAction);
    toolbar->addAction(saveAction);
}

void MainWindow::newProject()
{
    stopSequencePlayback(true);
    if (!m_mediaPaths.isEmpty()
        && QMessageBox::question(this, "New Project", "Discard the current project state?")
            != QMessageBox::Yes) return;
    QFile::remove(untitledRecoveryPath());
    m_projectPath.clear();
    m_sequenceFpsNumerator = 25;
    m_sequenceFpsDenominator = 1;
    m_timeline->setFrameRate(25, 1);
    m_mediaPaths.clear();
    m_mediaList->clear();
    m_timeline->clearClips();
    resetTimelineHistory();
    m_projectDirty = false;
    m_player->stop();
    m_player->setSource(QUrl());
    m_sourceLabel->setText("No source loaded");
    m_inspectorLabel->setText("New empty project. Import media to begin.");
    updateWindowTitle();
    statusBar()->showMessage("New project created", 4000);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open Kadris Project", {}, "Kadris Projects (*.kadris);;JSON Files (*.json)");
    if (path.isEmpty()) return;
    const QString recoveryPath = path + ".autosave";
    const QFileInfo originalInfo(path);
    const QFileInfo recoveryInfo(recoveryPath);
    const bool recoveryAvailable = recoveryInfo.exists()
        && recoveryInfo.lastModified() > originalInfo.lastModified();
    if (recoveryAvailable) {
        const auto choice = QMessageBox::question(this, "Recover Project",
            "A newer autosave was found for this project. Recover it?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice == QMessageBox::Yes) {
            if (readProject(recoveryPath)) {
                m_projectPath = path;
                updateWindowTitle();
                statusBar()->showMessage("Recovered the latest autosaved project", 6000);
                return;
            }
            QMessageBox::warning(this, "Recover Project",
                "The autosave could not be opened. The original project will be opened instead.");
        }
    }
    if (!readProject(path))
        QMessageBox::warning(this, "Open Project", "This file is not a valid Kadris project.");
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) { saveProjectAs(); return; }
    if (!writeProject(m_projectPath)) {
        QMessageBox::warning(this, "Save Project", "The project could not be saved.");
        return;
    }
    QFile::remove(m_projectPath + ".autosave");
    QFile::remove(untitledRecoveryPath());
    m_projectDirty = false;
}

void MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Save Kadris Project", "Untitled.kadris", "Kadris Projects (*.kadris)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".kadris", Qt::CaseInsensitive)) path += ".kadris";
    if (!writeProject(path)) {
        QMessageBox::warning(this, "Save Project", "The project could not be saved.");
        return;
    }
    QFile::remove(m_projectPath + ".autosave");
    QFile::remove(untitledRecoveryPath());
    m_projectDirty = false;
}

QString MainWindow::untitledRecoveryPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath("untitled-session.autosave");
}

void MainWindow::recoverUntitledProject()
{
    const QString recoveryPath = untitledRecoveryPath();
    if (!QFileInfo::exists(recoveryPath)) return;

    const auto choice = QMessageBox::question(this, "Recover Unsaved Session",
        "KadrisStudio found an autosaved session that was never saved as a project. Recover it?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice == QMessageBox::No) {
        QFile::remove(recoveryPath);
        return;
    }

    if (readProject(recoveryPath)) {
        // Keep this as an untitled project; the next manual save will ask for a filename.
        m_projectPath.clear();
        updateWindowTitle();
        statusBar()->showMessage("Recovered the previous unsaved session", 6000);
    } else {
        QMessageBox::warning(this, "Recover Unsaved Session",
            "The recovery file could not be opened and will be discarded.");
        QFile::remove(recoveryPath);
    }
}


void MainWindow::openRecoveryCenter()
{
    QStringList recoveryPaths;
    QStringList descriptions;
    QStringList kinds;
    QStringList originals;
    const QString untitledPath = untitledRecoveryPath();
    if (QFileInfo::exists(untitledPath)) {
        recoveryPaths.append(untitledPath);
        descriptions.append("Unsaved session");
        kinds.append("untitled");
        originals.append(QString());
    }
    if (!m_projectPath.isEmpty() && QFileInfo::exists(m_projectPath + ".autosave")) {
        recoveryPaths.append(m_projectPath + ".autosave");
        descriptions.append(QFileInfo(m_projectPath).fileName());
        kinds.append("project");
        originals.append(m_projectPath);
    }

    QDialog dialog(this);
    dialog.setWindowTitle("KadrisStudio Recovery Center");
    dialog.setMinimumSize(620, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel("Browse available crash-recovery snapshots. Restore a snapshot or discard one you no longer need.", &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    for (int i = 0; i < recoveryPaths.size(); ++i) {
        const QFileInfo info(recoveryPaths.at(i));
        auto *item = new QListWidgetItem(
            QString("%1    |    %2").arg(descriptions.at(i),
                info.lastModified().toString("yyyy-MM-dd hh:mm")), list);
        item->setData(Qt::UserRole, i);
        item->setToolTip(recoveryPaths.at(i));
    }
    if (recoveryPaths.isEmpty()) {
        auto *empty = new QListWidgetItem("No recovery snapshots are currently available.", list);
        empty->setFlags(Qt::NoItemFlags);
    } else {
        list->setCurrentRow(0);
    }

    auto *buttons = new QHBoxLayout();
    auto *restoreButton = new QPushButton("Restore Selected", &dialog);
    auto *discardButton = new QPushButton("Discard Selected", &dialog);
    auto *closeButton = new QPushButton("Close", &dialog);
    restoreButton->setEnabled(!recoveryPaths.isEmpty());
    discardButton->setEnabled(!recoveryPaths.isEmpty());
    buttons->addWidget(restoreButton);
    buttons->addWidget(discardButton);
    buttons->addStretch();
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(discardButton, &QPushButton::clicked, &dialog, [&]() {
        auto *item = list->currentItem();
        if (!item || item->data(Qt::UserRole).toInt() < 0
            || item->data(Qt::UserRole).toInt() >= recoveryPaths.size()) return;
        const int index = item->data(Qt::UserRole).toInt();
        if (QMessageBox::question(&dialog, "Discard Recovery",
                "Permanently discard this recovery snapshot?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        if (!QFile::remove(recoveryPaths.at(index))) {
            QMessageBox::warning(&dialog, "Discard Recovery", "The recovery file could not be removed.");
            return;
        }
        delete list->takeItem(list->row(item));
        recoveryPaths.removeAt(index);
        descriptions.removeAt(index);
        kinds.removeAt(index);
        originals.removeAt(index);
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setData(Qt::UserRole, row);
        const bool available = !recoveryPaths.isEmpty();
        restoreButton->setEnabled(available);
        discardButton->setEnabled(available);
        if (!available) {
            list->clear();
            auto *empty = new QListWidgetItem("No recovery snapshots are currently available.", list);
            empty->setFlags(Qt::NoItemFlags);
        }
    });
    connect(restoreButton, &QPushButton::clicked, &dialog, [&]() {
        auto *item = list->currentItem();
        if (!item || item->data(Qt::UserRole).toInt() < 0
            || item->data(Qt::UserRole).toInt() >= recoveryPaths.size()) return;
        if (m_projectDirty && QMessageBox::question(&dialog, "Replace Current Session",
                "The current project has unsaved changes. Replace it with this recovery snapshot?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        const int index = item->data(Qt::UserRole).toInt();
        if (!readProject(recoveryPaths.at(index))) {
            QMessageBox::warning(&dialog, "Restore Recovery", "This recovery snapshot is invalid or could not be opened.");
            return;
        }
        m_projectPath = kinds.at(index) == "project" ? originals.at(index) : QString();
        m_projectDirty = false;
        updateWindowTitle();
        statusBar()->showMessage(kinds.at(index) == "project"
            ? "Project recovery restored from autosave" : "Unsaved session recovery restored", 6000);
        dialog.accept();
    });
    dialog.exec();
}


void MainWindow::collectProjectMedia()
{
    if (m_projectPath.isEmpty()) {
        const auto choice = QMessageBox::question(this, "Collect Project Media",
            "Save this project before collecting its media?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (choice != QMessageBox::Yes) return;
        saveProjectAs();
        if (m_projectPath.isEmpty()) return;
    }

    const QString projectFolder = QFileInfo(m_projectPath).absolutePath();
    const QString mediaFolder = QDir(projectFolder).filePath("Media");
    if (QMessageBox::question(this, "Collect Project Media",
            QString("Copy every available media file used by this project into:\n%1\n\n"
                    "The project references will then be updated and the project saved.")
                .arg(QDir::toNativeSeparators(mediaFolder)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
        return;

    QStringList referencedPaths = m_mediaPaths;
    const QJsonArray clips = m_timeline->toJson();
    for (const QJsonValue &value : clips) {
        const QString path = value.toObject().value("path").toString();
        if (!path.isEmpty()) referencedPaths.append(path);
    }

    QHash<QString, QString> pathMap;
    QStringList missingFiles;
    QString error;
    if (!ProjectMediaCollection::copyReferencedMedia(
            referencedPaths, mediaFolder, &pathMap, &missingFiles, &error)) {
        QString details = error;
        if (!missingFiles.isEmpty()) {
            details += "\n\nMissing files:";
            for (const QString &missing : missingFiles)
                details += "\n• " + missing;
        }
        QMessageBox::warning(this, "Collect Project Media", details);
        return;
    }

    const QStringList previousMediaPaths = m_mediaPaths;
    m_mediaPaths.clear();
    m_mediaList->clear();
    for (const QString &oldPath : previousMediaPaths) {
        const QString canonical = QFileInfo(oldPath).canonicalFilePath();
        const QString newPath = pathMap.value(QDir::cleanPath(canonical));
        const QString replacement = newPath.isEmpty() ? oldPath : newPath;
        addMediaPath(replacement);
    }
    for (const QString &oldPath : referencedPaths) {
        const QString canonical = QFileInfo(oldPath).canonicalFilePath();
        const QString newPath = pathMap.value(QDir::cleanPath(canonical));
        if (!newPath.isEmpty())
            m_timeline->replaceMediaPath(oldPath, newPath);
    }

    m_projectDirty = true;
    saveProject();
    if (m_projectDirty) {
        statusBar()->showMessage(
            "Media was copied, but the project could not be saved. Save the project to keep the new references.",
            8000);
        return;
    }
    statusBar()->showMessage(
        QString("Collected %1 unique media file(s) into the project Media folder")
            .arg(pathMap.size()), 6000);
}

void MainWindow::autosaveProject()
{
    if (!m_projectDirty) return;

    const QString originalPath = m_projectPath;
    QString recoveryPath;
    if (originalPath.isEmpty()) {
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!QDir().mkpath(appData)) {
            statusBar()->showMessage("Autosave failed — could not create the recovery folder", 6000);
            return;
        }
        recoveryPath = untitledRecoveryPath();
    } else {
        recoveryPath = originalPath + ".autosave";
    }

    if (!writeProject(recoveryPath)) {
        m_projectPath = originalPath;
        updateWindowTitle();
        statusBar()->showMessage("Autosave failed — the original project is unchanged", 6000);
        return;
    }

    // writeProject updates the active path; restore the real project location.
    m_projectPath = originalPath;
    m_projectDirty = false;
    updateWindowTitle();
    statusBar()->showMessage("Crash-recovery autosave created", 2500);
}

bool MainWindow::writeProject(const QString &path)
{
    QJsonArray media;
    for (const QString &item : m_mediaPaths) {
        media.append(ProjectMediaPaths::toStoredPath(path, item));
    }

    QJsonArray timelineClips = m_timeline->toJson();
    for (int i = 0; i < timelineClips.size(); ++i) {
        QJsonObject clip = timelineClips.at(i).toObject();
        const QString sourcePath = clip.value("path").toString();
        if (!sourcePath.isEmpty() && QFileInfo(sourcePath).isAbsolute()) {
            clip.insert("path", ProjectMediaPaths::toStoredPath(path, sourcePath));
            timelineClips.replace(i, clip);
        }
    }

    QJsonObject root;
    root.insert("format", "kadris-project");
    root.insert("formatVersion", 6);
    root.insert("sequenceFrameRateNumerator", m_sequenceFpsNumerator);
    root.insert("sequenceFrameRateDenominator", m_sequenceFpsDenominator);
    root.insert("projectName", QFileInfo(path).completeBaseName());
    root.insert("mediaFiles", media);
    root.insert("timelineClips", timelineClips);
    root.insert("timelineMarkers", m_timeline->markersToJson());
    root.insert("timelineTrackStates", m_timeline->trackStatesToJson());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage("Could not save project: " + file.errorString(), 8000);
        return false;
    }
    const QByteArray projectData = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(projectData) != projectData.size()) {
        statusBar()->showMessage("Could not write complete project: " + file.errorString(), 8000);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        statusBar()->showMessage("Could not finalize project save: " + file.errorString(), 8000);
        return false;
    }
    m_projectPath = path;
    updateWindowTitle();
    statusBar()->showMessage("Project saved", 4000);
    return true;
}

bool MainWindow::readProject(const QString &path)
{
    stopSequencePlayback(true);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject root = document.object();
    if (root.value("format").toString() != "kadris-project"
        || (root.value("formatVersion").toInt() != 1
            && root.value("formatVersion").toInt() != 2
            && root.value("formatVersion").toInt() != 3
            && root.value("formatVersion").toInt() != 4
            && root.value("formatVersion").toInt() != 5
            && root.value("formatVersion").toInt() != 6)) return false;

    m_mediaPaths.clear();
    m_mediaList->clear();
    for (const QJsonValue &value : root.value("mediaFiles").toArray()) {
        const QString storedPath = value.toString();
        addMediaPath(ProjectMediaPaths::resolveStoredPath(path, storedPath));
    }

    QJsonArray timelineClips = root.value("timelineClips").toArray();
    for (int i = 0; i < timelineClips.size(); ++i) {
        QJsonObject clip = timelineClips.at(i).toObject();
        const QString storedPath = clip.value("path").toString();
        if (!storedPath.isEmpty() && !QFileInfo(storedPath).isAbsolute()) {
            clip.insert("path", ProjectMediaPaths::resolveStoredPath(path, storedPath));
            timelineClips.replace(i, clip);
        }
    }
    int fpsNumerator = root.value("sequenceFrameRateNumerator").toInt(25);
    int fpsDenominator = root.value("sequenceFrameRateDenominator").toInt(1);
    const bool supportedRate = (fpsDenominator == 1
            && (fpsNumerator == 24 || fpsNumerator == 25 || fpsNumerator == 30
                || fpsNumerator == 50 || fpsNumerator == 60))
        || (fpsDenominator == 1001
            && (fpsNumerator == 30000 || fpsNumerator == 60000));
    if (!supportedRate) { fpsNumerator = 25; fpsDenominator = 1; }
    m_sequenceFpsNumerator = fpsNumerator;
    m_sequenceFpsDenominator = fpsDenominator;
    m_timeline->setFrameRate(fpsNumerator, fpsDenominator);
    m_timeline->fromJson(timelineClips);
    m_timeline->markersFromJson(root.value("timelineMarkers").toArray());
    m_timeline->trackStatesFromJson(root.value("timelineTrackStates").toArray());
    resetTimelineHistory();
    m_projectPath = path;
    m_projectDirty = false;
    m_player->stop();
    m_player->setSource(QUrl());
    m_sourceLabel->setText("No source loaded");
    updateWindowTitle();
    statusBar()->showMessage("Project opened", 4000);
    return true;
}


QJsonObject MainWindow::captureTimelineState() const
{
    QJsonObject state;
    if (!m_timeline) return state;
    state.insert("clips", m_timeline->toJson());
    state.insert("markers", m_timeline->markersToJson());
    state.insert("tracks", m_timeline->trackStatesToJson());
    return state;
}

void MainWindow::restoreTimelineState(const QJsonObject &state)
{
    if (!m_timeline) return;
    m_restoringTimeline = true;
    m_timeline->fromJson(state.value("clips").toArray());
    m_timeline->markersFromJson(state.value("markers").toArray());
    m_timeline->trackStatesFromJson(state.value("tracks").toArray());
    m_restoringTimeline = false;
    m_lastTimelineState = captureTimelineState();
}

void MainWindow::resetTimelineHistory()
{
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_lastTimelineState = captureTimelineState();
}

void MainWindow::undoTimelineEdit()
{
    if (m_undoHistory.isEmpty()) {
        statusBar()->showMessage("Nothing to undo", 2500);
        return;
    }

    stopSequencePlayback(false);
    m_redoHistory.append(captureTimelineState());
    const QJsonObject previousState = m_undoHistory.takeLast();
    restoreTimelineState(previousState);
    m_projectDirty = true;
    refreshPausedTimelinePreview();
    statusBar()->showMessage("Undo complete", 2500);
}

void MainWindow::redoTimelineEdit()
{
    if (m_redoHistory.isEmpty()) {
        statusBar()->showMessage("Nothing to redo", 2500);
        return;
    }

    stopSequencePlayback(false);
    m_undoHistory.append(captureTimelineState());
    if (m_undoHistory.size() > 100) m_undoHistory.removeFirst();
    const QJsonObject nextState = m_redoHistory.takeLast();
    restoreTimelineState(nextState);
    m_projectDirty = true;
    refreshPausedTimelinePreview();
    statusBar()->showMessage("Redo complete", 2500);
}

void MainWindow::importMedia()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, "Import Media", {},
        "Media Files (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mp3 *.wav *.aac *.flac *.ogg *.png *.jpg *.jpeg *.tif *.tiff *.bmp);;All Files (*.*)");
    for (const QString &path : paths) addMediaPath(path);
    if (!paths.isEmpty())
        statusBar()->showMessage(QString("Imported %1 media item(s)").arg(paths.size()), 5000);
}

void MainWindow::relinkSelectedMedia()
{
    QTreeWidgetItem *item = m_mediaList->currentItem();
    if (!item) {
        statusBar()->showMessage("Select a media item to relink first", 3500);
        return;
    }

    const QString oldPath = item->data(0, Qt::UserRole).toString();
    const QString selectedPath = QFileDialog::getOpenFileName(
        this, "Relink Media", QFileInfo(oldPath).absolutePath(),
        "Media Files (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mp3 *.wav *.aac *.flac *.ogg *.png *.jpg *.jpeg *.tif *.tiff *.bmp);;All Files (*.*)");
    if (selectedPath.isEmpty()) return;
    if (!QFileInfo::exists(selectedPath)) {
        QMessageBox::warning(this, "Relink Media", "The selected file does not exist.");
        return;
    }

    if (m_sequencePlaying) stopSequencePlayback(false);
    const int updatedClips = m_timeline->replaceMediaPath(oldPath, selectedPath);
    for (int i = 0; i < m_mediaPaths.size(); ++i) {
        if (m_mediaPaths.at(i) == oldPath) m_mediaPaths[i] = selectedPath;
    }

    const QFileInfo info(selectedPath);
    item->setText(0, info.fileName());
    item->setText(1, info.suffix().toUpper());
    item->setData(0, Qt::UserRole, selectedPath);
    item->setToolTip(0, selectedPath);

    statusBar()->showMessage(
        QString("Media relinked. Updated %1 timeline clip(s). Save the project to keep this change.")
            .arg(updatedClips), 7000);
}

void MainWindow::addMediaPath(const QString &path)
{
    if (path.isEmpty() || m_mediaPaths.contains(path)) return;
    m_mediaPaths.append(path);
    m_projectDirty = true;
    const QFileInfo info(path);
    auto *item = new QTreeWidgetItem(m_mediaList, {info.fileName(), info.suffix().toUpper()});
    item->setData(0, Qt::UserRole, path);
    if (!info.exists()) {
        item->setText(0, info.fileName() + " (missing)");
        item->setToolTip(0, "Original file is currently missing");
    }
}

void MainWindow::addSelectedMediaToTimeline()
{
    const auto *item = m_mediaList->currentItem();
    if (!item) {
        statusBar()->showMessage("Select a media item first", 3000);
        return;
    }
    m_timeline->addClip(item->data(0, Qt::UserRole).toString());
    statusBar()->showMessage("Added item to the timeline model", 3000);
}

void MainWindow::loadPreview(const QString &path)
{
    stopSequencePlayback(false);
    m_monitor->clearAllFrames();
    m_monitor->setCaption(QFileInfo(path).fileName());
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, "Media Missing", "The selected source file cannot be found:\n" + path);
        return;
    }
    m_player->stop();
    m_player->setSource(QUrl::fromLocalFile(path));
    m_sourceLabel->setText(QFileInfo(path).fileName());
    m_inspectorLabel->setText(QString("Name: %1\nLocation: %2\nFormat: %3\n\n"
                                     "This is source playback. Multi-clip sequence playback "
                                     "is planned for a later milestone.")
        .arg(QFileInfo(path).fileName(), QFileInfo(path).absolutePath(),
             QFileInfo(path).suffix().toUpper()));
    statusBar()->showMessage("Source loaded. Press Play to preview.", 4000);
}

void MainWindow::playPause()
{
    if (m_player->source().isEmpty()) {
        statusBar()->showMessage("Load a media source before playback", 3000);
        return;
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause();
    else m_player->play();
}

void MainWindow::stopPlayback()
{
    m_player->stop();
    m_player->setPosition(0);
}

void MainWindow::toggleSequencePlayback()
{
    if (m_sequencePlaying) {
        m_sequencePlaying = false;
        m_sequenceTimer->stop();
        m_sequenceClock.invalidate();
        for (int track = 0; track < 3; ++track)
            m_videoTrackPlayers[track]->pause();
        for (int track = 0; track < 2; ++track)
            m_audioTrackPlayers[track]->pause();
        m_sequenceButton->setText("Play Timeline");
        statusBar()->showMessage("Timeline playback paused", 3000);
        return;
    }

    if (m_timeline->sequenceDurationMs() <= 0) {
        statusBar()->showMessage("Add at least one clip to the timeline first", 4000);
        return;
    }

    if (m_sequencePositionMs >= m_timeline->sequenceDurationMs()) {
        m_sequencePositionMs = 0;
        m_timeline->setPlayheadMs(0);
    }

    m_player->stop();
    m_monitor->clearAllFrames();
    m_monitor->setCaption("Timeline playback");

    // Rebuild every active source from the current playhead. This also makes
    // pause -> seek -> resume start at the correct source positions.
    for (int track = 0; track < 3; ++track) {
        m_videoTrackPlayers[track]->stop();
        m_videoTrackPlayers[track]->setSource(QUrl());
        m_activeVideoClipIds[track] = -1;
        m_waitingForVideoTrack[track] = false;
        m_failedVideoTrack[track] = false;
    }
    for (int track = 0; track < 2; ++track) {
        m_audioTrackPlayers[track]->stop();
        m_audioTrackPlayers[track]->setSource(QUrl());
        m_activeAudioClipIds[track] = -1;
        m_waitingForAudioTrack[track] = false;
        m_failedAudioTrack[track] = false;
    }

    m_sequencePlaying = true;
    m_playButton->setEnabled(false);
    m_sequenceButton->setText("Pause Timeline");
    m_sequenceClockOriginMs = m_sequencePositionMs;
    m_sequenceClock.start();
    m_sequenceTimer->start();
    syncVideoTracks();
    syncAudioTracks();

    statusBar()->showMessage(
        "Timeline playback: video layers and synchronized AUDIO 1 / AUDIO 2 playback.",
        5500);
}

void MainWindow::stopSequencePlayback(bool resetToStart)
{
    m_sequencePlaying = false;
    if (m_sequenceTimer) m_sequenceTimer->stop();
    m_sequenceClock.invalidate();

    for (int track = 0; track < 3; ++track) {
        m_videoTrackPlayers[track]->stop();
        m_videoTrackPlayers[track]->setSource(QUrl());
        m_activeVideoClipIds[track] = -1;
        m_waitingForVideoTrack[track] = false;
        m_failedVideoTrack[track] = false;
    }
    for (int track = 0; track < 2; ++track) {
        m_audioTrackPlayers[track]->stop();
        m_audioTrackPlayers[track]->setSource(QUrl());
        m_activeAudioClipIds[track] = -1;
        m_waitingForAudioTrack[track] = false;
        m_failedAudioTrack[track] = false;
    }

    if (m_monitor) {
        m_monitor->clearAllFrames();
        m_monitor->setCaption("Source and timeline preview");
    }
    if (m_sequenceButton) m_sequenceButton->setText("Play Timeline");
    if (m_playButton) m_playButton->setEnabled(true);

    if (resetToStart) {
        m_sequencePositionMs = 0;
        if (m_timeline) m_timeline->setPlayheadMs(0);
    }
    updateSequenceTimeLabel();
    refreshPausedTimelinePreview();
}

void MainWindow::tickSequencePlayback()
{
    if (!m_sequencePlaying || !m_sequenceClock.isValid()) return;

    const qint64 elapsed = m_sequenceClock.elapsed();
    if (elapsed < 0) return;

    // Drive the sequence from one monotonic clock origin, then quantize to
    // the 25 fps sequence timebase. This avoids cumulative timer-rounding drift.
    const FrameTimebase timebase(m_sequenceFpsNumerator, m_sequenceFpsDenominator);
    m_sequencePositionMs = timebase.frameToMilliseconds(
        timebase.millisecondsToFrame(m_sequenceClockOriginMs + elapsed));
    const qint64 sequenceEnd = m_timeline->sequenceDurationMs();
    if (m_sequencePositionMs >= sequenceEnd) {
        m_sequencePositionMs = sequenceEnd;
        m_timeline->setPlayheadMs(m_sequencePositionMs);
        updateSequenceTimeLabel();
        stopSequencePlayback(false);
        statusBar()->showMessage("Timeline playback finished", 4000);
        return;
    }

    m_timeline->setPlayheadMs(m_sequencePositionMs);
    updateSequenceTimeLabel();
    syncVideoTracks();
    syncAudioTracks();
}

void MainWindow::refreshPausedTimelinePreview()
{
    if (m_sequencePlaying || !m_timeline || !m_monitor) return;

    m_player->stop();
    m_monitor->setCaption("Timeline preview");
    for (int track = 0; track < 3; ++track) {
        auto *player = m_videoTrackPlayers[track];
        TimelineWidget::ClipInfo clip;
        const bool hasClip = !m_timeline->isTrackMuted(track)
            && m_timeline->clipAtTimeOnTrack(m_sequencePositionMs, track, &clip);
        if (!hasClip) {
            player->stop();
            player->setSource(QUrl());
            m_activeVideoClipIds[track] = -1;
            m_waitingForVideoTrack[track] = false;
            m_failedVideoTrack[track] = false;
            m_monitor->clearFrame(track);
            continue;
        }

        const int animatedBrightness = m_timeline->evaluatedBrightness(
            clip.id, m_sequencePositionMs);
        const int animatedContrast = m_timeline->evaluatedContrast(
            clip.id, m_sequencePositionMs);
        int animatedScale = 100, animatedX = 0, animatedY = 0;
        m_timeline->evaluatedTransform(clip.id, m_sequencePositionMs,
            &animatedScale, &animatedX, &animatedY);
        m_monitor->setTrackTransform(track, animatedScale, animatedX, animatedY);
        m_monitor->setTrackOpacity(track, m_timeline->evaluatedOpacity(clip.id, m_sequencePositionMs));
        m_monitor->setTrackBlendMode(track, clip.blendMode);
        m_monitor->setTrackColorGrade(track, animatedBrightness, animatedContrast,
            clip.saturation, clip.shadows, clip.midtones, clip.highlights,
            clip.redCurve, clip.greenCurve, clip.blueCurve);
        if (m_activeVideoClipIds[track] == clip.id
            && !m_waitingForVideoTrack[track]
            && !m_failedVideoTrack[track]) {
            player->setPlaybackRate(clip.playbackRate);
            const qint64 timelineOffset = qMax<qint64>(
                0, m_sequencePositionMs - clip.timelineStartMs);
            const qint64 sourcePosition = qMax<qint64>(0,
                clip.sourceInMs + qRound64(timelineOffset * clip.playbackRate));
            player->setPosition(sourcePosition);
            player->pause();
            continue;
        }

        player->stop();
        m_monitor->clearFrame(track);
        m_activeVideoClipIds[track] = clip.id;
        m_waitingForVideoTrack[track] = false;
        m_failedVideoTrack[track] = false;
        if (!QFileInfo::exists(clip.path)) {
            m_failedVideoTrack[track] = true;
            statusBar()->showMessage(
                QString("Missing media on VIDEO %1: %2")
                    .arg(track + 1).arg(QFileInfo(clip.path).fileName()), 6000);
            continue;
        }
        m_waitingForVideoTrack[track] = true;
        player->setSource(QUrl::fromLocalFile(clip.path));
        handleVideoTrackStatus(track);
    }

    for (int track = 0; track < 2; ++track) {
        m_audioTrackPlayers[track]->pause();
        m_activeAudioClipIds[track] = -1;
        m_waitingForAudioTrack[track] = false;
        m_failedAudioTrack[track] = false;
    }
}

void MainWindow::stepSequenceFrame(int direction)
{
    if (m_sequencePlaying) {
        statusBar()->showMessage("Pause the timeline before frame stepping", 2500);
        return;
    }
    const FrameTimebase timebase(m_sequenceFpsNumerator, m_sequenceFpsDenominator);
    const qint64 currentFrame = timebase.millisecondsToFrame(m_sequencePositionMs);
    const qint64 targetFrame = qMax<qint64>(0, currentFrame + direction);
    const qint64 durationFrames = timebase.millisecondsToFrame(
        m_timeline->sequenceDurationMs());
    m_sequencePositionMs = timebase.frameToMilliseconds(
        qMin(targetFrame, durationFrames));
    m_timeline->setPlayheadMs(m_sequencePositionMs);
    updateSequenceTimeLabel();
    refreshPausedTimelinePreview();
}

void MainWindow::setSequenceFrameRate()
{
    const QStringList rates = {
        "24 fps", "25 fps", "30 fps", "50 fps", "60 fps",
        "29.97 fps (30000/1001)", "59.94 fps (60000/1001)"
    };
    QString currentRate = m_sequenceFpsDenominator == 1001
        ? (m_sequenceFpsNumerator == 30000 ? rates.at(5) : rates.at(6))
        : QString("%1 fps").arg(m_sequenceFpsNumerator);
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, "Sequence Settings", "Project frame rate:",
        rates, qMax(0, rates.indexOf(currentRate)), false, &accepted);
    if (!accepted || selected.isEmpty()) return;

    int numerator = 25, denominator = 1;
    if (selected == "24 fps") numerator = 24;
    else if (selected == "30 fps") numerator = 30;
    else if (selected == "50 fps") numerator = 50;
    else if (selected == "60 fps") numerator = 60;
    else if (selected == "29.97 fps (30000/1001)") { numerator = 30000; denominator = 1001; }
    else if (selected == "59.94 fps (60000/1001)") { numerator = 60000; denominator = 1001; }
    if (numerator == m_sequenceFpsNumerator && denominator == m_sequenceFpsDenominator) return;

    stopSequencePlayback(false);
    m_sequenceFpsNumerator = numerator;
    m_sequenceFpsDenominator = denominator;
    m_timeline->setFrameRate(numerator, denominator);
    m_sequencePositionMs = m_timeline->playheadMs();
    m_projectDirty = true;
    updateSequenceTimeLabel();
    refreshPausedTimelinePreview();
    statusBar()->showMessage(QString("Sequence frame rate set to %1").arg(selected), 4000);
}

void MainWindow::syncVideoTracks()
{
    if (!m_sequencePlaying) return;

    for (int track = 0; track < 3; ++track) {
        if (m_timeline->isTrackMuted(track)) {
            if (m_activeVideoClipIds[track] != -1) {
                m_videoTrackPlayers[track]->stop();
                m_videoTrackPlayers[track]->setSource(QUrl());
                m_activeVideoClipIds[track] = -1;
                m_waitingForVideoTrack[track] = false;
                m_failedVideoTrack[track] = false;
                m_monitor->clearFrame(track);
            }
            continue;
        }
        TimelineWidget::ClipInfo clip;
        const bool hasClip = m_timeline->clipAtTimeOnTrack(
            m_sequencePositionMs, track, &clip);

        if (!hasClip) {
            if (m_activeVideoClipIds[track] != -1) {
                m_videoTrackPlayers[track]->stop();
                m_videoTrackPlayers[track]->setSource(QUrl());
                m_activeVideoClipIds[track] = -1;
                m_waitingForVideoTrack[track] = false;
                m_failedVideoTrack[track] = false;
                m_monitor->clearFrame(track);
            }
            continue;
        }

        const int animatedBrightness = m_timeline->evaluatedBrightness(
            clip.id, m_sequencePositionMs);
        const int animatedContrast = m_timeline->evaluatedContrast(
            clip.id, m_sequencePositionMs);
        int animatedScale = 100, animatedX = 0, animatedY = 0;
        m_timeline->evaluatedTransform(clip.id, m_sequencePositionMs,
            &animatedScale, &animatedX, &animatedY);
        m_monitor->setTrackTransform(track, animatedScale, animatedX, animatedY);
        m_monitor->setTrackOpacity(track, m_timeline->evaluatedOpacity(clip.id, m_sequencePositionMs));
        m_monitor->setTrackBlendMode(track, clip.blendMode);
        m_monitor->setTrackColorGrade(track, animatedBrightness, animatedContrast,
            clip.saturation, clip.shadows, clip.midtones, clip.highlights,
            clip.redCurve, clip.greenCurve, clip.blueCurve);
        if (clip.id == m_activeVideoClipIds[track]) {
            QMediaPlayer *player = m_videoTrackPlayers[track];
            player->setPlaybackRate(clip.playbackRate);
            if (m_waitingForVideoTrack[track]) {
                handleVideoTrackStatus(track);
            } else if (!m_failedVideoTrack[track]) {
                const qint64 timelineOffset = qMax<qint64>(
                    0, m_sequencePositionMs - clip.timelineStartMs);
                const qint64 sourcePosition = qMax<qint64>(0,
                    clip.sourceInMs + qRound64(timelineOffset * clip.playbackRate));
                if (player->playbackState() == QMediaPlayer::PlayingState) {
                    // Keep the decoded video aligned to the sequence clock without
                    // seeking every frame; large corrections recover from decoder drift.
                    constexpr qint64 videoDriftToleranceMs = 500;
                    if (qAbs(player->position() - sourcePosition)
                        > videoDriftToleranceMs) {
                        player->setPosition(sourcePosition);
                    }
                } else {
                    player->setPosition(sourcePosition);
                    player->play();
                }
            }
            continue;
        }

        m_videoTrackPlayers[track]->stop();
        m_monitor->clearFrame(track);
        m_activeVideoClipIds[track] = clip.id;
        m_monitor->setTrackColorGrade(track, animatedBrightness, animatedContrast,
            clip.saturation, clip.shadows, clip.midtones, clip.highlights,
            clip.redCurve, clip.greenCurve, clip.blueCurve);
        m_waitingForVideoTrack[track] = false;
        m_failedVideoTrack[track] = false;

        if (!QFileInfo::exists(clip.path)) {
            m_failedVideoTrack[track] = true;
            statusBar()->showMessage(
                QString("Missing media on VIDEO %1: %2")
                    .arg(track + 1).arg(QFileInfo(clip.path).fileName()), 6000);
            continue;
        }

        m_waitingForVideoTrack[track] = true;
        m_videoTrackPlayers[track]->setSource(QUrl::fromLocalFile(clip.path));
        handleVideoTrackStatus(track);
    }
}

void MainWindow::handleVideoTrackStatus(int track)
{
    if (track < 0 || track >= 3) return;

    QMediaPlayer *player = m_videoTrackPlayers[track];
    if (!m_sequencePlaying) {
        if (!m_waitingForVideoTrack[track]) return;
        const auto status = player->mediaStatus();
        if (status == QMediaPlayer::LoadedMedia
            || status == QMediaPlayer::BufferedMedia) {
            TimelineWidget::ClipInfo clip;
            if (!m_timeline->clipAtTimeOnTrack(m_sequencePositionMs, track, &clip)
                || clip.id != m_activeVideoClipIds[track]) {
                m_waitingForVideoTrack[track] = false;
                return;
            }
            const qint64 sourcePosition =
                clip.sourceInMs + static_cast<qint64>((m_sequencePositionMs - clip.timelineStartMs) * clip.playbackRate);
            player->setPlaybackRate(clip.playbackRate);
            player->setPosition(qMax<qint64>(0, sourcePosition));
            m_waitingForVideoTrack[track] = false;
            player->pause();
        } else if (status == QMediaPlayer::InvalidMedia
                   || status == QMediaPlayer::EndOfMedia) {
            m_waitingForVideoTrack[track] = false;
            m_failedVideoTrack[track] = true;
            TimelineWidget::ClipInfo clip;
            const QString mediaName =
                m_timeline->clipAtTimeOnTrack(m_sequencePositionMs, track, &clip)
                    ? QFileInfo(clip.path).fileName()
                    : QStringLiteral("unknown media");
            statusBar()->showMessage(
                QString("Could not load VIDEO %1: %2")
                    .arg(track + 1).arg(mediaName), 6000);
        }
        return;
    }
    const QMediaPlayer::MediaStatus status = player->mediaStatus();

    // Do not restart a source from frame zero if it ends before the timeline clip does.
    if (status == QMediaPlayer::EndOfMedia && !m_waitingForVideoTrack[track]) {
        m_failedVideoTrack[track] = true;
        m_monitor->clearFrame(track);
        return;
    }

    if (!m_waitingForVideoTrack[track]) return;

    if (status == QMediaPlayer::LoadedMedia
        || status == QMediaPlayer::BufferedMedia) {
        TimelineWidget::ClipInfo clip;
        if (!m_timeline->clipAtTimeOnTrack(m_sequencePositionMs, track, &clip)
            || clip.id != m_activeVideoClipIds[track]) {
            m_waitingForVideoTrack[track] = false;
            return;
        }

        const qint64 sourcePosition =
            clip.sourceInMs + static_cast<qint64>((m_sequencePositionMs - clip.timelineStartMs) * clip.playbackRate);
        player->setPlaybackRate(clip.playbackRate);
            player->setPosition(qMax<qint64>(0, sourcePosition));
        m_waitingForVideoTrack[track] = false;
        player->play();
    } else if (status == QMediaPlayer::InvalidMedia
        || status == QMediaPlayer::EndOfMedia) {
        m_waitingForVideoTrack[track] = false;
        m_failedVideoTrack[track] = true;
        m_monitor->clearFrame(track);
        statusBar()->showMessage(
            QString("Could not play media on VIDEO %1").arg(track + 1), 6000);
    }
}

void MainWindow::applyAudioMixerState()
{
    const bool anySolo = m_audioTrackSolo[0] || m_audioTrackSolo[1];

    for (int track = 0; track < 2; ++track) {
        QAudioOutput *output = m_audioTrackOutputs[track];
        if (!output) continue;

        const bool mutedBySolo = anySolo && !m_audioTrackSolo[track];
        float clipGain = 1.0f;
        bool clipMuted = false;
        TimelineWidget::ClipInfo clip;
        if (m_timeline && m_timeline->clipAtTimeOnTrack(m_sequencePositionMs, track + 3, &clip)) {
            clipGain = m_timeline->evaluatedAudioVolume(clip.id, m_sequencePositionMs) / 100.0f;
            clipMuted = clip.audioMuted;
            const qint64 localMs = qMax<qint64>(0, m_sequencePositionMs - clip.timelineStartMs);
            if (clip.fadeInMs > 0 && localMs < clip.fadeInMs)
                clipGain *= static_cast<float>(localMs) / clip.fadeInMs;
            const qint64 remainingMs = qMax<qint64>(0, clip.durationMs - localMs);
            if (clip.fadeOutMs > 0 && remainingMs < clip.fadeOutMs)
                clipGain *= static_cast<float>(remainingMs) / clip.fadeOutMs;
        }
        output->setVolume(qBound(0.0f,
            m_audioTrackGains[track] * m_masterGain * clipGain, 1.0f));
        output->setMuted(m_audioTrackMuted[track] || mutedBySolo
            || clipMuted || (m_timeline && m_timeline->isTrackMuted(track + 3)));
    }
}

void MainWindow::syncAudioTracks()
{
    if (!m_sequencePlaying) return;

    constexpr int firstAudioTrack = 3; // Timeline track 4 is AUDIO 1.
    for (int audioTrack = 0; audioTrack < 2; ++audioTrack) {
        const int timelineTrack = firstAudioTrack + audioTrack;
        QMediaPlayer *player = m_audioTrackPlayers[audioTrack];

        TimelineWidget::ClipInfo clip;
        const bool hasClip = m_timeline->clipAtTimeOnTrack(
            m_sequencePositionMs, timelineTrack, &clip);

        if (!hasClip) {
            if (m_activeAudioClipIds[audioTrack] != -1) {
                player->stop();
                player->setSource(QUrl());
                m_activeAudioClipIds[audioTrack] = -1;
                m_waitingForAudioTrack[audioTrack] = false;
                m_failedAudioTrack[audioTrack] = false;
            }
            continue;
        }

        const qint64 sourcePosition =
            clip.sourceInMs + static_cast<qint64>((m_sequencePositionMs - clip.timelineStartMs) * clip.playbackRate);

        if (clip.id == m_activeAudioClipIds[audioTrack]) {
            player->setPlaybackRate(clip.playbackRate);
            if (m_waitingForAudioTrack[audioTrack]) {
                handleAudioTrackStatus(audioTrack);
                continue;
            }
            if (m_failedAudioTrack[audioTrack]) continue;

            if (player->duration() > 0 && sourcePosition >= player->duration()) {
                player->stop();
                m_failedAudioTrack[audioTrack] = true;
                continue;
            }

            if (player->playbackState() == QMediaPlayer::PlayingState) {
                // Correct noticeable decoder drift without constantly seeking.
                if (qAbs(player->position() - sourcePosition) > 300)
                    player->setPosition(sourcePosition);
            } else {
                player->setPlaybackRate(clip.playbackRate);
            player->setPosition(qMax<qint64>(0, sourcePosition));
                player->play();
            }
            continue;
        }

        player->stop();
        m_activeAudioClipIds[audioTrack] = clip.id;
        m_waitingForAudioTrack[audioTrack] = false;
        m_failedAudioTrack[audioTrack] = false;

        if (!QFileInfo::exists(clip.path)) {
            m_failedAudioTrack[audioTrack] = true;
            statusBar()->showMessage(
                QString("Missing media on AUDIO %1: %2")
                    .arg(audioTrack + 1).arg(QFileInfo(clip.path).fileName()), 6000);
            continue;
        }

        m_waitingForAudioTrack[audioTrack] = true;
        player->setSource(QUrl::fromLocalFile(clip.path));
        handleAudioTrackStatus(audioTrack);
    }
    applyAudioMixerState();
}

void MainWindow::handleAudioTrackStatus(int audioTrack)
{
    if (audioTrack < 0 || audioTrack >= 2 || !m_sequencePlaying) return;

    QMediaPlayer *player = m_audioTrackPlayers[audioTrack];
    const QMediaPlayer::MediaStatus status = player->mediaStatus();

    if (status == QMediaPlayer::EndOfMedia
        && !m_waitingForAudioTrack[audioTrack]) {
        m_failedAudioTrack[audioTrack] = true;
        return;
    }

    if (!m_waitingForAudioTrack[audioTrack]) return;

    if (status == QMediaPlayer::LoadedMedia
        || status == QMediaPlayer::BufferedMedia) {
        constexpr int firstAudioTrack = 3;
        TimelineWidget::ClipInfo clip;
        if (!m_timeline->clipAtTimeOnTrack(
                m_sequencePositionMs, firstAudioTrack + audioTrack, &clip)
            || clip.id != m_activeAudioClipIds[audioTrack]) {
            m_waitingForAudioTrack[audioTrack] = false;
            return;
        }

        const qint64 sourcePosition =
            clip.sourceInMs + static_cast<qint64>((m_sequencePositionMs - clip.timelineStartMs) * clip.playbackRate);
        if (player->duration() > 0 && sourcePosition >= player->duration()) {
            m_waitingForAudioTrack[audioTrack] = false;
            m_failedAudioTrack[audioTrack] = true;
            return;
        }

        player->setPlaybackRate(clip.playbackRate);
            player->setPosition(qMax<qint64>(0, sourcePosition));
        m_waitingForAudioTrack[audioTrack] = false;
        player->play();
    } else if (status == QMediaPlayer::InvalidMedia
        || status == QMediaPlayer::EndOfMedia) {
        m_waitingForAudioTrack[audioTrack] = false;
        m_failedAudioTrack[audioTrack] = true;
        statusBar()->showMessage(
            QString("Could not play media on AUDIO %1").arg(audioTrack + 1), 6000);
    }
}

void MainWindow::updateSequenceTimeLabel()
{
    if (m_sequenceTimeLabel) {
        const FrameTimebase timebase(m_sequenceFpsNumerator, m_sequenceFpsDenominator);
        m_sequenceTimeLabel->setText(QString::fromStdString(
            timebase.timecode(timebase.millisecondsToFrame(m_sequencePositionMs))));
    }
}

void MainWindow::updatePosition(qint64 position)
{
    if (!m_seekSlider->isSliderDown()) m_seekSlider->setValue(static_cast<int>(position));
    m_timeLabel->setText(formatTime(position) + " / " + formatTime(m_player->duration()));
}

void MainWindow::updateDuration(qint64 duration)
{
    m_seekSlider->setRange(0, static_cast<int>(qMin<qint64>(duration, 2147483647)));
    m_timeLabel->setText(formatTime(m_player->position()) + " / " + formatTime(duration));
}

QString MainWindow::formatTime(qint64 milliseconds) const
{
    const qint64 secondsTotal = qMax<qint64>(0, milliseconds) / 1000;
    return QString("%1:%2").arg(secondsTotal / 60, 2, 10, QChar('0'))
                           .arg(secondsTotal % 60, 2, 10, QChar('0'));
}

void MainWindow::updateWindowTitle()
{
    const QString name = m_projectPath.isEmpty()
        ? "Untitled Project" : QFileInfo(m_projectPath).fileName();
    setWindowTitle(name + " — Kadris Studio");
}


void MainWindow::exportVideo()
{
    if (m_exportProcess) {
        statusBar()->showMessage("An export is already running", 3500);
        return;
    }

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(this, "Export Video",
            "FFmpeg was not found. Install FFmpeg and add its folder to PATH, "
            "then restart KadrisStudio.");
        return;
    }

    const QStringList resolutions = {
        "1920 × 1080 (Full HD)", "1280 × 720 (HD)", "3840 × 2160 (4K)"
    };
    bool accepted = false;
    const QString resolution = QInputDialog::getItem(
        this, "Export Settings", "Resolution:", resolutions, 0, false, &accepted);
    if (!accepted) return;

    int width = 1920;
    int height = 1080;
    if (resolution.startsWith("1280")) {
        width = 1280;
        height = 720;
    } else if (resolution.startsWith("3840")) {
        width = 3840;
        height = 2160;
    }

    QString outputPath = QFileDialog::getSaveFileName(
        this, "Export Video", "KadrisStudio-Export.mp4",
        "MP4 Video (*.mp4)");
    if (outputPath.isEmpty()) return;
    if (QFileInfo(outputPath).suffix().compare("mp4", Qt::CaseInsensitive) != 0)
        outputPath += ".mp4";

    QStringList arguments;
    QString error;
    if (!FfmpegExport::buildArguments(
            m_timeline->toJson(), width, height,
            m_sequenceFpsNumerator, m_sequenceFpsDenominator,
            outputPath, &arguments, &error)) {
        QMessageBox::warning(this, "Export Video",
            "The timeline cannot be exported yet:\n\n" + error);
        return;
    }

    auto *process = new QProcess(this);
    m_exportProcess = process;
    auto *progress = new QProgressDialog(
        "Rendering your timeline to MP4…", "Cancel Export", 0, 0, this);
    m_exportProgress = progress;
    progress->setWindowTitle("KadrisStudio Export");
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, process, &QProcess::kill);

    connect(process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, [this, process, progress, outputPath](
                int exitCode, QProcess::ExitStatus exitStatus) {
            const QString diagnostics =
                QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            progress->close();
            m_exportProgress = nullptr;
            m_exportProcess = nullptr;
            process->deleteLater();
            progress->deleteLater();

            if (exitStatus == QProcess::NormalExit && exitCode == 0
                && QFileInfo::exists(outputPath)
                && QFileInfo(outputPath).size() > 0) {
                QMessageBox::information(this, "Export Complete",
                    "Your video was exported successfully:\n\n" + outputPath);
                statusBar()->showMessage("MP4 export completed", 5000);
            } else {
                const QString detail = diagnostics.isEmpty()
                    ? QString("FFmpeg exited with code %1.").arg(exitCode)
                    : diagnostics.right(2500);
                QMessageBox::warning(this, "Export Failed",
                    "The video could not be exported.\n\n" + detail);
            }
        });

    connect(process, &QProcess::errorOccurred, this,
        [this, process, progress](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart) return;
            const QString detail = process->errorString();
            progress->close();
            m_exportProgress = nullptr;
            m_exportProcess = nullptr;
            process->deleteLater();
            progress->deleteLater();
            QMessageBox::warning(this, "Export Failed",
                "FFmpeg could not be started:\n\n" + detail);
        });

    process->setProgram(ffmpeg);
    process->setArguments(arguments);
    process->start();
    if (!process->waitForStarted(1500)) {
        if (process->error() == QProcess::FailedToStart) return;
    }
    statusBar()->showMessage("MP4 export started", 4000);
}
