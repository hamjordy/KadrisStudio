#include "ffmpeg_export.h"
#include "audio_crossfade.h"

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

bool expandNestedClips(const QJsonArray &source, double offsetMs,
                       QJsonArray *expanded, QString *error, int depth = 0)
{
    if (depth > 8) {
        if (error) *error = "Nested sequences exceed the maximum supported depth (8).";
        return false;
    }
    for (const QJsonValue &value : source) {
        if (!value.isObject()) continue;
        QJsonObject clip = value.toObject();
        const double start = clip.value("timelineStartMs").toDouble(0.0);
        if (clip.value("nestedSequence").toBool(false)) {
            if (clip.value("sourceInMs").toDouble(0.0) != 0.0
                || clip.value("playbackRate").toDouble(1.0) != 1.0) {
                if (error) *error = QString("Nested sequence %1 uses source trimming or speed changes that the export renderer does not support yet.")
                    .arg(clip.value("nestedName").toString("Untitled"));
                return false;
            }
            const QJsonArray children = clip.value("nestedClips").toArray();
            if (children.isEmpty()) {
                if (error) *error = QString("Nested sequence %1 is empty.")
                    .arg(clip.value("nestedName").toString("Untitled"));
                return false;
            }
            if (!expandNestedClips(children, offsetMs + start, expanded, error, depth + 1))
                return false;
            continue;
        }
        clip.insert("timelineStartMs", start + offsetMs);
        expanded->append(clip);
    }
    return true;
}
}

