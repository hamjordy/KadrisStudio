#pragma once

#include <QtGlobal>

#include <cmath>

namespace KadrisAudio {

// Equal-power envelope for a two-clip overlap. The outgoing and incoming
// gains are cosine/sine complements, so their squared gains sum to one.
inline double equalPowerCrossfadeGain(qint64 positionMs,
                                      qint64 overlapStartMs,
                                      qint64 overlapEndMs,
                                      bool incoming)
{
    if (overlapEndMs <= overlapStartMs)
        return 1.0;

    const double progress = qBound(0.0,
        static_cast<double>(positionMs - overlapStartMs)
            / static_cast<double>(overlapEndMs - overlapStartMs),
        1.0);
    constexpr double halfPi = 1.5707963267948966;
    const double angle = progress * halfPi;
    return incoming ? std::sin(angle) : std::cos(angle);
}

} // namespace KadrisAudio
