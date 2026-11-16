#pragma once

#include "util/helpers/TimedWorkEvent.h"

inline TimedWorkEvent& Latte_GetWorkEvent()
{
	static TimedWorkEvent event;
	return event;
}
