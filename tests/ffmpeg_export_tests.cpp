#include "ffmpeg_export.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QFile>

#include <cassert>

int main()
{
    const QJsonObject clip{{"id", 1}, {"path", "fixture.mp4"}, {"track", 0},
        {"timelineStartMs", 1000}, {"sourceInMs", 500}, {"durationMs", 2000},
        {"playbackRate", 2.0}};
    QStringList args;
    QString error;
    assert(!FfmpegExport::buildArguments(QJsonArray{clip}, 1920, 1080, 25, 1,
                                         "out.mp4", &args, &error));
    assert(error.contains("missing"));

    QJsonObject audio{{"id", 2}, {"path", "fixture.wav"}, {"track", 3},
        {"timelineStartMs", 0}, {"sourceInMs", 0}, {"durationMs", 1000}};
    QJsonObject video{{"id", 3}, {"path", "fixture.mp4"}, {"track", 0},
        {"timelineStartMs", 500}, {"sourceInMs", 100}, {"durationMs", 2000},
        {"playbackRate", 2.0}};
    // Use existing paths in the test working directory only when supplied by CI.
    // Path validation is exercised separately above; this test uses a local placeholder
    // file to verify deterministic argument construction.
    QFile fixture("ffmpeg-export-fixture.mp4");
    assert(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    video.insert("path", fixture.fileName());
    audio.insert("path", fixture.fileName());

    assert(FfmpegExport::buildArguments(QJsonArray{video, audio}, 1280, 720, 25, 1,
                                        "render.mp4", &args, &error));
    assert(args.contains("-filter_complex"));
    assert(args.join(" ").contains("setpts=(PTS-STARTPTS)/2.000000"));
    assert(args.join(" ").contains("amix=inputs=1"));
    assert(args.contains("[vout]"));
    assert(args.contains("[aout]"));
    assert(args.contains("-crf"));
    assert(args.contains("23"));
    assert(args.contains("-preset"));
    assert(args.contains("medium"));
    assert(args.contains("-b:a"));
    assert(args.contains("192k"));
    assert(args.contains("-ar"));
    assert(args.contains("48000"));
    assert(args.last() == "render.mp4");

    QJsonObject outgoing = video;
    outgoing.insert("timelineStartMs", 0);
    outgoing.insert("durationMs", 2000);
    outgoing.insert("track", 0);
    QJsonObject incoming = video;
    incoming.insert("timelineStartMs", 1500);
    incoming.insert("durationMs", 2000);
    incoming.insert("track", 1);
    incoming.insert("videoTransitionInMs", 500);
    assert(FfmpegExport::buildArguments(QJsonArray{incoming, outgoing}, 1280, 720, 25, 1,
                                        "cross-dissolve.mp4", &args, &error));
    const QString dissolveGraph = args.join(" ");
    assert(dissolveGraph.contains("fade=t=in:st=1.500000:d=0.500000:alpha=1"));
    assert(dissolveGraph.contains("+1.500000/TB"));
    assert(dissolveGraph.contains("overlay=eof_action=pass:shortest=0"));
    assert(dissolveGraph.indexOf("[1:v:0]") < dissolveGraph.indexOf("[0:v:0]"));

    assert(FfmpegExport::buildArguments(QJsonArray{video}, 1280, 720, 25, 1,
                                        "render.mp4", &args, &error, 18, "slow"));
    assert(args.contains("18"));
    assert(args.contains("slow"));
    assert(FfmpegExport::buildArguments(QJsonArray{video, audio}, 1280, 720, 25, 1,
                                        "render.mp4", &args, &error, 23, "medium", 320, 44100));
    assert(args.contains("320k"));
    assert(args.contains("44100"));
    assert(!FfmpegExport::buildArguments(QJsonArray{video, audio}, 1280, 720, 25, 1,
                                         "render.mp4", &args, &error, 23, "medium", 500, 48000));
    assert(error.contains("audio"));
    assert(!FfmpegExport::buildArguments(QJsonArray{video}, 1280, 720, 25, 1,
                                         "render.mp4", &args, &error, 60, "slow"));
    assert(error.contains("quality"));

    QJsonObject effect = video;
    effect.insert("opacity", 50);
    assert(!FfmpegExport::buildArguments(QJsonArray{effect}, 1280, 720, 25, 1,
                                         "render.mp4", &args, &error));
    assert(error.contains("effects not yet supported"));

    QJsonObject nestedVideo = video;
    nestedVideo.insert("timelineStartMs", 250);
    QJsonObject nestedAudio = audio;
    nestedAudio.insert("timelineStartMs", 500);
    QJsonObject nested{{"id", 10}, {"path", "nest://10"}, {"track", 0},
        {"timelineStartMs", 3000}, {"durationMs", 3000}, {"durationKnown", true},
        {"nestedSequence", true}, {"nestedName", "Intro"},
        {"nestedClips", QJsonArray{nestedVideo, nestedAudio}}};
    assert(FfmpegExport::buildArguments(QJsonArray{nested}, 1920, 1080, 25, 1,
                                        "nested-render.mp4", &args, &error));
    const QString nestedGraph = args.join(" ");
    assert(!nestedGraph.contains("nest://"));
    assert(nestedGraph.contains("+3.250000/TB"));
    assert(nestedGraph.contains("+3.500000/TB"));
    assert(nestedGraph.contains("amix=inputs=1"));

    QJsonObject unsupportedNested = nested;
    unsupportedNested.insert("playbackRate", 2.0);
    assert(!FfmpegExport::buildArguments(QJsonArray{unsupportedNested}, 1920, 1080, 25, 1,
                                         "nested-render.mp4", &args, &error));
    assert(error.contains("Nested sequence"));

    fixture.remove();
    return 0;
}
