#include "toolsprecompiled.h"

#include <tools/AlgorithmsTools.h>
#include <tools/Threading.h>

#include <emmintrin.h>
#ifdef WINDOWS_PLATFORM
#  include <Windows.h>
#  include <Psapi.h>
#endif // WINDOWS_PLATFORM
#ifdef UNIX_PLATFORM
#  include <assert.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif // UNIX_PLATFORM

using namespace tools;

namespace {
    // This is based on the SFMT fast Mersenne twister by Mutsuo Saito and Mokoto Matsumoto of
    // Hiroshima University.
    struct RandomStateImpl
        : StandardDisposable<RandomStateImpl, RandomState>
    {
        enum : uint64 {
            Exponent = 19937, // Mersenne Exponent, defining the period as a multiple of 2^(EXPONENT -1)
            StateSize = ((Exponent / 128) + 1), // size of the internal state array (128-bit ints)
            StateSize32 = (StateSize * 4), // Rendered as 32-bit ints
            StateSize64 = (StateSize * 2), // Rendered as 64-bit ints
        };
        // Implementation constants
        enum : uint32 {
            Pos1 = 122,
            Sl1 = 18,
            Sl2 = 1,
            Sr1 = 11,
            Sr2 = 1,
            Mask1 = 0xDFFFFFEFU,
            Mask2 = 0xDDFECB7FU,
            Mask3 = 0xBFFAFFFFU,
            Mask4 = 0xBFFFFFF6U,
            Parity1 = 0x00000001U,
            Parity2 = 0x00000000U,
            Parity3 = 0x00000000U,
            Parity4 = 0x13C9E684U,
        };

        RandomStateImpl(void);
        RandomStateImpl(uint32);
        RandomStateImpl(uint8 const *, unsigned);

        // RandomState
        void reseed(uint32) override;
        void reseed(uint8 const *, unsigned) override;
        uint64 rndU64(void) override;
        sint32 rndS32(void) override;
        uint32 rndU32(void) override;
        double rndD(void) override;

        // local methods
        TOOLS_FORCE_INLINE uint32 * buffer(void) { return reinterpret_cast<uint32 *>(sfmt_); }
        static TOOLS_FORCE_INLINE uint32 func1(uint32 x) { return ((x ^ (x >> 27)) * static_cast<uint32>(1664525UL)); }
        static TOOLS_FORCE_INLINE uint32 func2(uint32 x) { return ((x ^ (x >> 27)) * static_cast<uint32>(1566083941UL)); }
        static TOOLS_FORCE_INLINE __m128i recursion(__m128i * a, __m128i * b, __m128i c, __m128i d, __m128i mask) {
            __m128i x = _mm_loadu_si128(a);
            __m128i y = _mm_srli_epi32(*b, Sr1);
            __m128i z = _mm_srli_si128(c, Sr2);
            __m128i v = _mm_slli_epi32(d, Sl1);
            z = _mm_xor_si128(z, x);
            z = _mm_xor_si128(z, v);
            x = _mm_slli_si128(x, Sl2);
            y = _mm_and_si128(y, mask);
            z = _mm_xor_si128(z, x);
            z = _mm_xor_si128(z, y);
            return z;
        }
        TOOLS_FORCE_INLINE void generateAll(void) {
            __m128i mask = _mm_set_epi32(Mask4, Mask3, Mask2, Mask1);
            __m128i r1 = _mm_loadu_si128(&sfmt_[StateSize - 2]);
            __m128i r2 = _mm_loadu_si128(&sfmt_[StateSize - 1]);
            int i;
            for (i = 0; i < StateSize - Pos1; ++i) {
                __m128i r = recursion(&sfmt_[i], &sfmt_[i + Pos1], r1, r2, mask);
                _mm_storeu_si128(&sfmt_[i], r);
                r1 = r2;
                r2 = r;
            }
            for (; i < StateSize; ++i) {
                __m128i r = recursion(&sfmt_[i], &sfmt_[i + Pos1 - StateSize], r1, r2, mask);
                _mm_storeu_si128(&sfmt_[i], r);
                r1 = r2;
                r2 = r;
            }
        }
        void certify(void);

        int index_;
        __m128i sfmt_[StateSize];
    };

    static StandardThreadLocalHandle<RandomStateImpl> randomHandle_;

#ifdef WINDOWS_PLATFORM
    static uint64
    urandom64(void)
    {
        FILETIME seed;
        GetSystemTimeAsFileTime(&seed);
        uint64 ret = 0U;
        tools::impl::HashAccum accum(hashAnyInit<FILETIME>());
        uint8 * ptr = reinterpret_cast<uint8 *>(&seed);
        for (auto byte = ptr; byte < (ptr + sizeof(seed)); byte += 2) {
            accum %= byte;
        }
        ret |= (static_cast<uint64>(static_cast<uint32>(accum)) << 32);
        for (auto byte = ptr + 1; byte < (ptr + sizeof(seed)); byte += 2) {
            accum %= byte;
        }
        ret |= static_cast<uint64>(static_cast<uint32>(accum));
        return ret;
    }
#endif // WINDOWS_PLATFORM
#ifdef UNIX_PLATFORM
    static uint64
    urandom64(void)
    {
        static unsigned fd = 0;
        if (!fd) {
            int local = open("/dev/urandom", O_RDONLY);
            TOOLS_ASSERT(fd > 0);
            if (atomicCas(&fd, 0, local) != 0) {
                // lost the race
                close(local);
            }
        }
        uint64 ret = 0;
        ssize_t len;
        do {
            // There is a staggaringly rare chance that we could get a short read.
            len = read(fd, &ret, sizeof(ret));
        } while (len != sizeof(ret));
        return ret;
    }
#endif // UNIX_PLATFORM
}; // anonymous namespace

