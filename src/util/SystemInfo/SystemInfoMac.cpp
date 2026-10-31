#include "util/SystemInfo/SystemInfo.h"
#include "Cafe/HW/MMU/MMU.h"

#include <unistd.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>

#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <mach/kern_return.h>


// borrowed from https://en.wikichip.org/wiki/resident_set_size#OS_X
task_vm_info_data_t QueryRamUsage()
{
    task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
		return info;
    
	return {};
}

std::function<uint64()> PrepareGuestRamUsageQuery()
{
	if (!memory_base)
		return [] { return uint64{0}; };

	const long hostPageSize = sysconf(_SC_PAGESIZE);
	if (hostPageSize <= 0)
		return [] { return uint64{0}; };
	const uintptr_t pageSize = static_cast<uintptr_t>(hostPageSize);
    
    
	std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
	for (const auto* range : memory_getMMURanges())
	{
		if (!range->isMapped() || range->getSize() == 0 || range->areaId == MMU_MEM_AREA_ID::CEMU_PRIVATE)
			continue;
		const uintptr_t begin = reinterpret_cast<uintptr_t>(memory_base) + range->getBase();
		
		const uintptr_t end = begin + range->getSize();
		
		ranges.emplace_back(begin - begin % pageSize, ((end + pageSize - 1) / pageSize) * pageSize);
	}
	std::sort(ranges.begin(), ranges.end());

	return [ranges = std::move(ranges), pageSize]() -> uint64 {
		uint64 residentBytes = 0;
		uintptr_t previousEnd = 0;
		char residency[4096];
		for (const auto& [begin, end] : ranges)
		{
			uintptr_t address = std::max(begin, previousEnd);
			previousEnd = std::max(previousEnd, end);
			while (address < end)
			{
				const size_t pages = std::min<size_t>((end - address) / pageSize, sizeof(residency));
				
				if (mincore(reinterpret_cast<char*>(address), pages * pageSize, residency) != 0)
					return 0;
				for (size_t i = 0; i < pages; ++i)
				{
					if (residency[i] & MINCORE_INCORE)
						residentBytes += pageSize;
				}
				address += pages * pageSize;
			}
		}
		return residentBytes;
	};
}

uint64 QueryGuestRamUsage()
{
	return PrepareGuestRamUsageQuery()();
}




// apple official documentation is non-existsent.
// based on https://github.com/giampaolo/psutil/blob/master/psutil/_psutil_osx.c#L623
void QueryCoreTimes(std::vector<ProcessorTime>& out)
{
	// initialize default
	for (auto i = 0; i < out.size(); ++i)
	{
		out[i] = {};
	}

	natural_t cpu_count;
	processor_info_array_t info_array;
	mach_msg_type_number_t info_count;
	kern_return_t error;

	mach_port_t host_port = mach_host_self();
	error = host_processor_info(host_port, PROCESSOR_CPU_LOAD_INFO, &cpu_count, &info_array, &info_count);
	mach_port_deallocate(mach_task_self(), host_port);

	if (error != KERN_SUCCESS)
		return;

	processor_cpu_load_info_data_t* cpuLoad = (processor_cpu_load_info_data_t*) info_array;

    out.resize(cpu_count);

    for (natural_t i = 0; i < cpu_count; ++i)
    {
		uint64 system = cpuLoad[i].cpu_ticks[CPU_STATE_SYSTEM];
		uint64 user = cpuLoad[i].cpu_ticks[CPU_STATE_USER] + cpuLoad[i].cpu_ticks[CPU_STATE_NICE];
		uint64 idle = cpuLoad[i].cpu_ticks[CPU_STATE_IDLE];

		out[i].idle = idle;
		out[i].kernel = system;
		out[i].user = user;
	}

	int ret = vm_deallocate(mach_task_self(), (vm_address_t) info_array,
						info_count * sizeof(int));
	if (ret != KERN_SUCCESS)
		cemuLog_log(LogType::Force, "vm_deallocate() failed");
}
