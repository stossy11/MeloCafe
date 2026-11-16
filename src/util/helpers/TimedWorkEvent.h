#pragma once

#include "util/helpers/fspinlock.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

// Multiple producers, one waiting thread. Take a snapshot before checking for work.
class TimedWorkEvent
{
public:
	using Clock = std::chrono::steady_clock;

#ifdef __APPLE__
	~TimedWorkEvent()
	{
		dispatch_release(m_semaphore);
	}
#endif

	std::size_t Snapshot() const
	{
		return m_state.load(std::memory_order_acquire) & ~s_waiting;
	}

	void Notify()
	{
		auto state = m_state.load(std::memory_order_relaxed);
		while (!(state & s_waiting))
		{
			if (m_state.compare_exchange_weak(state, state + 2, std::memory_order_release, std::memory_order_relaxed))
				return;
		}

		std::lock_guard lock(m_mutex);
		if (!(m_state.load(std::memory_order_relaxed) & s_waiting))
		{
			m_state.fetch_add(2, std::memory_order_release);
			return;
		}

		m_signalTime = Clock::now();
		// Advance the generation and clear the waiting bit in the same update.
		m_state.fetch_add(1, std::memory_order_release);
#ifdef __APPLE__
		dispatch_semaphore_signal(m_semaphore);
#else
		m_condition.notify_one();
#endif
	}

	void WaitUntil(std::size_t generation, Clock::time_point deadline)
	{
		if (Snapshot() != generation)
			return;

		const auto start = Clock::now();
		if (start >= deadline)
			return;

		const auto spinDeadline = std::min(deadline, start + s_spinBudget);
		const int spinCount = m_shortWaitScore >= 2 ? 35 : 10;
		for (int iteration = 0; iteration < spinCount; ++iteration)
		{
			if (iteration < 10)
				cemuSpinlockPause();
			else
			{
				if (Clock::now() >= spinDeadline)
					break;
				std::this_thread::yield();
			}
			if (Snapshot() != generation)
			{
				RecordWait(Clock::now() < spinDeadline);
				return;
			}
		}

		std::unique_lock lock(m_mutex);
		if (Clock::now() >= deadline)
			return;

		m_signalTime = {};
		auto state = generation;
		if (!m_state.compare_exchange_strong(state, generation | s_waiting, std::memory_order_acq_rel))
			return;

#ifdef __APPLE__
		const auto waitTime = deadline == Clock::time_point::max() ? DISPATCH_TIME_FOREVER :
			dispatch_time(DISPATCH_TIME_NOW, std::chrono::ceil<std::chrono::nanoseconds>(deadline - Clock::now()).count());
		lock.unlock();
		const bool timedOut = dispatch_semaphore_wait(m_semaphore, waitTime) != 0;
		lock.lock();
		// If Notify won the mutex after the timeout, consume its token before rearming.
		if (timedOut && m_signalTime != Clock::time_point{})
			dispatch_semaphore_wait(m_semaphore, DISPATCH_TIME_NOW);
#else
		m_condition.wait_until(lock, deadline, [&] { return Snapshot() != generation; });
#endif
		m_state.fetch_and(~s_waiting, std::memory_order_relaxed);
		// Sample the producer's timestamp even if this thread was rescheduled late.
		RecordWait(m_signalTime != Clock::time_point{} && m_signalTime < spinDeadline);
	}

private:
	void RecordWait(bool shortWait)
	{
		// Keep isolated fast signals or timeouts from immediately undoing a trend.
		m_shortWaitScore = std::clamp(m_shortWaitScore + (shortWait ? 1 : -1), 0, 4);
	}

	static constexpr std::size_t s_waiting = 1;
	static constexpr auto s_spinBudget = std::chrono::microseconds(50);
	std::atomic<std::size_t> m_state{};
	int m_shortWaitScore = 2;
	std::mutex m_mutex;
	Clock::time_point m_signalTime{};
#ifdef __APPLE__
	dispatch_semaphore_t m_semaphore = dispatch_semaphore_create(0);
#else
	std::condition_variable m_condition;
#endif
};
