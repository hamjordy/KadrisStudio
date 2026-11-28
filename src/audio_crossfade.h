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

// Linear video fade envelope based on clip-local timeline position.
inline double videoFadeOpacity(qint64 elapsedMs, qint64 durationMs,
                               qint64 fadeInMs, qint64 fadeOutMs)
{
    if (durationMs <= 0) return 1.0;
    const qint64 elapsed = qMax<qint64>(0, elapsedMs);
    double gain = 1.0;
    if (fadeInMs > 0 && elapsed < fadeInMs)
        gain = qMin(gain, static_cast<double>(elapsed) / fadeInMs);
    const qint64 remaining = qMax<qint64>(0, durationMs - elapsed);
    if (fadeOutMs > 0 && remaining < fadeOutMs)
        gain = qMin(gain, static_cast<double>(remaining) / fadeOutMs);
    return qBound(0.0, gain, 1.0);
}


// Linear opacity envelope for a true two-image Cross Dissolve.
// The outgoing image fades from 100% to 0%; the incoming image
// fades from 0% to 100% over the same transition interval.
inline double videoCrossDissolveOpacity(qint64 positionMs,
                                        qint64 transitionStartMs,
                                        qint64 transitionEndMs,
                                        bool incoming)
{
    if (transitionEndMs <= transitionStartMs)
        return incoming ? 1.0 : 0.0;

    const double progress = qBound(0.0,
        static_cast<double>(positionMs - transitionStartMs)
            / static_cast<double>(transitionEndMs - transitionStartMs),
        1.0);
    return incoming ? progress : 1.0 - progress;
}

// Update a track's peak hold and latch clipping at digital full scale.
// Returns the level to display; a latched clip keeps its peak until reset.
inline float updatePeakHold(float outputPeak, float &peakHold, bool &clipped)
{
    if (!std::isfinite(outputPeak))
        outputPeak = 0.0f;
    outputPeak = qBound(0.0f, outputPeak, 1.0f);
    if (outputPeak >= 0.999f)
        clipped = true;

    if (outputPeak >= peakHold)
        peakHold = outputPeak;
    else if (!clipped)
        peakHold *= 0.94f;

    return qMax(outputPeak, peakHold);
}

inline void resetPeakHold(float &peakHold, bool &clipped)
{
    peakHold = 0.0f;
    clipped = false;
}

} // namespace KadrisAudio
