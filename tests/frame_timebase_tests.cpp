#include "frame_timebase.h"

#include <cassert>

int main()
{
    const FrameTimebase film(24, 1);
    assert(film.frameToMilliseconds(24) == 1000);
    assert(film.millisecondsToFrame(1000) == 24);
    assert(film.snapMilliseconds(1040) == 1042);
    assert(film.timecode(86400) == "01:00:00:00");

    const FrameTimebase pal(25, 1);
    assert(pal.frameToMilliseconds(25) == 1000);
    assert(pal.millisecondsToFrame(1234) == 31);
    assert(pal.snapMilliseconds(1234) == 1240);
    assert(pal.timecode(90061) == "01:00:02:11");

    const FrameTimebase ntsc(30000, 1001);
    assert(ntsc.frameToMilliseconds(30) == 1001);
    assert(ntsc.millisecondsToFrame(1001) == 30);

    const FrameTimebase fallback(0, 0);
    assert(fallback.fpsNumerator() == 25);
    assert(fallback.fpsDenominator() == 1);
    return 0;
}
