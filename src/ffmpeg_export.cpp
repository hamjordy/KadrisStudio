#include "ffmpeg_export.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonObject>
#include <QtMath>

#include <algorithm>

namespace {
QString number(double value)
{
    return QString::number(value, 'f', 6).remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"));
}

QString atempoFilters(double rate)
{
    QStringList filters;
    while (rate > 2.0) { filters << "atempo=2.0"; rate /= 2.0; }
    while (rate < 0.5) { filters << "atempo=0.5"; rate /= 0.5; }
    filters << "atempo=" + number(rate);
    return filters.join(',');
}
}

bool FfmpegExport::buildArguments(const QJsonArray &clips, int width, int height,
                                  int fpsNumerator, int fpsDenominator,
                                  const QString &outputPath, QStringList *arguments,
                                  QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!arguments) return fail("No FFmpeg argument list was provided.");
    arguments->clear();
    if (outputPath.trimmed().isEmpty()) return fail("Choose an output file.");
    if (width < 16 || height < 16 || fpsNumerator <= 0 || fpsDenominator <= 0)
        return fail("The export resolution or frame rate is invalid.");

    struct Segment {
        int input = -1;
        int track = 0;
        double start = 0.0;
        double duration = 0.0;
        double sourceIn = 0.0;
        double rate = 1.0;
        int volume = 100;
        bool muted = false;
    };
    QVector<Segment> video;
    QVector<Segment> audio;
    double totalDuration = 0.0;
    int inputIndex = 0;

    for (const QJsonValue &value : clips) {
        if (!value.isObject()) continue;
        const QJsonObject clip = value.toObject();
        const QString path = clip.value("path").toString();
        const int track = clip.value("track").toInt(-1);
        const double startMs = clip.value("timelineStartMs").toDouble(-1.0);
        const double durationMs = clip.value("durationMs").toDouble(0.0);
        const double sourceInMs = clip.value("sourceInMs").toDouble(0.0);
        const double rate = clip.value("playbackRate").toDouble(1.0);
        if (path.isEmpty() || !QFileInfo::exists(path))
            return fail(QString("Media file is missing: %1").arg(path));
        if (track < 0 || track > 4 || startMs < 0.0 || durationMs <= 0.0
            || rate <= 0.0 || !qIsFinite(rate))
            return fail(QString("Clip %1 has invalid timing or track data.")
                        .arg(clip.value("id").toInt()));
        if (track < 3) {
            const QJsonObject color = clip.value("color").toObject();
            const bool hasUnsupportedEffects =
                clip.value("scale").toInt(100) != 100
                || clip.value("positionX").toInt(0) != 0
                || clip.value("positionY").toInt(0) != 0
                || clip.value("opacity").toInt(100) != 100
                || clip.value("blendMode").toInt(0) != 0
                || color.value("brightness").toInt(0) != 0
                || color.value("contrast").toInt(0) != 0
                || color.value("saturation").toInt(100) != 100
                || color.value("shadows").toInt(0) != 0
                || color.value("midtones").toInt(0) != 0
                || color.value("highlights").toInt(0) != 0
                || color.value("redCurve").toInt(0) != 0
                || color.value("greenCurve").toInt(0) != 0
                || color.value("blueCurve").toInt(0) != 0
                || !clip.value("brightnessKeyframes").toArray().isEmpty()
                || !clip.value("contrastKeyframes").toArray().isEmpty()
                || !clip.value("opacityKeyframes").toArray().isEmpty()
                || !clip.value("transformKeyframes").toArray().isEmpty();
            if (hasUnsupportedEffects)
                return fail(QString("Clip %1 uses effects not yet supported by this first export renderer.")
                            .arg(clip.value("id").toInt()));
        }

        Segment segment;
        segment.input = inputIndex++;
        segment.track = track;
        segment.start = startMs / 1000.0;
        segment.duration = durationMs / 1000.0;
        segment.sourceIn = sourceInMs / 1000.0;
        segment.rate = rate;
        segment.volume = qBound(0, clip.value("audioVolume").toInt(100), 100);
        segment.muted = clip.value("audioMuted").toBool(false);
        if (track < 3) video.append(segment);
        else audio.append(segment);
        totalDuration = qMax(totalDuration, segment.start + segment.duration);
        *arguments << "-i" << path;
    }

    if (video.isEmpty()) return fail("Add at least one video clip to the timeline.");
    if (totalDuration <= 0.0) return fail("The timeline has no duration.");

    const QString fps = QString("%1/%2").arg(fpsNumerator).arg(fpsDenominator);
    QStringList filters;
    filters << QString("color=c=black:s=%1x%2:r=%3:d=%4[base]")
                   .arg(width).arg(height).arg(fps, number(totalDuration));
    QString current = "base";
    int layer = 0;
    for (const Segment &segment : video) {
        const QString label = QString("v%1").arg(layer);
        filters << QString("[%1:v:0]trim=start=%2:duration=%3,setpts=(PTS-STARTPTS)/%4,"
                           "fps=%5,scale=%6:%7:force_original_aspect_ratio=decrease,"
                           "pad=%6:%7:(ow-iw)/2:(oh-ih)/2,setsar=1[%8]")
                       .arg(segment.input).arg(number(segment.sourceIn))
                       .arg(number(segment.duration * segment.rate)).arg(number(segment.rate))
                       .arg(fps).arg(width).arg(height).arg(label);
        const QString next = QString("mix%1").arg(layer);
        filters << QString("[%1][%2]overlay=eof_action=pass:shortest=0:"
                           "enable='between(t,%3,%4)'[%5]")
                       .arg(current, label).arg(number(segment.start))
                       .arg(number(segment.start + segment.duration)).arg(next);
        current = next;
        ++layer;
    }
    filters << QString("[%1]trim=duration=%2,fps=%3,format=yuv420p[vout]")
                   .arg(current).arg(number(totalDuration)).arg(fps);

    *arguments << "-filter_complex" << filters.join(';') << "-map" << "[vout]";

    if (!audio.isEmpty()) {
        QStringList audioLabels;
        int audioNumber = 0;
        for (const Segment &segment : audio) {
            const QString label = QString("a%1").arg(audioNumber);
            QString chain = QString("[%1:a:0]atrim=start=%2:duration=%3,"
                                     "asetpts=PTS-STARTPTS,%4,volume=%5,adelay=%6|%6,"
                                     "apad,atrim=duration=%7[%8]")
                                .arg(segment.input).arg(number(segment.sourceIn))
                                .arg(number(segment.duration * segment.rate))
                                .arg(atempoFilters(segment.rate))
                                .arg(segment.muted ? "0" : number(segment.volume / 100.0))
                                .arg(qRound64(segment.start * 1000.0))
                                .arg(number(totalDuration)).arg(label);
            filters << chain;
            audioLabels << QString("[%1]").arg(label);
            ++audioNumber;
        }
        // Audio filters are appended after video filters; pass the complete graph once.
        QString graph = filters.join(';');
        graph += ';' + audioLabels.join("") +
                 QString("amix=inputs=%1:duration=longest:dropout_transition=0[aout]")
                     .arg(audioLabels.size());
        arguments->removeAt(arguments->indexOf("-filter_complex") + 1);
        *arguments << "-filter_complex" << graph << "-map" << "[vout]" << "-map" << "[aout]";
    }

    *arguments << "-t" << number(totalDuration)
               << "-c:v" << "libx264" << "-preset" << "medium"
               << "-pix_fmt" << "yuv420p" << "-r" << fps;
    if (!audio.isEmpty()) *arguments << "-c:a" << "aac" << "-b:a" << "192k";
    *arguments << "-movflags" << "+faststart" << "-y" << outputPath;
    return true;
}
