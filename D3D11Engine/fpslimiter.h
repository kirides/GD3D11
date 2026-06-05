#pragma once

#include <chrono>
#include <thread>
#include <Windows.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ntdll.lib")

// NtSetTimerResolution for more precise timer control
typedef LONG(NTAPI* pNtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef LONG(NTAPI* pNtQueryTimerResolution)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);

struct FpsLimiter {
public:
	FpsLimiter() {
		m_frequency = 0;
		m_counterStart = 0;
		m_speedLimit = 0;
		m_enabled = false;
		m_fps = 0;
		m_timeStart = 0;
		m_timerResolutionSet = false;
		m_waitableTimer = nullptr;
		m_ntSetTimerResolution = nullptr;
		m_ntQueryTimerResolution = nullptr;
		m_currentResolution = 0;

		// Load NtSetTimerResolution from ntdll.dll
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (ntdll) {
			m_ntSetTimerResolution = reinterpret_cast<pNtSetTimerResolution>(
				GetProcAddress(ntdll, "NtSetTimerResolution"));
			m_ntQueryTimerResolution = reinterpret_cast<pNtQueryTimerResolution>(
				GetProcAddress(ntdll, "NtQueryTimerResolution"));
		}
	}

	~FpsLimiter() {
		Reset();
		if (m_waitableTimer) {
			CloseHandle(m_waitableTimer);
			m_waitableTimer = nullptr;
		}
	}

	void Start() {
		m_timeStart = GetCounter();
		m_enabled = true;
	}

