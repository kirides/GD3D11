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
            m_delta = 0.01f; // report a very low number
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
