#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Timing and rate calculation are independent of the window and playback graph.
class HoldSpeedGesture
{
    bool m_tracking = false, m_active = false;
    uint64_t m_started = 0;
    int m_anchorX = 0;
    double m_originalRate = 1.0, m_rate = 2.0;

public:
    static constexpr uint64_t DelayMs = 450;
    bool IsTracking() const { return m_tracking; }
    bool IsActive() const { return m_active; }

    void Begin(uint64_t now, double originalRate)
    {
        m_tracking = true;
        m_active = false;
        m_started = now;
        m_originalRate = originalRate;
        m_rate = 2.0;
    }

    std::optional<double> Update(uint64_t now, int screenX, int dragSpan)
    {
        if (!m_tracking) return std::nullopt;
        if (!m_active) {
            if (now - m_started < DelayMs) return std::nullopt;
            m_active = true;
            m_anchorX = screenX;
            return m_rate;
        }
        double rate = 2.0 + 3.0 * (screenX - m_anchorX) / std::max(1, dragSpan);
        rate = std::clamp(std::round(rate * 10.0) / 10.0, 2.0, 5.0);
        if (rate == m_rate) return std::nullopt;
        m_rate = rate;
        return rate;
    }

    std::optional<double> End()
    {
        std::optional<double> restore;
        if (m_active) restore = m_originalRate;
        m_tracking = m_active = false;
        return restore;
    }
};