	void Wait() {
		if (!m_enabled) {
			return;
		}
        ZoneScopedN("FpsLimiter::Wait");

		auto timeNow = GetCounter();
		auto frameDuration = timeNow - m_timeStart;
		
		if (frameDuration < m_speedLimit) {
			auto waittime = m_speedLimit - frameDuration;
			
			// Use a hybrid approach with thresholds adjusted for high-resolution timer:
			// With NtSetTimerResolution (0.5ms) we can use much lower thresholds than before
			// 1. For waits > 1ms, use waitable timer
			// 2. For waits 0.25ms-1ms, use short sleep + spin
			// 3. For waits < 0.25ms, just spin
			
			// Adjusted thresholds to take advantage of 0.5ms timer resolution
			const double SPIN_THRESHOLD = 0.25;
			const double SLEEP_THRESHOLD = 1.0;
			
			if (waittime > SLEEP_THRESHOLD) {
				// Use high-resolution waitable timer for longer waits
				if (m_waitableTimer) {
					// With 0.5ms resolution, we can leave less margin for spin
					// Leave 0.5ms for final spin to account for timer inaccuracy
					double sleepTime = waittime - 0.5;
					if (sleepTime > 0) {
						LARGE_INTEGER dueTime;
						// Negative value means relative time in 100-nanosecond intervals
						dueTime.QuadPart = -static_cast<LONGLONG>(sleepTime * 10000.0);
						
						if (SetWaitableTimer(m_waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
							WaitForSingleObject(m_waitableTimer, INFINITE);
						}
					}
				} else {
					// Fallback to sleep_until if waitable timer not available
					// Leave 0.5ms for final spin
					double sleepTime = waittime - 0.5;
					if (sleepTime > 0) {
						auto sleepDuration = std::chrono::duration<double, std::milli>(sleepTime);
						std::this_thread::sleep_until(std::chrono::high_resolution_clock::now() + 
							std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(sleepDuration));
					}
				}
				
				// Spin for the remaining time
				while (GetCounter() - timeNow < waittime) {
					// YieldProcessor hint to CPU for spin-wait loops
					YieldProcessor();
				}
			}
			else if (waittime > SPIN_THRESHOLD) {
				// For medium waits, do short sleeps + spin
				// With 0.5ms resolution, Sleep(0) or Sleep(1) becomes viable
				double sleepTime = waittime - SPIN_THRESHOLD;
				if (sleepTime > 0) {
					// Sleep for 0.5ms increments when possible
					DWORD sleepMs = static_cast<DWORD>(sleepTime);
					if (sleepMs > 0) {
						Sleep(sleepMs);
					} else if (sleepTime >= 0.25) {
						// Even Sleep(0) can be useful with high timer resolution
						// It yields to other threads but returns quickly
						Sleep(0);
					}
				}
				
				// Spin for the remaining time
				while (GetCounter() - timeNow < waittime) {
					YieldProcessor();
				}
			}
			else {
				// For short waits, just spin with YieldProcessor
				while (GetCounter() - timeNow < waittime) {
					YieldProcessor();
				}
			}
			
			timeNow = GetCounter();
		}

		m_timeStart = timeNow;
	}

	void Reset() {
		m_enabled = false;
		
		// Reset timer resolution if we set it
		if (m_timerResolutionSet) {
			// Reset NtSetTimerResolution
			if (m_ntSetTimerResolution && m_currentResolution > 0) {
				ULONG currentRes;
				m_ntSetTimerResolution(m_currentResolution, FALSE, &currentRes);
			}
			
			// Also reset timeBeginPeriod as a fallback
			timeEndPeriod(1);
			m_timerResolutionSet = false;
			m_currentResolution = 0;
		}
	}

	int GetLimit() { return m_fps; }
	
	void SetLimit(int framerateLimit) {
		constexpr int MIN_FPS = 1;
		if (framerateLimit < MIN_FPS)
			framerateLimit = MIN_FPS;

		// Clean up if disabling
		if (m_enabled && framerateLimit <= 0) {
			Reset();
		}

		m_fps = framerateLimit;
		m_enabled = framerateLimit > 0;
		m_timeStart = 0;
		
		if (m_enabled) {
			m_speedLimit = 1000.0 / framerateLimit;
			
			LARGE_INTEGER li;
			QueryPerformanceFrequency(&li);
			m_frequency = double(li.QuadPart) / 1000.0;

			QueryPerformanceCounter(&li);
			m_counterStart = li.QuadPart;

			m_timeStart = GetCounter();
			
			// Set Windows timer resolution for better Sleep/timer accuracy
			// Try NtSetTimerResolution first (more precise control)
			if (m_ntSetTimerResolution && m_ntQueryTimerResolution) {
				ULONG minRes, maxRes, currentRes;
				
				// Query available timer resolutions
				// Resolution values are in 100-nanosecond units
				if (m_ntQueryTimerResolution(&minRes, &maxRes, &currentRes) == 0) {
					// maxRes is the highest resolution (smallest interval)
					// Typical values: maxRes = 5000 (0.5ms), minRes = 156250 (15.625ms)
					
					// Try to set to maximum resolution (0.5ms if available)
					ULONG desiredRes = maxRes;
					if (m_ntSetTimerResolution(desiredRes, TRUE, &currentRes) == 0) {
						m_currentResolution = currentRes;
						m_timerResolutionSet = true;
					}
				}
			}
			
			// Fallback to timeBeginPeriod if NtSetTimerResolution not available or failed
			if (!m_timerResolutionSet) {
				if (timeBeginPeriod(1) == TIMERR_NOERROR) {
					m_timerResolutionSet = true;
				}
			}
			
			// Create a high-resolution waitable timer if not already created
			if (!m_waitableTimer) {
				m_waitableTimer = CreateWaitableTimerExW(
					nullptr,
					nullptr,
					CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
					TIMER_ALL_ACCESS
				);
				
				// Fall back to regular waitable timer if high-resolution not supported
				if (!m_waitableTimer) {
					m_waitableTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
				}
			}
		}
	}

private:
	unsigned int m_fps;
	bool m_enabled;
	double m_speedLimit;
	__int64 m_counterStart;
	double m_timeStart;
	double m_frequency;
	bool m_timerResolutionSet;
	HANDLE m_waitableTimer;
	ULONG m_currentResolution;
	pNtSetTimerResolution m_ntSetTimerResolution;
	pNtQueryTimerResolution m_ntQueryTimerResolution;

	double GetCounter() {
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return double(li.QuadPart - m_counterStart) / m_frequency;
	}
};
