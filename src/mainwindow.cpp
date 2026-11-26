#include "mainwindow.h"
#include "timelinewidget.h"

#include <QAction>
#include <QAudioOutput>
#include <QDockWidget>
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
        });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
        [this](QMediaPlayer::PlaybackState state) {
            m_playButton->setText(state == QMediaPlayer::PlayingState ? "Pause" : "Play");
        });
    connect(m_timeline, &TimelineWidget::clipActivated, this, &MainWindow::loadPreview);

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
    m_playButton = new QPushButton("Play");
    m_playButton->setObjectName("primaryButton");
    auto *stop = new QPushButton("Stop");
    m_seekSlider = new QSlider(Qt::Horizontal);
    m_seekSlider->setRange(0, 0);
    m_timeLabel = new QLabel("00:00 / 00:00");
    m_timeLabel->setObjectName("timecodeLabel");
    connect(m_playButton, &QPushButton::clicked, this, &MainWindow::playPause);
    connect(stop, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    connect(m_seekSlider, &QSlider::sliderMoved, this,
        [this](int value) { m_player->setPosition(value); });
    transport->addWidget(m_playButton);
    transport->addWidget(stop);
    transport->addWidget(m_seekSlider, 1);
    transport->addWidget(m_timeLabel);
    layout->addLayout(transport);
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
    auto *scroll = new QScrollArea(timelineDock);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_timeline = new TimelineWidget(scroll);
    scroll->setWidget(m_timeline);
    timelineDock->setWidget(scroll);
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
    root.insert("formatVersion", 1);
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject root = document.object();
    if (root.value("format").toString() != "kadris-project"
        || root.value("formatVersion").toInt() != 1) return false;

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