bool FfmpegExport::buildArguments(const QJsonArray &clips, int width, int height,
                                  int fpsNumerator, int fpsDenominator,
                                  const QString &outputPath, QStringList *arguments,
                                  QString *error, int qualityCrf,
                                  const QString &encodingPreset, int audioBitrateKbps,
                                  int audioSampleRate)
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
    const QStringList allowedPresets = {
        "ultrafast", "superfast", "veryfast", "faster", "fast",
        "medium", "slow", "slower", "veryslow"
    };
    if (qualityCrf < 0 || qualityCrf > 51 || !allowedPresets.contains(encodingPreset))
        return fail("The video quality or encoding preset is invalid.");
    const QList<int> allowedAudioBitrates = {96, 128, 160, 192, 256, 320};
    const QList<int> allowedAudioSampleRates = {44100, 48000};
    if (!allowedAudioBitrates.contains(audioBitrateKbps)
        || !allowedAudioSampleRates.contains(audioSampleRate))
        return fail("The audio bitrate or sample rate is invalid.");

    QJsonArray renderClips;
    if (!expandNestedClips(clips, 0.0, &renderClips, error)) return false;

    struct Segment {
        int input = -1;
        int track = 0;
        double start = 0.0;
        double duration = 0.0;
        double sourceIn = 0.0;
        double rate = 1.0;
        double videoTransitionIn = 0.0;
        double videoTransitionOut = 0.0;
        qint64 timelineStartMs = 0;
        qint64 durationMs = 0;
        int volume = 100;
        bool muted = false;
    };
    QVector<Segment> video;
    QVector<Segment> audio;
    double totalDuration = 0.0;
    int inputIndex = 0;

    for (const QJsonValue &value : renderClips) {
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
        segment.timelineStartMs = qRound64(startMs);
        segment.durationMs = qRound64(durationMs);
        segment.videoTransitionOut = qMax(0.0,
            clip.value("videoTransitionOutMs").toDouble(0.0)) / 1000.0;
        segment.volume = qBound(0, clip.value("audioVolume").toInt(100), 100);
        segment.muted = clip.value("audioMuted").toBool(false);
        segment.videoTransitionIn = qMax(0.0,
            clip.value("videoTransitionInMs").toDouble(0.0)) / 1000.0;
        if (track < 3) {
            if (segment.videoTransitionIn > segment.duration)
                return fail(QString("Clip %1 has a Cross Dissolve longer than the clip.")
                            .arg(clip.value("id").toInt()));
            video.append(segment);
        }
        else audio.append(segment);
        totalDuration = qMax(totalDuration, segment.start + segment.duration);
        *arguments << "-i" << path;
    }

    if (video.isEmpty()) return fail("Add at least one video clip to the timeline.");
    if (totalDuration <= 0.0) return fail("The timeline has no duration.");

    for (const Segment &incoming : video) {
        if (incoming.videoTransitionIn <= 0.0) continue;
        bool matchedOutgoing = false;
        for (const Segment &outgoing : video) {
            if (outgoing.videoTransitionOut <= 0.0) continue;
            if (KadrisAudio::validVideoCrossDissolve(
                    outgoing.timelineStartMs, outgoing.durationMs, outgoing.track,
                    qRound64(outgoing.videoTransitionOut * 1000.0),
                    incoming.timelineStartMs, incoming.durationMs, incoming.track,
                    qRound64(incoming.videoTransitionIn * 1000.0))) {
                matchedOutgoing = true;
                break;
            }
        }
        if (!matchedOutgoing)
            return fail("A Cross Dissolve has no matching outgoing clip on a lower video track.");
    }
    for (const Segment &outgoing : video) {
        if (outgoing.videoTransitionOut <= 0.0) continue;
        bool matchedIncoming = false;
        for (const Segment &incoming : video) {
            if (incoming.videoTransitionIn <= 0.0) continue;
            if (KadrisAudio::validVideoCrossDissolve(
                    outgoing.timelineStartMs, outgoing.durationMs, outgoing.track,
                    qRound64(outgoing.videoTransitionOut * 1000.0),
                    incoming.timelineStartMs, incoming.durationMs, incoming.track,
                    qRound64(incoming.videoTransitionIn * 1000.0))) {
                matchedIncoming = true;
                break;
            }
        }
        if (!matchedIncoming)
            return fail("A Cross Dissolve has no matching incoming clip on a higher video track.");
    }

    std::stable_sort(video.begin(), video.end(), [](const Segment &a, const Segment &b) {
        if (a.track != b.track) return a.track < b.track;
        return a.start < b.start;
    });

    const QString fps = QString("%1/%2").arg(fpsNumerator).arg(fpsDenominator);
    QStringList filters;
    filters << QString("color=c=black:s=%1x%2:r=%3:d=%4[base]")
                   .arg(width).arg(height).arg(fps, number(totalDuration));
    QString current = "base";
    int layer = 0;
    for (const Segment &segment : video) {
        const QString label = QString("v%1").arg(layer);
        QString videoChain = QString("[%1:v:0]trim=start=%2:duration=%3,"
                           "setpts=(PTS-STARTPTS)/%4+%5/TB,"
                           "fps=%6,scale=%7:%8:force_original_aspect_ratio=decrease,"
                           "pad=%7:%8:(ow-iw)/2:(oh-ih)/2,setsar=1")
                       .arg(segment.input).arg(number(segment.sourceIn))
                       .arg(number(segment.duration * segment.rate)).arg(number(segment.rate))
                       .arg(number(segment.start)).arg(fps).arg(width).arg(height);
        if (segment.videoTransitionIn > 0.0) {
            videoChain += QString(",format=rgba,fade=t=in:st=%1:d=%2:alpha=1")
                              .arg(number(segment.start))
                              .arg(number(segment.videoTransitionIn));
        }
        videoChain += QString("[%1]").arg(label);
        filters << videoChain;
        const QString next = QString("mix%1").arg(layer);
        filters << QString("[%1][%2]overlay=eof_action=pass:shortest=0[%3]")
                       .arg(current, label, next);
        current = next;
        ++layer;
    }
    filters << QString("[%1]trim=duration=%2,fps=%3,format=yuv420p[vout]")
                   .arg(current).arg(number(totalDuration)).arg(fps);

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
        QString graph = filters.join(';');
        graph += ';' + audioLabels.join("") +
                 QString("amix=inputs=%1:duration=longest:dropout_transition=0[aout]")
                     .arg(audioLabels.size());
        *arguments << "-filter_complex" << graph << "-map" << "[vout]" << "-map" << "[aout]";
    } else {
        *arguments << "-filter_complex" << filters.join(';') << "-map" << "[vout]";
    }

    *arguments << "-t" << number(totalDuration)
               << "-c:v" << "libx264" << "-preset" << encodingPreset
               << "-crf" << QString::number(qualityCrf)
               << "-pix_fmt" << "yuv420p" << "-r" << fps;
    if (!audio.isEmpty()) *arguments << "-c:a" << "aac" << "-b:a" << QString("%1k").arg(audioBitrateKbps)
                                      << "-ar" << QString::number(audioSampleRate);
    *arguments << "-movflags" << "+faststart" << "-y" << outputPath;
    return true;
}
