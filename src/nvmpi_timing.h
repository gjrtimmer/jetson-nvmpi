/*
 * nvmpi_timing.h — opt-in phase timing for create/close latency (issue #105).
 *
 * Enabled by NVMPI_TIMING=1 in the environment. The variable is read once
 * (C++11 thread-safe static init) and cached; when it is unset every
 * NvmpiTimer call is one branch on a cached bool and no clock is read.
 * When enabled, each phase prints one line to stderr regardless of the
 * NVMPI_LOG threshold, so CI logs can carry it:
 *   [libnvmpi][timing]: dec_create phase=createVideoDecoder ms=0.412
 */
#ifndef __NVMPI_TIMING_H__
#define __NVMPI_TIMING_H__

#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline bool nvmpi_timing_enabled()
{
	/* Initialised once, thread-safe; getenv is not called again. */
	static const bool on = []() {
		const char *v = getenv("NVMPI_TIMING");
		return v && strcmp(v, "1") == 0;
	}();
	return on;
}

//Stack-only stopwatch. phase() prints the time since the previous phase
//(or construction); total() prints the time since construction.
struct NvmpiTimer
{
	const char *scope;                   //e.g. "dec_create"; must be a literal
	bool on;
	std::chrono::steady_clock::time_point t0, last;

	explicit NvmpiTimer(const char *s) : scope(s), on(nvmpi_timing_enabled())
	{
		if (on) t0 = last = std::chrono::steady_clock::now();
	}

	void phase(const char *name)
	{
		if (!on) return;
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		print(name, now - last);
		last = now;
	}

	void total(const char *name = "total")
	{
		if (!on) return;
		print(name, std::chrono::steady_clock::now() - t0);
	}

private:
	void print(const char *name, std::chrono::steady_clock::duration d) const
	{
		fprintf(stderr, "[libnvmpi][timing]: %s phase=%s ms=%.3f\n", scope, name,
		        std::chrono::duration<double, std::milli>(d).count());
	}
};

#endif /* __NVMPI_TIMING_H__ */
