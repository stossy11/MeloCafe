#pragma once

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

class ScopedThreadQoS
{
public:
	explicit ScopedThreadQoS(bool enabled = true)
	{
		SetEnabled(enabled);
	}

	~ScopedThreadQoS()
	{
		SetEnabled(false);
	}

	ScopedThreadQoS(const ScopedThreadQoS&) = delete;
	ScopedThreadQoS& operator=(const ScopedThreadQoS&) = delete;

	void SetEnabled(bool enabled)
	{
#ifdef __APPLE__
		if (enabled && !m_override)
			m_override = pthread_override_qos_class_start_np(pthread_self(), QOS_CLASS_USER_INTERACTIVE, 0);
		else if (!enabled && m_override)
		{
			pthread_override_qos_class_end_np(m_override);
			m_override = nullptr;
		}
#else
		(void)enabled;
#endif
	}

private:
#ifdef __APPLE__
	pthread_override_t m_override = nullptr;
#endif
};
