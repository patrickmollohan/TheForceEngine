#pragma once
//////////////////////////////////////////////////////////////////////
// Wall
// Dark Forces Derived Renderer - Wall functions
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>
#include <math.h>

// Experiment with a drop in way of fixing precision issues at higher resolutions.
#define ENABLE_HIGH_PRECISION_FIXED_POINT 0

#if ENABLE_HIGH_PRECISION_FIXED_POINT == 0
typedef s32 fixed16;
#define HALF_16 0x8000
#define ONE_16 0x10000
#define FRAC_BITS 16ll
#define SUB_TEXEL_SHIFT 6
#define FLOAT_SCALE 65536.0f
#define ANGLE_TO_FIXED_SCALE 4
#else
typedef s64 fixed16;
#define HALF_16  0x80000ll
#define ONE_16  0x100000ll
#define FRAC_BITS 20ll
#define SUB_TEXEL_SHIFT 10ll
#define FLOAT_SCALE 1048576.0f
#define ANGLE_TO_FIXED_SCALE 64
#endif

namespace FixedPoint
{
	// multiplies 2 fixed point numbers, the result is fixed point.
	// 16.16 * 16.16 overflows 32 bit, so the calculation is done in 64 bit and then shifted back to 32 bit.
	inline fixed16 mul16(fixed16 x, fixed16 y)
	{
		const s64 x64 = s64(x);
		const s64 y64 = s64(y);
		return fixed16((x64 * y64) >> FRAC_BITS);
	}

	// divides 2 fixed point numbers, the result is fixed point.
	// 16.16 * FIXED_ONE overflows 32 bit, so the calculation is done in 64 bit but the result can be safely cast back to 32 bit.
	inline fixed16 div16(fixed16 num, fixed16 denom)
	{
		const s64 num64 = s64(num);
		const s64 den64 = s64(denom);
		return fixed16((num64 << FRAC_BITS) / den64);
	}

	// truncates a 16.16 fixed point number, returns an int: x >> 16
	inline s32 floor16(fixed16 x)
	{
		return s32(x >> FRAC_BITS);
	}

	// computes a * b / c while keeping everything in 64 bits until the end.
	inline fixed16 fusedMulDiv(fixed16 a, fixed16 b, fixed16 c)
	{
		const s64 a64 = s64(a);
		const s64 b64 = s64(b);
		const s64 c64 = s64(c);
		s64 value = (a64 * b64) / c64;
		return fixed16(value);
	}
	
	// computes a * b * c while keeping everything in 64 bits until the end.
	inline fixed16 fusedMulMul(fixed16 a, fixed16 b, fixed16 c)
	{
		const s64 a64 = s64(a);
		const s64 b64 = s64(b);
		const s64 c64 = s64(c);
		s64 value = (a64 * b64 * c64) >> (FRAC_BITS + FRAC_BITS);
		return fixed16(value);
	}

	// rounds a 16.16 fixed point number, returns an int: (x + HALF_16) >> 16
	inline s32 round16(fixed16 x)
	{
		return s32((x + HALF_16) >> FRAC_BITS);
	}

	// converts an integer to a fixed point number: x << 16
	inline fixed16 intToFixed16(s32 x)
	{
		return fixed16(x) << FRAC_BITS;
	}

	inline fixed16 floatToFixed16(f32 x)
	{
		return fixed16(x * FLOAT_SCALE);
	}

	// Convert a floating point angle from [0, 2pi) to a fixed point value where 0 -> 0 and 2pi -> 16384
	// This isn't really the same as fixed16 but matches the original source.
	inline fixed16 floatAngleToFixed(f32 angle)
	{
		return fixed16(16384.0f * angle / (2.0f*PI));
	}

	inline s32 fixed16to12(fixed16 x)
	{
		return s32(x >> 4);
	}
}
