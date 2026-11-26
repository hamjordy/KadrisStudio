#include "mainwindow.h"
#include "timelinewidget.h"

#include <QAction>
#include <QAudioOutput>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
#include <QVideoWidget>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    buildInterface();
    buildMenus();

    connect(m_player, &QMediaPlayer::positionChanged, this, &MainWindow::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MainWindow::updateDuration);
    connect(m_player, &QMediaPlayer::errorOccurred, this,
        [this](QMediaPlayer::Error, const QString &message) {
            statusBar()->showMessage(QString("Playback error: %1").arg(message), 8000);
            if (m_sequencePlaying && m_waitingForSequenceMedia) {
                m_waitingForSequenceMedia = false;
                m_activeSequenceClipId = -1;
                m_sequencePositionMs = qMax(m_sequencePositionMs, m_activeSequenceClipEndMs);
                if (!m_sequenceClock.isValid()) m_sequenceClock.start();
                activateSequencePosition();
            }
        });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
        [this](QMediaPlayer::MediaStatus) { handleSequenceMediaStatus(); });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
        [this](QMediaPlayer::PlaybackState state) {
            if (!m_sequencePlaying)
                m_playButton->setText(state == QMediaPlayer::PlayingState ? "Pause" : "Play");
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
    connect(m_timeline, &TimelineWidget::playheadChanged, this,
        [this](qint64 position) {
            m_sequencePositionMs = position;
            updateSequenceTimeLabel();
            if (m_sequencePlaying) {
                m_activeSequenceClipId = -1;
                m_waitingForSequenceMedia = false;
                m_player->stop();
                m_sequenceClock.invalidate();
                activateSequencePosition();
            }
        });
    connect(m_timeline, &TimelineWidget::timelineEdited, this,
        [this]() {
            if (m_sequencePlaying) {
                stopSequencePlayback(false);
                statusBar()->showMessage("Timeline changed — sequence playback stopped", 4000);
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

    m_videoWidget = new QVideoWidget(central);
    m_videoWidget->setObjectName("videoMonitor");
    m_videoWidget->setMinimumSize(480, 270);
    m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    m_player->setVideoOutput(m_videoWidget);
    layout->addWidget(m_videoWidget, 1);

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
    m_sequenceTimeLabel = new QLabel("00:00");
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
    m_inspectorLabel = new QLabel("Select a media item to load it in the source monitor.\n\n"
                                  "Clip properties and effect controls are planned for later milestones.");
    m_inspectorLabel->setObjectName("mutedLabel");
    m_inspectorLabel->setWordWrap(true);
    m_inspectorLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    inspectorLayout->addWidget(inspectorHeading);
    inspectorLayout->addWidget(m_inspectorLabel, 1);
    inspectorDock->setWidget(inspector);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

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
    timelineTools->addWidget(splitButton);
    timelineTools->addWidget(deleteButton);
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
    root.insert("formatVersion", 2);
    root.insert("projectName", QFileInfo(path).completeBaseName());
    root.insert("mediaFiles", media);
    root.insert("timelineClips", m_timeline->toJson());

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
            && root.value("formatVersion").toInt() != 2)) return false;

    m_mediaPaths.clear();
    m_mediaList->clear();
    for (const QJsonValue &value : root.value("mediaFiles").toArray())
        addMediaPath(value.toString());
    m_timeline->fromJson(root.value("timelineClips").toArray());
    m_projectPath = path;
    m_player->stop();
    m_player->setSource(QUrl());
    m_sourceLabel->setText("No source loaded");
    updateWindowTitle();
    statusBar()->showMessage("Project opened", 4000);
    return true;
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
        m_player->pause();
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

    m_sequencePlaying = true;
    m_playButton->setEnabled(false);
    m_sequenceButton->setText("Pause Timeline");
    m_sequenceTimer->start();
    activateSequencePosition();

    if (!m_waitingForSequenceMedia && !m_sequenceClock.isValid())
        m_sequenceClock.start();

    statusBar()->showMessage(
        "Timeline playback started. Video clips play in sequence; separate audio-track mixing is not implemented yet.",
        6000);
}

void MainWindow::stopSequencePlayback(bool resetToStart)
{
    m_sequencePlaying = false;
    if (m_sequenceTimer) m_sequenceTimer->stop();
    m_sequenceClock.invalidate();
    m_waitingForSequenceMedia = false;
    m_activeSequenceClipId = -1;
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
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
    if (!m_sequencePlaying || m_waitingForSequenceMedia) return;

    if (!m_sequenceClock.isValid()) {
        m_sequenceClock.start();
        return;
    }

    const qint64 elapsed = m_sequenceClock.restart();
    if (elapsed <= 0) return;

    m_sequencePositionMs += elapsed;
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

    TimelineWidget::ClipInfo current;
    if (m_timeline->clipAtTime(m_sequencePositionMs, &current)) {
        if (current.id != m_activeSequenceClipId)
            activateSequencePosition();
    } else if (m_activeSequenceClipId >= 0) {
        activateSequencePosition();
    }
}

void MainWindow::activateSequencePosition()
{
    if (!m_sequencePlaying) return;

    TimelineWidget::ClipInfo clip;
    if (m_timeline->clipAtTime(m_sequencePositionMs, &clip)) {
        if (clip.id == m_activeSequenceClipId) {
            if (m_waitingForSequenceMedia) {
                handleSequenceMediaStatus();
            } else if (m_player->playbackState() != QMediaPlayer::PlayingState) {
                m_player->play();
                if (!m_sequenceClock.isValid()) m_sequenceClock.start();
            }
            return;
        }

        if (!QFileInfo::exists(clip.path)) {
            statusBar()->showMessage(
                QString("Missing timeline media: %1").arg(clip.path), 6000);
            m_sequencePositionMs = clip.timelineStartMs + clip.durationMs;
            m_activeSequenceClipId = -1;
            if (!m_sequenceClock.isValid()) m_sequenceClock.start();
            return;
        }

        m_player->stop();
        m_waitingForSequenceMedia = true;
        m_sequenceClock.invalidate();
        m_activeSequenceClipId = clip.id;
        m_activeSequenceClipEndMs = clip.timelineStartMs + clip.durationMs;
        m_pendingSequenceSourceMs =
            clip.sourceInMs + (m_sequencePositionMs - clip.timelineStartMs);

        m_player->setSource(QUrl::fromLocalFile(clip.path));
        handleSequenceMediaStatus();
        return;
    }

    if (m_activeSequenceClipId >= 0) {
        m_player->stop();
        m_player->setSource(QUrl());
        m_activeSequenceClipId = -1;
    }

    TimelineWidget::ClipInfo next;
    if (!m_timeline->nextVideoClipAtOrAfter(m_sequencePositionMs, &next)
        && m_sequencePositionMs >= m_timeline->sequenceDurationMs()) {
        stopSequencePlayback(false);
        statusBar()->showMessage("Timeline playback finished", 4000);
    } else if (!m_sequenceClock.isValid()) {
        m_sequenceClock.start();
    }
}

void MainWindow::handleSequenceMediaStatus()
{
    if (!m_sequencePlaying || !m_waitingForSequenceMedia) return;

    if (m_player->mediaStatus() == QMediaPlayer::LoadedMedia
        || m_player->mediaStatus() == QMediaPlayer::BufferedMedia) {
        m_player->setPosition(m_pendingSequenceSourceMs);
        m_waitingForSequenceMedia = false;
        m_player->play();
        m_sequenceClock.start();
    } else if (m_player->mediaStatus() == QMediaPlayer::InvalidMedia) {
        m_waitingForSequenceMedia = false;
        m_activeSequenceClipId = -1;
        m_sequencePositionMs = m_activeSequenceClipEndMs;
        m_sequenceClock.start();
        statusBar()->showMessage("A timeline clip could not be decoded; skipping it", 6000);
        activateSequencePosition();
    } else if (m_player->mediaStatus() == QMediaPlayer::EndOfMedia) {
        m_waitingForSequenceMedia = false;
        m_activeSequenceClipId = -1;
        m_sequencePositionMs = qMax(m_sequencePositionMs, m_activeSequenceClipEndMs);
        m_timeline->setPlayheadMs(m_sequencePositionMs);
        updateSequenceTimeLabel();
        m_sequenceClock.start();
        activateSequencePosition();
    }
}

void MainWindow::updateSequenceTimeLabel()
{
    if (m_sequenceTimeLabel)
        m_sequenceTimeLabel->setText(formatTime(m_sequencePositionMs));
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
