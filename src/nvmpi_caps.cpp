/*
 * nvmpi_caps.cpp — capability bits of this libnvmpi build (#166).
 *
 * Lets a dlopen caller ask what the loaded library does instead of parsing
 * its file name. Add a bit here together with the behaviour it reports;
 * bits are never reused.
 */
#include "nvmpi.h"

extern "C" unsigned int nvmpi_get_caps(void)
{
	/* NVMPI_CAP_DEC_EOS_DRAIN: the zero-size put_packet drain
	 * (nvmpi_dec_api.cpp, nvmpi_dec_capture.cpp) is compiled into every
	 * build variant (the WITH_NVUTILS guards there only pick the buffer API). */
	return NVMPI_CAP_DEC_EOS_DRAIN;
}
