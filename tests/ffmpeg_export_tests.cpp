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
    assert(args.last() == "render.mp4");

    assert(FfmpegExport::buildArguments(QJsonArray{video}, 1280, 720, 25, 1,
                                        "render.mp4", &args, &error, 18, "slow"));
    assert(args.contains("18"));
    assert(args.contains("slow"));
    assert(!FfmpegExport::buildArguments(QJsonArray{video}, 1280, 720, 25, 1,
                                         "render.mp4", &args, &error, 60, "slow"));
    assert(error.contains("quality"));

    QJsonObject effect = video;
    effect.insert("opacity", 50);
    assert(!FfmpegExport::buildArguments(QJsonArray{effect}, 1280, 720, 25, 1,
                                         "render.mp4", &args, &error));
    assert(error.contains("effects not yet supported"));
    fixture.remove();
    return 0;
}