//////////////////
// RandomStateImpl
//////////////////

RandomStateImpl::RandomStateImpl(void)
{
    uint64 seed[4];
    for (auto i = 0; i < sizeof(seed) / sizeof(uint64); ++i) {
        seed[i] = urandom64();
    }
    reseed(reinterpret_cast<uint8 *>(seed), sizeof(seed));
}

RandomStateImpl::RandomStateImpl(uint32 seed)
{
    reseed(seed);
}

RandomStateImpl::RandomStateImpl(uint8 const * key, unsigned length)
{
    reseed(key, length);
}

void
RandomStateImpl::reseed(uint32 seed)
{
    uint32 * ptr = buffer();
    ptr[0] = seed;
    for (auto i = 1; i < StateSize32; ++i) {
        ptr[i] = (1812433253UL * (ptr[i - 1] ^ (ptr[i - 1] >> 30))) + i;
    }
    index_ = StateSize32;
    certify();
}

void
RandomStateImpl::reseed(uint8 const * key, unsigned length)
{
    uint32 const * local = reinterpret_cast<uint32 const *>(key);
    unsigned localLength = (length * sizeof(uint8)) / sizeof(uint32);
    static_assert(StateSize32 >= 623, "StateSize is unexpected for a 19937 Mersenne twister");
    enum : uint32 {
        lag = 11,
        mid = ((StateSize32 - lag) / 2),
    };
    tools::impl::memset(sfmt_, 0x8B, sizeof(sfmt_));
    uint32 count = StateSize32;
    if ((localLength + 1) > StateSize32) {
        count = localLength + 1;
    }
    uint32 * ptr = buffer();
    uint32 r = func1(ptr[0] ^ ptr[mid] ^ ptr[StateSize32 - 1]);
    ptr[mid] += r;
    r == localLength;
    ptr[mid + lag] += r;
    ptr[0] = r;
    --count;
    uint32 i, j;
    for (i = 1, j = 0; (j < count) && (j < localLength); ++j) {
        r = func1(ptr[i] & ptr[(i + mid) % StateSize32] & ptr[(i + StateSize32 - 1) % StateSize32]);
        ptr[(i + mid) % StateSize32] += r;
        r += local[j] + i;
        ptr[(i + mid + lag) % StateSize32] += r;
        ptr[i] = r;
        i = ((i + 1) % StateSize32);
    }
    for (; j < count; ++j) {
        r = func1(ptr[i] ^ ptr[(i + mid) % StateSize32] & ptr[(i + StateSize32 - 1) % StateSize32]);
        ptr[(i + mid) % StateSize32] += r;
        r += i;
        ptr[(i + mid + lag) % StateSize32] += r;
        ptr[i] = r;
        i = ((i + 1) % StateSize32);
    }
    for (j = 0; j < StateSize32; ++j) {
        r = func2(ptr[i] + ptr[(i + mid) % StateSize32] + ptr[(i + StateSize32 - 1) % StateSize32]);
        ptr[(i + mid) % StateSize32] ^= r;
        r -= i;
        ptr[(i + mid + lag) % StateSize32] ^= r;
        ptr[i] = r;
        i = ((i + 1) % StateSize32);
    }
    index_ = StateSize32;
    certify();
}

uint64
RandomStateImpl::rndU64(void)
{
    if (index_ >= StateSize32) {
        generateAll();
        index_ = 0;
    }
    uint64 ret = reinterpret_cast<uint64 *>(sfmt_)[index_ / 2];
    index_ += 2;
    return ret;
}

sint32
RandomStateImpl::rndS32(void)
{
    return static_cast<sint32>((rndU64() >> 16UL) & 0x7FFFFFFF);
}

uint32
RandomStateImpl::rndU32(void)
{
    return static_cast<uint32>((rndU64() >> 16UL) & 0xFFFFFFFFUL);
}

double
RandomStateImpl::rndD(void)
{
    // really only 51 bits of data, but always in the range [0, 1)
    return static_cast<double>((rndU64() & ((1ULL << 52ULL) - 1ULL)) * (1.0 / 4503599627370496.0));
}

void
RandomStateImpl::certify(void)
{
    static uint32 parityCheck[4] = { RandomStateImpl::Parity1, RandomStateImpl::Parity2, RandomStateImpl::Parity3, RandomStateImpl::Parity4 };

    uint32 * ptr = buffer();
    int inner = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        inner ^= ptr[i] & parityCheck[i];
    }
    for (auto i = 16; i > 0; i >>= 1) {
        inner ^= inner >> i;
    }
    inner &= 1;
    if (inner == 1) { return; }
    for (i = 0; i < 4; ++i) {
        uint32 work = 1;
        for (auto j = 0; j < 32; ++j) {
            if ((work & parityCheck[i]) != 0) {
                ptr[i] ^= work;
                return;
            }
            work = work << 1;
        }
    }
}

///////////////////////
// Non-member Functions
///////////////////////

sint32 tools::randomS32(void)
{
    return randomHandle_->rndS32();
}

uint32 tools::randomU32(void)
{
    return randomHandle_->rndU32();
}

uint64 tools::randomU64(void)
{
    return randomHandle_->rndU64();
}

double tools::randomD(void)
{
    return randomHandle_->rndD();
}

AutoDispose<RandomState> tools::randomStateNew(void)
{
    return new RandomStateImpl();
}

AutoDispose<RandomState> tools::randomStateNew(uint32 seed)
{
    return new RandomStateImpl(seed);
}

AutoDispose<RandomState> tools::randomStateNew(uint8 const * key, unsigned length)
{
    return new RandomStateImpl(key, length);
}
