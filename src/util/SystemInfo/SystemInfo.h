#pragma once
#if __APPLE__
#include <mach/mach.h>
#include <functional>
#endif

struct ProcessorTime
{
	uint64 idle{}, kernel{}, user{};

	uint64 work() const;
	uint64 total() const;

	static double Compare(const ProcessorTime& last, const ProcessorTime& now);
};

uint32 GetProcessorCount();
#if __APPLE__
task_vm_info_data_t QueryRamUsage();
std::function<uint64()> PrepareGuestRamUsageQuery();
#else
uint64 QueryRamUsage();
#endif
uint64 QueryGuestRamUsage();
void QueryProcTime(uint64 &out_now, uint64 &out_user, uint64 &out_kernel);
void QueryProcTime(ProcessorTime &out);
void QueryCoreTimes(std::vector<ProcessorTime>& out);
