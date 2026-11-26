#pragma once

#include <cstdint>
#include <string>

class FrameTimebase final
{
public:
    FrameTimebase(std::int64_t fpsNumerator = 25,
                  std::int64_t fpsDenominator = 1) noexcept
        : m_numerator(fpsNumerator > 0 ? fpsNumerator : 25),
          m_denominator(fpsDenominator > 0 ? fpsDenominator : 1) {}

    std::int64_t fpsNumerator() const noexcept { return m_numerator; }
    std::int64_t fpsDenominator() const noexcept { return m_denominator; }

    std::int64_t frameToMilliseconds(std::int64_t frame) const noexcept
    {
        if (frame <= 0) return 0;
        return (frame * 1000 * m_denominator + m_numerator / 2) / m_numerator;
    }

    std::int64_t millisecondsToFrame(std::int64_t milliseconds) const noexcept
    {
        if (milliseconds <= 0) return 0;
        return (milliseconds * m_numerator
                + 500 * m_denominator) / (1000 * m_denominator);
    }

    std::int64_t snapMilliseconds(std::int64_t milliseconds) const noexcept
    {
        return frameToMilliseconds(millisecondsToFrame(milliseconds));
    }

    std::string timecode(std::int64_t frame) const
    {
        if (frame < 0) frame = 0;
        const std::int64_t nominalFps =
            (m_numerator + m_denominator / 2) / m_denominator;
        const std::int64_t framesPerDay = nominalFps * 60 * 60 * 24;
        frame %= framesPerDay;
        const auto frames = frame % nominalFps;
        const auto totalSeconds = frame / nominalFps;
        const auto seconds = totalSeconds % 60;
        const auto minutes = (totalSeconds / 60) % 60;
        const auto hours = totalSeconds / 3600;
        auto two = [](std::int64_t value) {
            return value < 10 ? std::string("0") + std::to_string(value)
                              : std::to_string(value);
        };
        return two(hours) + ":" + two(minutes) + ":"
            + two(seconds) + ":" + two(frames);
    }

private:
    std::int64_t m_numerator;
    std::int64_t m_denominator;
};
