/*
 * nvmpi_pts.h - total pts round trip through the V4L2 buffer timestamp.
 *
 * Both codecs carry the caller's int64 microsecond pts in the V4L2 buffer
 * timeval (V4L2_BUF_FLAG_TIMESTAMP_COPY). The Tegra kernel stores that
 * timeval as u64 nanoseconds (v4l2_timeval_to_ns: sec * 1e9 + usec * 1e3,
 * wrapping) and reads it back as signed ns, so a microsecond value only
 * survives unchanged inside [0, INT64_MAX / 1000] (9223372036854775 us,
 * ~292 000 years). Negative values (AV_NOPTS_VALUE is INT64_MIN) and huge
 * ones were truncated (issue #148).
 *
 * encode() therefore passes [0, kPtsPassMax] through byte for byte and maps
 * every other int64 to a reserved sentinel in the top kPtsSlots values of
 * the safe range, remembering the original in a small side table; decode()
 * turns a sentinel back into the original. Normal pts never touch the table
 * (no lock, no allocation); only out-of-range values take the slow path.
 *
 * Thread safety: encode() runs on the producer (user) thread, decode() on
 * the capture/DQ thread. The table slots are atomics; encode() serialises
 * concurrent producers with a mutex (slow path only).
 *
 * Bound: kPtsSlots distinct out-of-range values are tracked. A value equal
 * to one already stored reuses its slot (a stream of AV_NOPTS_VALUE needs
 * one slot). When all slots are used the oldest is recycled, so a frame
 * still in flight after kPtsSlots newer distinct out-of-range puts would
 * come back with a newer value; the decoder/encoder pipelines hold far
 * fewer frames than that.
 */
#ifndef NVMPI_PTS_H
#define NVMPI_PTS_H

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <sys/time.h>

static const int64_t kPtsSafeMax = INT64_MAX / 1000;   /* last exact us   */
enum { kPtsSlots = 256 };                                /* side table size */
static const int64_t kPtsPassMax = kPtsSafeMax - kPtsSlots; /* last passthru */
static const int64_t kPtsSentBase = kPtsPassMax + 1;     /* first sentinel  */

class nvmpi_pts_map
{
public:
	nvmpi_pts_map()
	{
		for (int i = 0; i < kPtsSlots; i++)
			slot_[i].store(0, std::memory_order_relaxed);
	}

	/* int64 us pts -> timeval that survives the kernel round trip. */
	void encode(int64_t pts, struct timeval *tv)
	{
		int64_t wire = pts;

		if (pts < 0 || pts > kPtsPassMax)
			wire = encode_slow(pts);
		tv->tv_sec = (time_t)(wire / 1000000);
		tv->tv_usec = (suseconds_t)(wire % 1000000);
	}

	/* timeval read back from the buffer -> the caller's original pts. A
	 * value that is not a sentinel (including one the kernel mangled) is
	 * returned as read. */
	int64_t decode(const struct timeval &tv) const
	{
		/* unsigned arithmetic: wraps instead of overflowing on garbage */
		int64_t wire = (int64_t)((uint64_t)tv.tv_sec * 1000000u
		                         + (uint64_t)tv.tv_usec);

		if (wire < kPtsSentBase || wire > kPtsSafeMax)
			return wire;
		return slot_[wire - kPtsSentBase].load(std::memory_order_acquire);
	}

private:
	/* Slow path: find or allocate the sentinel of an out-of-range pts. */
	int64_t encode_slow(int64_t pts)
	{
		std::lock_guard<std::mutex> lk(mu_);

		for (int i = 0; i < used_; i++)
			if (slot_[i].load(std::memory_order_relaxed) == pts)
				return kPtsSentBase + i;
		int i = next_;
		next_ = (next_ + 1) % kPtsSlots;
		if (used_ < kPtsSlots)
			used_++;
		/* release: pairs with the acquire in decode() */
		slot_[i].store(pts, std::memory_order_release);
		return kPtsSentBase + i;
	}

	std::atomic<int64_t> slot_[kPtsSlots];
	std::mutex mu_;          /* guards next_, used_ and slot writes */
	int next_{0};
	int used_{0};
};

#endif /* NVMPI_PTS_H */
