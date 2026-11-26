#include "mainwindow.h"
#include "timelinewidget.h"
#include "programmonitorwidget.h"
#include "frame_timebase.h"

#include <QAction>
#include <QAudioOutput>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QMenuBar>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
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

    connect(m_timeline, &TimelineWidget::clipActivated, this,
        [this](const QString &path) {
            m_inspectorLabel->setText(QString("Timeline clip: %1\n\n"
                "Use the timeline controls to move, trim, split, or remove this clip.")
                .arg(QFileInfo(path).fileName()));
        });
    connect(m_timeline, &TimelineWidget::clipSelectionChanged, this,
        [this](int clipId, const QString &path, int track) {
            if (clipId < 0) {
                m_inspectorLabel->setText("No timeline clip selected.");
                return;
            }
            m_inspectorLabel->setText(QString("Selected clip: %1\\nClip ID: %2\\nTrack: V%3\\n\\n"
                "Clip-specific effect controls will be attached here.")
                .arg(QFileInfo(path).fileName()).arg(clipId).arg(track + 1));
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
        m_lastTimelineState = m_timeline->toJson();
    connect(m_timeline, &TimelineWidget::timelineEdited, this,
        [this]() {
            const QJsonArray currentState = m_timeline->toJson();
            if (!m_restoringTimeline && currentState != m_lastTimelineState) {
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
        });
    inspectorLayout->addWidget(resetColor);
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
    file->addSeparator();
    auto *exitAction = file->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, [this]() { close(); });

    QMenu *edit = menuBar()->addMenu("&Edit");
    auto *undoAction = edit->addAction("Undo Timeline Edit");
    undoAction->setShortcut(QKeySequence::Undo);
    connect(undoAction, &QAction::triggered, this, &MainWindow::undoTimelineEdit);
    auto *redoAction = edit->addAction("Redo Timeline Edit");
    redoAction->setShortcut(QKeySequence("Ctrl+Shift+Z"));
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
    m_projectPath.clear();
    m_mediaPaths.clear();
    m_mediaList->clear();
    m_timeline->clearClips();
    resetTimelineHistory();
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
    if (!readProject(path))
        QMessageBox::warning(this, "Open Project", "This file is not a valid Kadris project.");
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) { saveProjectAs(); return; }
    if (!writeProject(m_projectPath))
        QMessageBox::warning(this, "Save Project", "The project could not be saved.");
}

void MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Save Kadris Project", "Untitled.kadris", "Kadris Projects (*.kadris)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".kadris", Qt::CaseInsensitive)) path += ".kadris";
    if (!writeProject(path))
        QMessageBox::warning(this, "Save Project", "The project could not be saved.");
}

bool MainWindow::writeProject(const QString &path)
{
    QJsonArray media;
    for (const QString &item : m_mediaPaths) media.append(item);
    QJsonObject root;
    root.insert("format", "kadris-project");
    root.insert("formatVersion", 4);
    root.insert("projectName", QFileInfo(path).completeBaseName());
    root.insert("mediaFiles", media);
    root.insert("timelineClips", m_timeline->toJson());
    root.insert("timelineMarkers", m_timeline->markersToJson());
    root.insert("timelineTrackStates", m_timeline->trackStatesToJson());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        statusBar()->showMessage("Could not save project: " + file.errorString(), 8000);
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
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
            && root.value("formatVersion").toInt() != 4)) return false;

    m_mediaPaths.clear();
    m_mediaList->clear();
    for (const QJsonValue &value : root.value("mediaFiles").toArray())
        addMediaPath(value.toString());
    m_timeline->fromJson(root.value("timelineClips").toArray());
    m_timeline->markersFromJson(root.value("timelineMarkers").toArray());
    m_timeline->trackStatesFromJson(root.value("timelineTrackStates").toArray());
    resetTimelineHistory();
    m_projectPath = path;
    m_player->stop();
    m_player->setSource(QUrl());
    m_sourceLabel->setText("No source loaded");
    updateWindowTitle();
    statusBar()->showMessage("Project opened", 4000);
    return true;
}


