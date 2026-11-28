#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

class FfmpegExport
{
public:
    // Builds an FFmpeg command for timeline cuts and supported two-layer video Cross Dissolves.
    // Unsupported clip effects are rejected rather than silently omitted.
    static bool buildArguments(const QJsonArray &clips, int width, int height,
                               int fpsNumerator, int fpsDenominator,
                               const QString &outputPath, QStringList *arguments,
                               QString *error = nullptr, int qualityCrf = 23,
                               const QString &encodingPreset = "medium",
                               int audioBitrateKbps = 192, int audioSampleRate = 48000);
};
