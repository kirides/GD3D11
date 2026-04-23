#pragma once
#include <exception>
#include <wrl.h>

#define BASIC_TIMING(x) BasicTimer x = {}; x.Update();

class BasicTimer {
private:
    LARGE_INTEGER m_frequency;
    LARGE_INTEGER m_currentTime;
    LARGE_INTEGER m_startTime;
    LARGE_INTEGER m_lastTime;
    float m_total;
    float m_delta;

public:
    BasicTimer() {
        if ( !QueryPerformanceFrequency( &m_frequency ) ) {
            throw std::exception();
        }

        Reset();
    }

    BasicTimer( const BasicTimer& other ) = delete;
    BasicTimer( BasicTimer&& other ) = default;

    void Reset() {
        m_total = 0;
        m_delta = 1.0f / 60.0f;

        Update();

        m_startTime = m_currentTime;
    }

    void Update() {
        if ( !QueryPerformanceCounter( &m_currentTime ) ) {
            throw std::exception();
        }

        m_total = static_cast<float>( static_cast<double>( m_currentTime.QuadPart - m_startTime.QuadPart ) / static_cast<double>( m_frequency.QuadPart ) );

        if ( m_lastTime.QuadPart == m_startTime.QuadPart ) {
            m_delta = 0.000001f; // report a very low number
        } else {
            m_delta = static_cast<float>( static_cast<double>( m_currentTime.QuadPart - m_lastTime.QuadPart ) / static_cast<double>( m_frequency.QuadPart ) );
        }

        m_lastTime = m_currentTime;
    }

    float GetTotal() const {
        return m_total;
    }

    float GetDelta() const {
        return m_delta;
    }

};

class OneShotTimer {
private:
private:
    // Frequency is constant, cache it once
    static inline const double s_inverseFrequency = []() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency( &freq );
        return 1.0 / static_cast<double>(freq.QuadPart);
    }();

    LARGE_INTEGER m_startTime;

public:
    OneShotTimer() {
        QueryPerformanceCounter( &m_startTime );
    }

    OneShotTimer( const OneShotTimer& other ) = delete;
    OneShotTimer( OneShotTimer&& other ) = default;

    float GetDelta() const {
        LARGE_INTEGER m_currentTime;
        QueryPerformanceCounter( &m_currentTime );

        float total = static_cast<float>(static_cast<double>(m_currentTime.QuadPart - m_startTime.QuadPart) * s_inverseFrequency);

        return total;
    }
};