void MainWindow::resetTimelineHistory()
{
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_lastTimelineState = m_timeline ? m_timeline->toJson() : QJsonArray{};
}

void MainWindow::undoTimelineEdit()
{
    if (m_undoHistory.isEmpty()) {
        statusBar()->showMessage("Nothing to undo", 2500);
        return;
    }

    stopSequencePlayback(false);
    m_redoHistory.append(m_timeline->toJson());
    const QJsonArray previousState = m_undoHistory.takeLast();
    m_restoringTimeline = true;
    m_timeline->fromJson(previousState);
    m_restoringTimeline = false;
    m_lastTimelineState = previousState;
    statusBar()->showMessage("Timeline edit undone", 2500);
}

void MainWindow::redoTimelineEdit()
{
    if (m_redoHistory.isEmpty()) {
        statusBar()->showMessage("Nothing to redo", 2500);
        return;
    }

    stopSequencePlayback(false);
    m_undoHistory.append(m_timeline->toJson());
    const QJsonArray nextState = m_redoHistory.takeLast();
    m_restoringTimeline = true;
    m_timeline->fromJson(nextState);
    m_restoringTimeline = false;
    m_lastTimelineState = nextState;
    statusBar()->showMessage("Timeline edit restored", 2500);
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

void MainWindow::addMediaPath(const QString &path)
{
    if (path.isEmpty() || m_mediaPaths.contains(path)) return;
    m_mediaPaths.append(path);
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
}

void MainWindow::tickSequencePlayback()
{
    if (!m_sequencePlaying || !m_sequenceClock.isValid()) return;

    const qint64 elapsed = m_sequenceClock.elapsed();
    if (elapsed < 0) return;

    // Drive the sequence from one monotonic clock origin, then quantize to
    // the 25 fps sequence timebase. This avoids cumulative timer-rounding drift.
    const FrameTimebase timebase(25, 1);
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

        if (m_activeVideoClipIds[track] == clip.id
            && !m_waitingForVideoTrack[track]
            && !m_failedVideoTrack[track]) {
            player->setPosition(qMax<qint64>(0,
                clip.sourceInMs + m_sequencePositionMs - clip.timelineStartMs));
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
    const FrameTimebase timebase(25, 1);
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

        if (clip.id == m_activeVideoClipIds[track]) {
            if (m_waitingForVideoTrack[track]) {
                handleVideoTrackStatus(track);
            } else if (!m_failedVideoTrack[track]
                && m_videoTrackPlayers[track]->playbackState()
                    != QMediaPlayer::PlayingState) {
                m_videoTrackPlayers[track]->play();
            }
            continue;
        }

        m_videoTrackPlayers[track]->stop();
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
                clip.sourceInMs + (m_sequencePositionMs - clip.timelineStartMs);
            player->setPosition(qMax<qint64>(0, sourcePosition));
            m_waitingForVideoTrack[track] = false;
            player->pause();
        } else if (status == QMediaPlayer::InvalidMedia
                   || status == QMediaPlayer::EndOfMedia) {
            m_waitingForVideoTrack[track] = false;
            m_failedVideoTrack[track] = true;
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
            clip.sourceInMs + (m_sequencePositionMs - clip.timelineStartMs);
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
        output->setVolume(qBound(0.0f,
            m_audioTrackGains[track] * m_masterGain, 1.0f));
        output->setMuted(m_audioTrackMuted[track] || mutedBySolo
            || (m_timeline && m_timeline->isTrackMuted(track + 3)));
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
            clip.sourceInMs + (m_sequencePositionMs - clip.timelineStartMs);

        if (clip.id == m_activeAudioClipIds[audioTrack]) {
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
            clip.sourceInMs + (m_sequencePositionMs - clip.timelineStartMs);
        if (player->duration() > 0 && sourcePosition >= player->duration()) {
            m_waitingForAudioTrack[audioTrack] = false;
            m_failedAudioTrack[audioTrack] = true;
            return;
        }

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
        const FrameTimebase timebase(25, 1);
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
