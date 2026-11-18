#include "x87.h"

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>

namespace raven::translate {

bool x87_double_arithmetic = false;

namespace {

__extension__ typedef unsigned __int128 u128;

constexpr uint64_t one = uint64_t(1) << 63;
constexpr uint64_t quiet = uint64_t(1) << 62;
constexpr int bias = 16383;

// FSW. C1 also reports the direction of a stack fault (1 overflow, 0 underflow) and whether an inexact result was rounded up.
constexpr uint16_t IE = 1, DE = 2, ZE = 4, OE = 8, UE = 16, PE = 32, SF = 64, ES = 128, C0 = 0x100, C1 = 0x200, C2 = 0x400,
	C3 = 0x4000, B = 0x8000;
constexpr uint16_t exception_bits = IE | DE | ZE | OE | UE | PE;
// Exceptions that stop an instruction before it writes anything when they are unmasked.
constexpr uint16_t before_result = IE | DE | ZE;

// The format's tininess rule: whether the result, rounded to the precision with an unbounded exponent, is below the smallest
// normal number (after rounding) or the exact result is (before).
constexpr bool tininess_after_rounding = true;

struct F80 {
	uint64_t sig = 0;
	uint16_t se = 0;
	bool sign() const { return se >> 15; }
	unsigned exponent() const { return se & 0x7fffu; }
};

constexpr F80 indefinite{0xc000000000000000, 0xffff};

enum class Kind : uint8_t { zero, denormal, normal, infinity, qnan, snan, unsupported };

Kind kind(F80 f)
{
	const bool integer = f.sig >> 63;
	if (f.exponent() == 0)
		return f.sig == 0 ? Kind::zero : Kind::denormal;
	if (f.exponent() == 0x7fff) {
		if (!integer)
			return Kind::unsupported;
		if ((f.sig << 1) == 0)
			return Kind::infinity;
		return (f.sig & quiet) ? Kind::qnan : Kind::snan;
	}
	return integer ? Kind::normal : Kind::unsupported;
}

bool is_nan(Kind k)
{
	return k == Kind::qnan || k == Kind::snan;
}

bool finite_nonzero(Kind k)
{
	return k == Kind::normal || k == Kind::denormal;
}

F80 make(bool sign, uint16_t exponent, uint64_t sig)
{
	return {sig, static_cast<uint16_t>((sign ? 0x8000u : 0u) | exponent)};
}

F80 infinity(bool sign)
{
	return make(sign, 0x7fff, one);
}

F80 zero(bool sign)
{
	return make(sign, 0, 0);
}

// A finite nonzero value as sig * 2^(exp - 63), sig with bit 63 set. A pseudo-denormal (exponent 0, integer bit set) has the value of
// exponent 1.
struct Unpacked {
	bool sign;
	int exp;
	uint64_t sig;
};

Unpacked unpack(F80 f)
{
	Unpacked u{f.sign(), static_cast<int>(f.exponent()) - bias, f.sig};
	if (f.exponent() == 0) {
		u.exp = 1 - bias;
		const int shift = std::countl_zero(f.sig);
		u.sig <<= shift;
		u.exp -= shift;
	}
	return u;
}

struct Format {
	int precision;
	int emin;
	int emax;
	// What an unmasked overflow or underflow adds to or takes from the exponent of a result kept in a register.
	int wrap;
};

constexpr Format extended{64, 1 - bias, bias, 24576};
constexpr Format double_format{53, -1022, 1023, 1536};
constexpr Format single_format{24, -126, 127, 192};

Format precision_control(uint16_t fcw)
{
	switch (fcw >> 8 & 3) {
	case 0: return {24, extended.emin, extended.emax, extended.wrap};
	case 2: return {53, extended.emin, extended.emax, extended.wrap};
	default: return extended;
	}
}

bool round_up(u128 rest, u128 half, bool odd, bool sign, unsigned rc)
{
	if (!rest)
		return false;
	switch (rc) {
	case 0: return rest > half || (rest == half && odd);
	case 1: return sign;
	case 2: return !sign;
	default: return false;
	}
}

// A rounded result: m holds the significand at bits 127 down to 128 - precision, bit 127 being the integer bit unless the value is
// below the format's smallest normal (then exp is emin).
struct Rounded {
	bool sign = false;
	int exp = 0;
	u128 m = 0;
	bool infinite = false;
};

struct Flags {
	uint16_t bits = 0;
	bool up = false;
};

// m * 2^(exp - 127), m with bit 127 set, rounded to format under rc. With unmasked overflow or underflow (masks says), the result
// keeps the precision with its exponent wrapped into range, as x87 leaves it in a register.
Rounded round(bool sign, int exp, u128 m, Format format, unsigned rc, uint16_t masks, Flags &flags)
{
	const int lsb = 128 - format.precision;
	const u128 mask = (u128(1) << lsb) - 1;
	auto at_precision = [&](u128 value, int e, int &out_exp, bool &up) {
		const u128 rest = value & mask;
		up = round_up(rest, u128(1) << (lsb - 1), (value >> lsb) & 1, sign, rc);
		value -= rest;
		out_exp = e;
		if (up) {
			value += u128(1) << lsb;
			if (value == 0) {
				value = u128(1) << 127;
				++out_exp;
			}
		}
		return value;
	};
	Rounded result;
	result.sign = sign;
	int unbounded_exp = 0;
	bool up = false;
	const u128 unbounded = at_precision(m, exp, unbounded_exp, up);
	const bool inexact = (m & mask) != 0;
	const bool tiny = tininess_after_rounding ? unbounded_exp < format.emin : exp < format.emin;
	if (unbounded_exp > format.emax) {
		if (!(masks & OE)) {
			flags.bits |= OE | (inexact ? PE : 0);
			flags.up = inexact && up;
			result.exp = unbounded_exp - format.wrap;
			result.m = unbounded;
			return result;
		}
		flags.bits |= OE | PE;
		const bool to_infinity = rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
		flags.up = to_infinity;
		if (to_infinity)
			result.infinite = true;
		else {
			result.exp = format.emax;
			result.m = ~mask;
		}
		return result;
	}
	if (exp >= format.emin || (!(masks & UE) && tiny)) {
		if (tiny && !(masks & UE)) {
			flags.bits |= UE;
			result.exp = unbounded_exp + format.wrap;
		} else
			result.exp = unbounded_exp;
		result.m = unbounded;
		if (inexact) {
			flags.bits |= PE;
			flags.up = up;
		}
		return result;
	}
	// Below the smallest normal: denormalize, then round at the same bit positions.
	const int shift = format.emin - exp;
	u128 work = shift >= 128 ? u128(1) : (m >> shift) | ((m & ((u128(1) << shift) - 1)) != 0 ? 1 : 0);
	const bool lost = (work & mask) != 0;
	int ignored = 0;
	work = at_precision(work, format.emin, ignored, up);
	result.exp = format.emin;
	result.m = work;
	if (lost) {
		flags.bits |= PE | (tiny ? UE : 0);
		flags.up = up;
	}
	return result;
}

F80 pack80(const Rounded &r)
{
	if (r.infinite)
		return infinity(r.sign);
	const uint64_t sig = static_cast<uint64_t>(r.m >> 64);
	if (sig == 0)
		return zero(r.sign);
	if (sig >> 63)
		return make(r.sign, static_cast<uint16_t>((r.exp + bias) & 0x7fff), sig);
	return make(r.sign, 0, sig);
}

uint64_t pack64(const Rounded &r)
{
	const uint64_t sign = uint64_t(r.sign) << 63;
	if (r.infinite)
		return sign | uint64_t(0x7ff) << 52;
	const uint64_t sig = static_cast<uint64_t>(r.m >> 75);
	if (sig >> 52)
		return sign | uint64_t(r.exp + 1023) << 52 | (sig & ((uint64_t(1) << 52) - 1));
	return sign | sig;
}

uint32_t pack32(const Rounded &r)
{
	const uint32_t sign = uint32_t(r.sign) << 31;
	if (r.infinite)
		return sign | uint32_t(0xff) << 23;
	const uint32_t sig = static_cast<uint32_t>(r.m >> 104);
	if (sig >> 23)
		return sign | static_cast<uint32_t>(r.exp + 127) << 23 | (sig & ((uint32_t(1) << 23) - 1));
	return sign | sig;
}

F80 quieted(F80 f)
{
	f.sig |= quiet;
	return f;
}

// The NaN an operation on a and b returns, one of them at least a NaN: the quieted NaN, or of two the one with the larger
// significand (the positive one when they are equal). An SNaN raises IE.
F80 propagate(F80 a, F80 b, Flags &flags)
{
	const Kind ka = kind(a), kb = kind(b);
	if (ka == Kind::snan || kb == Kind::snan)
		flags.bits |= IE;
	if (!is_nan(ka))
		return quieted(b);
	if (!is_nan(kb))
		return quieted(a);
	if (ka == Kind::qnan && kb == Kind::snan)
		return a;
	if (ka == Kind::snan && kb == Kind::qnan)
		return b;
	const F80 qa = quieted(a), qb = quieted(b);
	if (qa.sig != qb.sig)
		return qa.sig > qb.sig ? qa : qb;
	return qa.se <= qb.se ? qa : qb;
}

enum class Op : uint8_t { add, sub, subr, mul, div, divr };

F80 from_double(uint64_t bits, Flags &flags);

// a op b for finite nonzero operands: the exact result, rounded.
F80 arithmetic(Op op, Unpacked a, Unpacked b, Format format, unsigned rc, uint16_t masks, Flags &flags)
{
	switch (op) {
	case Op::add: case Op::sub: {
		const bool b_sign = op == Op::sub ? !b.sign : b.sign;
		Unpacked x = a, y = b;
		y.sign = b_sign;
		if (y.exp > x.exp || (y.exp == x.exp && y.sig > x.sig))
			std::swap(x, y);
		u128 mx = u128(x.sig) << 64, my = u128(y.sig) << 64;
		const int shift = x.exp - y.exp;
		// One guard bit above the significands for a carry: values are m * 2^(exp - 126) here.
		mx >>= 1;
		my >>= 1;
		if (shift >= 127)
			my = my != 0;
		else if (shift > 0)
			my = (my >> shift) | ((my & ((u128(1) << shift) - 1)) != 0 ? 1 : 0);
		u128 sum = x.sign == y.sign ? mx + my : mx - my;
		int exp = x.exp + 1;
		if (sum == 0)
			return zero(rc == 1);
		const int lead = std::countl_zero(static_cast<uint64_t>(sum >> 64)) +
			(static_cast<uint64_t>(sum >> 64) == 0 ? std::countl_zero(static_cast<uint64_t>(sum)) : 0);
		sum <<= lead;
		exp -= lead;
		return pack80(round(x.sign, exp, sum, format, rc, masks, flags));
	}
	case Op::mul: {
		u128 product = u128(a.sig) * b.sig;
		int exp = a.exp + b.exp + 1;
		if (!(product >> 127)) {
			product <<= 1;
			--exp;
		}
		return pack80(round(a.sign != b.sign, exp, product, format, rc, masks, flags));
	}
	default: {
		const Unpacked x = op == Op::div ? a : b, y = op == Op::div ? b : a;
		const u128 high = (u128(x.sig) << 64) / y.sig;
		const uint64_t remainder = static_cast<uint64_t>((u128(x.sig) << 64) % y.sig);
		const uint64_t low = static_cast<uint64_t>((u128(remainder) << 64) / y.sig);
		const bool rest = (u128(remainder) << 64) % y.sig != 0;
		u128 quotient;
		int exp;
		if (high >> 64) {
			quotient = (high << 63) | (low >> 1) | ((low & 1) || rest ? 1 : 0);
			exp = x.exp - y.exp;
		} else {
			quotient = (high << 64) | low | (rest ? 1 : 0);
			exp = x.exp - y.exp - 1;
		}
		return pack80(round(x.sign != y.sign, exp, quotient, format, rc, masks, flags));
	}
	}
}

u128 isqrt(u128 n, u128 &remainder)
{
	u128 result = 0, bit = u128(1) << 126;
	while (bit > n)
		bit >>= 2;
	while (bit) {
		if (n >= result + bit) {
			n -= result + bit;
			result = (result >> 1) + bit;
		} else
			result >>= 1;
		bit >>= 2;
	}
	remainder = n;
	return result;
}

F80 square_root(Unpacked a, Format format, unsigned rc, uint16_t masks, Flags &flags)
{
	const int e2 = a.exp - 63;
	const int k = (e2 & 1) ? 63 : 64;
	u128 remainder = 0;
	const u128 root = isqrt(u128(a.sig) << k, remainder);
	u128 m = root << 64;
	if (remainder > root)
		m |= (u128(1) << 63) | 1;
	else if (remainder)
		m |= 1;
	return pack80(round(false, (e2 - k) / 2 + 63, m, format, rc, masks, flags));
}

// The rounded integer part of a finite value, as a magnitude; saturated beyond 2^127.
struct Integer {
	u128 magnitude = 0;
	bool inexact = false;
	bool up = false;
};

Integer to_integer(Unpacked u, unsigned rc)
{
	Integer result;
	if (u.exp >= 63) {
		result.magnitude = u.exp - 63 >= 64 ? ~u128(0) : u128(u.sig) << (u.exp - 63);
		return result;
	}
	const u128 m = u128(u.sig) << 64;
	const int lsb = 127 - u.exp;
	u128 rest, half;
	bool odd = false;
	if (lsb > 128) {
		rest = 1;
		half = 2;
	} else if (lsb == 128) {
		rest = m;
		half = u128(1) << 127;
	} else {
		result.magnitude = m >> lsb;
		rest = m & ((u128(1) << lsb) - 1);
		half = u128(1) << (lsb - 1);
		odd = result.magnitude & 1;
	}
	result.inexact = rest != 0;
	result.up = round_up(rest, half, odd, u.sign, rc);
	result.magnitude += result.up ? 1 : 0;
	return result;
}

F80 from_magnitude(bool sign, u128 magnitude)
{
	if (magnitude == 0)
		return zero(sign);
	const uint64_t value = static_cast<uint64_t>(magnitude);
	const int lead = std::countl_zero(value);
	return make(sign, static_cast<uint16_t>(63 - lead + bias), value << lead);
}

F80 from_single(uint32_t bits, Flags &flags)
{
	const bool sign = bits >> 31;
	const unsigned exponent = bits >> 23 & 0xff;
	const uint32_t fraction = bits & 0x7fffff;
	if (exponent == 0xff) {
		if (fraction == 0)
			return infinity(sign);
		F80 nan = make(sign, 0x7fff, one | uint64_t(fraction) << 40);
		if (!(fraction & 0x400000))
			flags.bits |= IE;
		return quieted(nan);
	}
	if (exponent == 0) {
		if (fraction == 0)
			return zero(sign);
		flags.bits |= DE;
		const int lead = std::countl_zero(static_cast<uint64_t>(fraction));
		return make(sign, static_cast<uint16_t>(-86 - lead + bias), uint64_t(fraction) << lead);
	}
	return make(sign, static_cast<uint16_t>(static_cast<int>(exponent) - 127 + bias), one | uint64_t(fraction) << 40);
}

F80 from_double(uint64_t bits, Flags &flags)
{
	const bool sign = bits >> 63;
	const unsigned exponent = bits >> 52 & 0x7ff;
	const uint64_t fraction = bits & ((uint64_t(1) << 52) - 1);
	if (exponent == 0x7ff) {
		if (fraction == 0)
			return infinity(sign);
		if (!(fraction >> 51))
			flags.bits |= IE;
		return quieted(make(sign, 0x7fff, one | fraction << 11));
	}
	if (exponent == 0) {
		if (fraction == 0)
			return zero(sign);
		flags.bits |= DE;
		const int lead = std::countl_zero(fraction);
		return make(sign, static_cast<uint16_t>(-1022 - (lead - 11) + bias), fraction << lead);
	}
	return make(sign, static_cast<uint16_t>(static_cast<int>(exponent) - 1023 + bias), one | fraction << 11);
}

// ST(0) to a single or a double. NaNs keep the top of their payload.
uint64_t to_narrow(F80 f, Format format, unsigned rc, uint16_t masks, Flags &flags)
{
	const bool is_double = format.precision == 53;
	const Kind k = kind(f);
	const uint64_t sign = f.sign();
	if (k == Kind::unsupported) {
		flags.bits |= IE;
		return is_double ? 0xfff8000000000000 : 0xffc00000;
	}
	if (is_nan(k)) {
		if (k == Kind::snan)
			flags.bits |= IE;
		const uint64_t sig = f.sig | quiet;
		return is_double ? sign << 63 | uint64_t(0x7ff) << 52 | (sig << 1 >> 12) : sign << 31 | uint64_t(0xff) << 23 | (sig << 1 >> 41);
	}
	if (k == Kind::infinity)
		return is_double ? sign << 63 | uint64_t(0x7ff) << 52 : sign << 31 | uint64_t(0xff) << 23;
	if (k == Kind::zero)
		return is_double ? sign << 63 : sign << 31;
	const Unpacked u = unpack(f);
	const Rounded r = round(u.sign, u.exp, u128(u.sig) << 64, format, rc, masks | OE | UE, flags);
	return is_double ? pack64(r) : pack32(r);
}

// The full tag of a register that is not empty: 0 valid, 1 zero, 2 special.
unsigned full_tag(F80 f)
{
	switch (kind(f)) {
	case Kind::zero: return 1;
	case Kind::normal: return 0;
	default: return 2;
	}
}

// The x87 constants to 128 bits, rounded to 64 as the rounding control says.
F80 constant(const Instruction &instruction, unsigned rc)
{
	struct Value {
		uint16_t se;
		uint64_t high, low;
	};
	Value value{};
	switch (instruction.mnemonic) {
	case Mnemonic::fld1: return make(false, bias, one);
	case Mnemonic::fldz: return zero(false);
	case Mnemonic::fldpi: value = {0x4000, 0xc90fdaa22168c234, 0xc4c6628b00000000}; break;
	case Mnemonic::fldl2t: value = {0x4000, 0xd49a784bcd1b8afe, 0x492bf6ff00000000}; break;
	case Mnemonic::fldl2e: value = {0x3fff, 0xb8aa3b295c17f0bb, 0xbe87fed000000000}; break;
	case Mnemonic::fldlg2: value = {0x3ffd, 0x9a209a84fbcff798, 0x8f8959ac00000000}; break;
	default: value = {0x3ffe, 0xb17217f7d1cf79ab, 0xc9e3b39800000000}; break;
	}
	const bool up = round_up(value.low, uint64_t(1) << 63, value.high & 1, false, rc);
	return {value.high + (up ? 1 : 0), value.se};
}

// What FCW keeps of a loaded value: bits 7 and 13-15 read as zero, bit 6 as one.
uint16_t control_word(uint16_t value)
{
	return static_cast<uint16_t>((value & 0x1f3f) | 0x40);
}

class Core {
public:
	Core(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next)
		: s(state), x(state.x87), memory(memory), in(instruction), next(next)
	{
	}
	X87Result run();

private:
	CpuState &s;
	X87State &x;
	GuestMemory &memory;
	const Instruction &in;
	uint64_t next;

	unsigned top() const { return x.fsw >> 11 & 7; }
	void set_top(unsigned value) { x.fsw = static_cast<uint16_t>((x.fsw & ~0x3800u) | (value & 7) << 11); }
	unsigned physical(unsigned i) const { return (top() + i) & 7; }
	bool empty(unsigned i) const { return !(x.ftw >> physical(i) & 1); }
	F80 st(unsigned i) const
	{
		F80 f;
		std::memcpy(&f.sig, x.st[physical(i)], 8);
		std::memcpy(&f.se, reinterpret_cast<const uint8_t *>(x.st[physical(i)]) + 8, 2);
		return f;
	}
	void set_st(unsigned i, F80 f)
	{
		std::memcpy(x.st[physical(i)], &f.sig, 8);
		std::memcpy(reinterpret_cast<uint8_t *>(x.st[physical(i)]) + 8, &f.se, 2);
		x.ftw = static_cast<uint8_t>(x.ftw | 1u << physical(i));
	}
	void pop()
	{
		x.ftw = static_cast<uint8_t>(x.ftw & ~(1u << top()));
		set_top(top() + 1);
	}
	unsigned rc() const { return x.fcw >> 10 & 3; }
	uint16_t masks() const { return x.fcw & exception_bits; }
	bool aborts(uint16_t bits) const { return (bits & before_result & ~masks()) != 0; }

	// Records the exceptions an instruction raised and sets C1 (and clears C0, C2 and C3 when asked); sets ES and B for any unmasked
	// flag.
	void report(Flags flags, bool c1)
	{
		x.fsw = static_cast<uint16_t>(x.fsw | flags.bits);
		x.fsw = static_cast<uint16_t>(c1 ? x.fsw | C1 : x.fsw & ~C1);
		summary();
	}
	void summary()
	{
		if (x.fsw & exception_bits & ~masks())
			x.fsw = static_cast<uint16_t>(x.fsw | ES | B);
		else
			x.fsw = static_cast<uint16_t>(x.fsw & ~(ES | B));
	}
	void set_conditions(uint16_t bits)
	{
		x.fsw = static_cast<uint16_t>((x.fsw & ~(C0 | C1 | C2 | C3)) | bits);
	}

	uint64_t address(const Operand &operand) const
	{
		uint64_t at = static_cast<uint64_t>(operand.value);
		if (operand.rip_relative)
			at += next;
		else if (operand.base != no_register)
			at += s.gpr[operand.base];
		if (operand.index != no_register)
			at += s.gpr[operand.index] * operand.scale;
		if (in.address_size == 4)
			at = static_cast<uint32_t>(at);
		if (operand.segment == Segment::fs)
			at += s.fs_base;
		else if (operand.segment == Segment::gs)
			at += s.gs_base;
		return at;
	}
	template <typename T>
	T load(const Operand &operand, unsigned offset = 0) const
	{
		T value;
		memory.read(address(operand) + offset, &value, sizeof value);
		return value;
	}
	template <typename T>
	void store(const Operand &operand, T value, unsigned offset = 0)
	{
		memory.write(address(operand) + offset, &value, sizeof value);
	}
	F80 load80(const Operand &operand, unsigned offset = 0) const
	{
		return {load<uint64_t>(operand, offset), load<uint16_t>(operand, offset + 8)};
	}
	void store80(const Operand &operand, F80 f, unsigned offset = 0)
	{
		store(operand, f.sig, offset);
		store(operand, f.se, offset + 8);
	}

	bool waits() const;
	void push(F80 value, Flags flags);
	void load_float();
	void store_float();
	void exchange();
	void binary(Op op, F80 a, F80 b, unsigned destination, bool pops, bool underflow, uint16_t converted = 0);
	void arithmetic_instruction();
	void unary();
	void compare(bool ordered, bool eflags, unsigned pops);
	void examine();
	void conditional_move();
	void load_integer();
	void store_integer();
	void scale();
	void extract();
	void remainder(bool nearest);
	void store_environment(bool save);
	void load_environment(bool restore);
	void initialize();
	F80 compare_operand(Flags &flags, bool &missing);
};

bool Core::waits() const
{
	switch (in.mnemonic) {
	case Mnemonic::fninit: case Mnemonic::fnclex: case Mnemonic::fnstcw: case Mnemonic::fnstsw: case Mnemonic::fnstenv:
	case Mnemonic::fnsave:
		return false;
	default: return true;
	}
}

// Pushes value (flags are what computing it raised). Onto a full register: stack overflow, and the masked response pushes the
// indefinite.
void Core::push(F80 value, Flags flags)
{
	const unsigned slot = (top() + 7) & 7;
	if (x.ftw >> slot & 1) {
		// The overflow is found first: the value's own exceptions are not reported.
		flags.bits = IE | SF;
		if (aborts(flags.bits)) {
			report(flags, true);
			return;
		}
		set_top(slot);
		set_st(0, indefinite);
		report(flags, true);
		return;
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		return;
	}
	set_top(slot);
	set_st(0, value);
	report(flags, false);
}

void Core::load_float()
{
	const Operand &source = in.operands[0];
	Flags flags;
	if (source.kind == OperandKind::st) {
		if (empty(source.reg)) {
			flags.bits = IE | SF;
			if (aborts(flags.bits)) {
				report(flags, false);
				return;
			}
			push(indefinite, flags);
			return;
		}
		push(st(source.reg), flags);
		return;
	}
	F80 value;
	switch (source.size) {
	case 4: value = from_single(load<uint32_t>(source), flags); break;
	case 8: value = from_double(load<uint64_t>(source), flags); break;
	default: value = load80(source); break;
	}
	push(value, flags);
}

void Core::store_float()
{
	const Operand &target = in.operands[0];
	const bool pops = in.mnemonic == Mnemonic::fstp;
	Flags flags;
	F80 value = st(0);
	if (empty(0)) {
		flags.bits = IE | SF;
		if (aborts(flags.bits)) {
			report(flags, false);
			return;
		}
		value = indefinite;
	}
	if (target.kind == OperandKind::st) {
		set_st(target.reg, value);
	} else if (target.size == 10) {
		store80(target, value);
	} else {
		const Format format = target.size == 8 ? double_format : single_format;
		Flags converted = flags;
		const uint64_t bits = to_narrow(value, format, rc(), masks(), converted);
		if ((converted.bits & ~masks() & (IE | DE | OE | UE)) != 0) {
			report(converted, converted.up);
			return;
		}
		flags = converted;
		if (target.size == 8)
			store(target, bits);
		else
			store(target, static_cast<uint32_t>(bits));
	}
	if (pops)
		pop();
	report(flags, flags.up);
}

void Core::exchange()
{
	const unsigned i = in.operands[0].reg;
	Flags flags;
	F80 a = st(0), b = st(i);
	if (empty(0) || empty(i)) {
		flags.bits = IE | SF;
		if (aborts(flags.bits)) {
			report(flags, false);
			return;
		}
		if (empty(0))
			a = indefinite;
		if (empty(i))
			b = indefinite;
	}
	set_st(0, b);
	set_st(i, a);
	report(flags, false);
}

// a op b into ST(destination); underflow: an operand register was empty.
void Core::binary(Op op, F80 a, F80 b, unsigned destination, bool pops, bool underflow, uint16_t converted)
{
	Flags flags;
	F80 result;
	const Kind ka = kind(a), kb = kind(b);
	// A denormal operand counts only when the operation computes with finite operands: not beside a NaN, an unsupported value or an
	// empty register, nor when it ends in an invalid operation or a division by zero.
	bool denormal = ka == Kind::denormal || kb == Kind::denormal || (converted & DE);
	if (underflow) {
		flags.bits = IE | SF;
		result = indefinite;
	} else if (ka == Kind::unsupported || kb == Kind::unsupported) {
		flags.bits = IE;
		result = indefinite;
	} else if (is_nan(ka) || is_nan(kb)) {
		result = propagate(a, b, flags);
	} else {
		const bool sa = a.sign(), sb = b.sign();
		switch (op) {
		case Op::add: case Op::sub: case Op::subr: {
			const F80 p = op == Op::subr ? b : a, q = op == Op::subr ? a : b;
			const bool sy = op == Op::add ? q.sign() : !q.sign();
			const Kind kx = kind(p), ky = kind(q);
			if (kx == Kind::infinity && ky == Kind::infinity && p.sign() != sy) {
				flags.bits |= IE;
				result = indefinite;
			} else if (kx == Kind::infinity)
				result = p;
			else if (ky == Kind::infinity)
				result = infinity(sy);
			else if (kx == Kind::zero && ky == Kind::zero)
				result = zero(p.sign() == sy ? p.sign() : rc() == 1);
			else if (kx == Kind::zero || ky == Kind::zero) {
				const F80 other = kx == Kind::zero ? q : p;
				const Unpacked u = unpack(other);
				const bool sign = kx == Kind::zero ? sy : p.sign();
				result = pack80(round(sign, u.exp, u128(u.sig) << 64, precision_control(x.fcw), rc(), masks(), flags));
			} else {
				Unpacked ux = unpack(p), uy = unpack(q);
				uy.sign = sy;
				result = x87_double_arithmetic ? F80{} : arithmetic(Op::add, ux, uy, precision_control(x.fcw), rc(), masks(), flags);
				if (x87_double_arithmetic) {
					Flags ignored;
					const double dx = std::ldexp(static_cast<double>(ux.sig), ux.exp - 63) * (ux.sign ? -1 : 1);
					const double dy = std::ldexp(static_cast<double>(uy.sig), uy.exp - 63) * (uy.sign ? -1 : 1);
					const double sum = dx + dy;
					uint64_t bits;
					std::memcpy(&bits, &sum, 8);
					result = from_double(bits, ignored);
				}
			}
			break;
		}
		case Op::mul:
			if ((ka == Kind::infinity && kb == Kind::zero) || (ka == Kind::zero && kb == Kind::infinity)) {
				flags.bits |= IE;
				result = indefinite;
			} else if (ka == Kind::infinity || kb == Kind::infinity)
				result = infinity(sa != sb);
			else if (ka == Kind::zero || kb == Kind::zero)
				result = zero(sa != sb);
			else if (x87_double_arithmetic) {
				const Unpacked ua = unpack(a), ub = unpack(b);
				const double product = std::ldexp(static_cast<double>(ua.sig), ua.exp - 63) * std::ldexp(static_cast<double>(ub.sig),
					ub.exp - 63) * (sa != sb ? -1 : 1);
				uint64_t bits;
				std::memcpy(&bits, &product, 8);
				Flags ignored;
				result = from_double(bits, ignored);
			} else
				result = arithmetic(Op::mul, unpack(a), unpack(b), precision_control(x.fcw), rc(), masks(), flags);
			break;
		default: {
			const F80 n = op == Op::div ? a : b, d = op == Op::div ? b : a;
			const Kind kn = kind(n), kd = kind(d);
			if ((kn == Kind::zero && kd == Kind::zero) || (kn == Kind::infinity && kd == Kind::infinity)) {
				flags.bits |= IE;
				result = indefinite;
			} else if (kn == Kind::infinity)
				result = infinity(sa != sb);
			else if (kd == Kind::infinity || kn == Kind::zero)
				result = zero(sa != sb);
			else if (kd == Kind::zero) {
				flags.bits |= ZE;
				result = infinity(sa != sb);
			} else if (x87_double_arithmetic) {
				const Unpacked un = unpack(n), ud = unpack(d);
				const double quotient = std::ldexp(static_cast<double>(un.sig), un.exp - 63) / std::ldexp(static_cast<double>(ud.sig),
					ud.exp - 63) * (sa != sb ? -1 : 1);
				uint64_t bits;
				std::memcpy(&bits, &quotient, 8);
				Flags ignored;
				result = from_double(bits, ignored);
			} else
				result = arithmetic(Op::div, unpack(n), unpack(d), precision_control(x.fcw), rc(), masks(), flags);
			break;
		}
		}
	}
	if (underflow || ka == Kind::unsupported || kb == Kind::unsupported || is_nan(ka) || is_nan(kb) || (flags.bits & (IE | ZE)))
		denormal = false;
	if (denormal && !(masks() & DE)) {
		// Unmasked, it stops the instruction before any result.
		report({DE, false}, false);
		return;
	}
	if (denormal)
		flags.bits |= DE;
	if (aborts(flags.bits)) {
		report({static_cast<uint16_t>(flags.bits & (before_result | SF)), false}, false);
		return;
	}
	set_st(destination, result);
	if (pops)
		pop();
	report(flags, flags.up);
}

void Core::arithmetic_instruction()
{
	Op op;
	bool pops = false;
	switch (in.mnemonic) {
	case Mnemonic::fadd: case Mnemonic::fiadd: op = Op::add; break;
	case Mnemonic::faddp: op = Op::add; pops = true; break;
	case Mnemonic::fmul: case Mnemonic::fimul: op = Op::mul; break;
	case Mnemonic::fmulp: op = Op::mul; pops = true; break;
	case Mnemonic::fsub: case Mnemonic::fisub: op = Op::sub; break;
	case Mnemonic::fsubp: op = Op::sub; pops = true; break;
	case Mnemonic::fsubr: case Mnemonic::fisubr: op = Op::subr; break;
	case Mnemonic::fsubrp: op = Op::subr; pops = true; break;
	case Mnemonic::fdiv: case Mnemonic::fidiv: op = Op::div; break;
	case Mnemonic::fdivp: op = Op::div; pops = true; break;
	case Mnemonic::fdivr: case Mnemonic::fidivr: op = Op::divr; break;
	default: op = Op::divr; pops = true; break;
	}
	const Operand &first = in.operands[0];
	if (in.count == 1 && first.kind == OperandKind::mem) {
		// ST(0) op memory.
		Flags flags;
		F80 b;
		const bool integer = in.mnemonic >= Mnemonic::fiadd && in.mnemonic <= Mnemonic::fidivr;
		if (integer)
			b = first.size == 2 ? from_magnitude(load<int16_t>(first) < 0, static_cast<u128>(std::abs(int(load<int16_t>(first)))))
				: from_magnitude(load<int32_t>(first) < 0, static_cast<u128>(std::abs(static_cast<int64_t>(load<int32_t>(first)))));
		else
			b = first.size == 4 ? from_single(load<uint32_t>(first), flags) : from_double(load<uint64_t>(first), flags);
		// The conversion's SNaN stays one for the operation, which reports its IE; the conversion's DE is the operation's.
		if ((flags.bits & IE) && is_nan(kind(b)))
			b.sig &= ~quiet;
		binary(op, st(0), b, 0, false, empty(0), flags.bits);
		return;
	}
	const unsigned a = first.kind == OperandKind::st && first.reg != no_register ? first.reg : 0;
	const unsigned b = in.operands[1].kind == OperandKind::st && in.operands[1].reg != no_register ? in.operands[1].reg : 0;
	// st(dest) = st(dest) op st(other).
	binary(op, st(a), st(b), a, pops, empty(a) || empty(b));
}

void Core::unary()
{
	Flags flags;
	if (empty(0)) {
		flags.bits = IE | SF;
		if (aborts(flags.bits)) {
			report(flags, false);
			return;
		}
		set_st(0, indefinite);
		report(flags, false);
		return;
	}
	F80 a = st(0), result = a;
	const Kind k = kind(a);
	switch (in.mnemonic) {
	case Mnemonic::fabs: result.se &= 0x7fff; break;
	case Mnemonic::fchs: result.se ^= 0x8000; break;
	case Mnemonic::fsqrt:
		if (k == Kind::unsupported) {
			flags.bits = IE;
			result = indefinite;
		} else if (is_nan(k))
			result = propagate(a, a, flags);
		else if (k == Kind::zero)
			result = a;
		else if (a.sign()) {
			flags.bits = IE;
			result = indefinite;
		} else if (k == Kind::infinity)
			result = a;
		else {
			if (k == Kind::denormal)
				flags.bits |= DE;
			if (x87_double_arithmetic) {
				const Unpacked u = unpack(a);
				const double root = std::sqrt(std::ldexp(static_cast<double>(u.sig), u.exp - 63));
				uint64_t bits;
				std::memcpy(&bits, &root, 8);
				Flags ignored;
				result = from_double(bits, ignored);
			} else
				result = square_root(unpack(a), precision_control(x.fcw), rc(), masks(), flags);
		}
		break;
	default:
		// frndint
		if (k == Kind::unsupported) {
			flags.bits = IE;
			result = indefinite;
		} else if (is_nan(k))
			result = propagate(a, a, flags);
		else if (finite_nonzero(k)) {
			if (k == Kind::denormal)
				flags.bits |= DE;
			const Unpacked u = unpack(a);
			if (u.exp < 63) {
				const Integer integer = to_integer(u, rc());
				result = from_magnitude(u.sign, integer.magnitude);
				if (integer.inexact) {
					flags.bits |= PE;
					flags.up = integer.up;
				}
			}
		}
		break;
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		return;
	}
	set_st(0, result);
	report(flags, flags.up);
}

// The second operand of a compare: ST(i), memory, an integer or zero (ftst).
F80 Core::compare_operand(Flags &flags, bool &missing)
{
	missing = false;
	if (in.mnemonic == Mnemonic::ftst)
		return zero(false);
	if (in.mnemonic == Mnemonic::fcompp || in.mnemonic == Mnemonic::fucompp) {
		missing = empty(1);
		return st(1);
	}
	const Operand &operand = in.count == 2 ? in.operands[1] : in.operands[0];
	if (operand.kind == OperandKind::st) {
		missing = empty(operand.reg);
		return st(operand.reg);
	}
	if (in.mnemonic == Mnemonic::ficom || in.mnemonic == Mnemonic::ficomp) {
		const int64_t value = operand.size == 2 ? load<int16_t>(operand) : load<int32_t>(operand);
		return from_magnitude(value < 0, static_cast<u128>(value < 0 ? -value : value));
	}
	Flags conversion;
	F80 value = operand.size == 4 ? from_single(load<uint32_t>(operand), conversion) : from_double(load<uint64_t>(operand), conversion);
	// Keep the SNaN an SNaN for the compare's own check; DE is the compare's too.
	if ((conversion.bits & IE))
		value.sig &= ~quiet;
	flags.bits |= conversion.bits & DE;
	return value;
}

void Core::compare(bool ordered, bool eflags, unsigned pops)
{
	Flags flags;
	bool missing = false;
	const F80 b = compare_operand(flags, missing);
	const F80 a = st(0);
	missing |= empty(0);
	// C3 C2 C0 (ZF PF CF): 000 greater, 001 less, 100 equal, 111 unordered.
	bool less = false, equal = false, unordered = false;
	const Kind ka = kind(a), kb = kind(b);
	if (missing) {
		flags.bits = IE | SF;
		unordered = true;
	} else if (ka == Kind::unsupported || kb == Kind::unsupported) {
		flags.bits |= IE;
		unordered = true;
	} else if (is_nan(ka) || is_nan(kb)) {
		if (ordered || ka == Kind::snan || kb == Kind::snan)
			flags.bits |= IE;
		unordered = true;
	} else {
		if (ka == Kind::denormal || kb == Kind::denormal)
			flags.bits |= DE;
		if (ka == Kind::zero && kb == Kind::zero)
			equal = true;
		else if (ka == Kind::infinity || kb == Kind::infinity || ka == Kind::zero || kb == Kind::zero || a.sign() != b.sign()) {
			auto rank = [](F80 f) {
				const Kind k = kind(f);
				int r = k == Kind::zero ? 0 : k == Kind::infinity ? 2 : 1;
				return f.sign() ? -r : r;
			};
			const int ra = rank(a), rb = rank(b);
			if (ra != rb || (ra == 1 || ra == -1)) {
				if (ra == rb) {
					const Unpacked ua = unpack(a), ub = unpack(b);
					const bool magnitude_less = ua.exp != ub.exp ? ua.exp < ub.exp : ua.sig < ub.sig;
					equal = ua.exp == ub.exp && ua.sig == ub.sig;
					less = !equal && (magnitude_less != a.sign());
				} else
					less = ra < rb;
			} else
				equal = true;
		} else {
			const Unpacked ua = unpack(a), ub = unpack(b);
			equal = ua.exp == ub.exp && ua.sig == ub.sig;
			const bool magnitude_less = ua.exp != ub.exp ? ua.exp < ub.exp : ua.sig < ub.sig;
			less = !equal && (magnitude_less != a.sign());
		}
	}
	// An unmasked invalid operation still sets the condition codes (unordered); only the pops are skipped.
	const bool stopped = aborts(flags.bits);
	if (eflags) {
		const bool zf = equal || unordered, pf = unordered, cf = less || unordered;
		s.nzcv = (zf ? 1ull << 30 : 0) | (cf ? 0 : 1ull << 29);
		s.parity_source = pf ? 0 : 1;
		s.adjust_source = s.parity_source;
	} else
		set_conditions(static_cast<uint16_t>((equal || unordered ? C3 : 0) | (unordered ? C2 : 0) | (less || unordered ? C0 : 0)));
	for (unsigned i = 0; i < pops && !stopped; ++i)
		pop();
	// fcomi and fucomi leave C1 as it was unless the stack faulted.
	report(flags, eflags && !missing ? (x.fsw & C1) != 0 : false);
}

void Core::examine()
{
	const F80 a = st(0);
	uint16_t codes;
	if (empty(0))
		codes = C3 | C0;
	else
		switch (kind(a)) {
		case Kind::unsupported: codes = 0; break;
		case Kind::qnan: case Kind::snan: codes = C0; break;
		case Kind::normal: codes = C2; break;
		case Kind::infinity: codes = C2 | C0; break;
		case Kind::zero: codes = C3; break;
		default: codes = C3 | C2; break;
		}
	set_conditions(static_cast<uint16_t>(codes | (a.sign() ? C1 : 0)));
}

void Core::conditional_move()
{
	const bool cf = !(s.nzcv >> 29 & 1), zf = s.nzcv >> 30 & 1;
	const bool pf = std::popcount(static_cast<unsigned>(s.parity_source & 0xff)) % 2 == 0;
	bool condition;
	switch (in.mnemonic) {
	case Mnemonic::fcmovb: condition = cf; break;
	case Mnemonic::fcmove: condition = zf; break;
	case Mnemonic::fcmovbe: condition = cf || zf; break;
	case Mnemonic::fcmovu: condition = pf; break;
	case Mnemonic::fcmovnb: condition = !cf; break;
	case Mnemonic::fcmovne: condition = !zf; break;
	case Mnemonic::fcmovnbe: condition = !cf && !zf; break;
	default: condition = !pf; break;
	}
	const unsigned i = in.operands[1].reg;
	Flags flags;
	if (empty(0) || empty(i)) {
		// Whatever the condition, the masked response loads the indefinite.
		flags.bits = IE | SF;
		if (!aborts(flags.bits))
			set_st(0, indefinite);
		report(flags, false);
		return;
	}
	// C1 and the flags stay as they were.
	if (condition)
		set_st(0, st(i));
}

void Core::load_integer()
{
	const Operand &source = in.operands[0];
	int64_t value;
	switch (source.size) {
	case 2: value = load<int16_t>(source); break;
	case 4: value = load<int32_t>(source); break;
	default: value = load<int64_t>(source); break;
	}
	const u128 magnitude = value < 0 ? u128(0) - static_cast<u128>(static_cast<__int128>(value)) : static_cast<u128>(value);
	push(from_magnitude(value < 0, magnitude), {});
}

void Core::store_integer()
{
	const Operand &target = in.operands[0];
	const bool pops = in.mnemonic != Mnemonic::fist;
	const unsigned mode = in.mnemonic == Mnemonic::fisttp ? 3 : rc();
	const unsigned bits = target.size * 8u;
	const u128 limit = u128(1) << (bits - 1);
	Flags flags;
	int64_t result = 0;
	bool invalid = false;
	if (empty(0)) {
		flags.bits = IE | SF;
		invalid = true;
	} else {
		const F80 a = st(0);
		const Kind k = kind(a);
		if (k == Kind::zero)
			result = 0;
		else if (!finite_nonzero(k))
			invalid = true;
		else {
			// No DE: a conversion to an integer does not report a denormal operand.
			const Unpacked u = unpack(a);
			const Integer integer = to_integer(u, mode);
			if (integer.magnitude > limit || (integer.magnitude == limit && !u.sign))
				invalid = true;
			else {
				result = u.sign ? static_cast<int64_t>(u128(0) - integer.magnitude) : static_cast<int64_t>(integer.magnitude);
				if (integer.inexact) {
					flags.bits |= PE;
					flags.up = integer.up;
				}
			}
		}
		if (invalid)
			flags.bits = static_cast<uint16_t>((flags.bits & ~(PE)) | IE);
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		return;
	}
	if (invalid) {
		result = bits == 64 ? INT64_MIN : -static_cast<int64_t>(limit);
		flags.up = false;
	}
	switch (target.size) {
	case 2: store(target, static_cast<int16_t>(result)); break;
	case 4: store(target, static_cast<int32_t>(result)); break;
	default: store(target, result); break;
	}
	if (pops)
		pop();
	report(flags, flags.up);
}

void Core::scale()
{
	Flags flags;
	F80 result;
	if (empty(0) || empty(1)) {
		flags.bits = IE | SF;
		result = indefinite;
	} else {
		const F80 a = st(0), b = st(1);
		const Kind ka = kind(a), kb = kind(b);
		if (ka == Kind::unsupported || kb == Kind::unsupported) {
			flags.bits = IE;
			result = indefinite;
		} else if (is_nan(ka) || is_nan(kb))
			result = propagate(a, b, flags);
		else {
			if (ka == Kind::denormal || kb == Kind::denormal)
				flags.bits |= DE;
			if (kb == Kind::infinity) {
				if ((ka == Kind::zero && !b.sign()) || (ka == Kind::infinity && b.sign())) {
					flags.bits |= IE;
					result = indefinite;
				} else if (ka == Kind::zero || ka == Kind::infinity)
					result = a;
				else
					result = b.sign() ? zero(a.sign()) : infinity(a.sign());
			} else if (ka == Kind::zero || ka == Kind::infinity)
				result = a;
			else {
				int64_t n = 0;
				if (kb != Kind::zero) {
					const Unpacked ub = unpack(b);
					const Integer integer = to_integer(ub, 3);
					const int64_t magnitude = integer.magnitude > (u128(1) << 20) ? int64_t(1) << 20 : static_cast<int64_t>(integer.magnitude);
					n = ub.sign ? -magnitude : magnitude;
				}
				const Unpacked ua = unpack(a);
				result = pack80(round(ua.sign, ua.exp + static_cast<int>(n), u128(ua.sig) << 64, extended, rc(), masks(), flags));
			}
		}
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		return;
	}
	set_st(0, result);
	report(flags, flags.up);
}

void Core::extract()
{
	Flags flags;
	if (empty(0)) {
		flags.bits = IE | SF;
		if (aborts(flags.bits)) {
			report(flags, false);
			return;
		}
		set_st(0, indefinite);
		push(indefinite, {});
		x.fsw = static_cast<uint16_t>(x.fsw | IE | SF);
		summary();
		return;
	}
	if (x.ftw >> ((top() + 7) & 7) & 1) {
		flags.bits = IE | SF;
		if (!aborts(flags.bits)) {
			set_st(0, indefinite);
			set_top(top() + 7);
			set_st(0, indefinite);
		}
		report(flags, true);
		return;
	}
	const F80 a = st(0);
	const Kind k = kind(a);
	F80 exponent, significand;
	if (k == Kind::unsupported) {
		flags.bits = IE;
		exponent = significand = indefinite;
	} else if (is_nan(k)) {
		exponent = significand = propagate(a, a, flags);
	} else if (k == Kind::infinity) {
		exponent = infinity(false);
		significand = a;
	} else if (k == Kind::zero) {
		flags.bits = ZE;
		exponent = infinity(true);
		significand = a;
	} else {
		if (k == Kind::denormal)
			flags.bits |= DE;
		const Unpacked u = unpack(a);
		exponent = from_magnitude(u.exp < 0, static_cast<u128>(u.exp < 0 ? -u.exp : u.exp));
		significand = make(u.sign, bias, u.sig);
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		return;
	}
	set_st(0, exponent);
	set_top(top() + 7);
	set_st(0, significand);
	report(flags, false);
}

// fprem and fprem1: exact. With the exponents 64 or more apart only a partial reduction is done (C2 set), by a multiple of b times a
// power of two that leaves the exponent difference between 32 and 63.
void Core::remainder(bool nearest)
{
	Flags flags;
	F80 result;
	// Without a quotient (a fault, an invalid operand, a NaN) C0 and C3 stay as they were; C1 and C2 are cleared.
	const uint16_t unchanged = static_cast<uint16_t>(x.fsw & (C0 | C3));
	uint16_t codes = unchanged;
	if (empty(0) || empty(1)) {
		flags.bits = IE | SF;
		result = indefinite;
	} else {
		const F80 a = st(0), b = st(1);
		const Kind ka = kind(a), kb = kind(b);
		if (ka == Kind::unsupported || kb == Kind::unsupported) {
			flags.bits = IE;
			result = indefinite;
		} else if (is_nan(ka) || is_nan(kb))
			result = propagate(a, b, flags);
		else if (ka == Kind::infinity || kb == Kind::zero) {
			flags.bits = static_cast<uint16_t>(IE | ((ka == Kind::denormal || kb == Kind::denormal) ? DE : 0));
			result = indefinite;
		} else if (ka == Kind::zero || kb == Kind::infinity) {
			// The quotient is 0; a pseudo-denormal comes back normalized.
			if (ka == Kind::denormal)
				flags.bits |= DE;
			result = ka == Kind::zero ? a : pack80(round(a.sign(), unpack(a).exp, u128(unpack(a).sig) << 64, extended, rc(), masks(), flags));
			codes = 0;
		} else {
			if (ka == Kind::denormal || kb == Kind::denormal)
				flags.bits |= DE;
			const Unpacked ua = unpack(a), ub = unpack(b);
			const int d = ua.exp - ub.exp;
			auto pack_remainder = [&](bool sign, u128 r, int exp) {
				// r * 2^exp.
				if (r == 0)
					return zero(ua.sign);
				const int length = 128 - (static_cast<uint64_t>(r >> 64) ? std::countl_zero(static_cast<uint64_t>(r >> 64))
					: 64 + std::countl_zero(static_cast<uint64_t>(r)));
				return pack80(round(sign, exp + length - 1, r << (128 - length), extended, rc(), masks(), flags));
			};
			if (d >= 64) {
				const int n = 32 + d % 32;
				const u128 numerator = u128(ua.sig) << n;
				const u128 r = numerator % ub.sig;
				// a - q * b * 2^(d - n): units of 2^(eb - 63 + d - n).
				result = pack_remainder(ua.sign, r, ub.exp - 63 + d - n);
				codes = C2;
			} else if (d < -1 || (d == -1 && !nearest)) {
				result = pack80(round(ua.sign, ua.exp, u128(ua.sig) << 64, extended, rc(), masks(), flags));
				codes = 0;
			} else {
				// Units of 2^(eb - 64): a is sig_a * 2^(d + 1), b is 2 * sig_b.
				const u128 numerator = u128(ua.sig) << (d + 1), divisor = u128(ub.sig) << 1;
				u128 q = numerator / divisor, r = numerator % divisor;
				bool sign = ua.sign;
				if (nearest && (r << 1 > divisor || (r << 1 == divisor && (q & 1)))) {
					r = divisor - r;
					sign = !sign;
					++q;
				}
				result = pack_remainder(sign, r, ub.exp - 64);
				codes = static_cast<uint16_t>(((q >> 2 & 1) ? C0 : 0) | ((q >> 1 & 1) ? C3 : 0) | ((q & 1) ? C1 : 0));
			}
		}
	}
	if (aborts(flags.bits)) {
		report(flags, false);
		set_conditions(unchanged);
		return;
	}
	set_st(0, result);
	set_conditions(codes);
	x.fsw = static_cast<uint16_t>(x.fsw | flags.bits);
	summary();
}

void Core::initialize()
{
	x.fcw = 0x37f;
	x.fsw = 0;
	x.ftw = 0;
	x.fop = 0;
	x.fip = 0;
	x.fdp = 0;
}

// fnstenv and fnsave in the 32-bit protected-mode layout 64-bit mode uses without REX.W: FCW, FSW, the full tag word, each padded to
// 32 bits with ones, then the pointers (left zero), then for fnsave the eight registers from ST(0).
void Core::store_environment(bool save)
{
	const Operand &target = in.operands[0];
	uint16_t tags = 0;
	for (unsigned r = 0; r < 8; ++r) {
		F80 f;
		std::memcpy(&f.sig, x.st[r], 8);
		std::memcpy(&f.se, reinterpret_cast<const uint8_t *>(x.st[r]) + 8, 2);
		const unsigned tag = (x.ftw >> r & 1) ? full_tag(f) : 3;
		tags = static_cast<uint16_t>(tags | tag << 2 * r);
	}
	const uint32_t words[7] = {0xffff0000u | x.fcw, 0xffff0000u | x.fsw, 0xffff0000u | tags, 0, 0, 0, 0xffff0000u};
	for (unsigned i = 0; i < 7; ++i)
		store(target, words[i], 4 * i);
	if (save) {
		for (unsigned i = 0; i < 8; ++i)
			store80(target, st(i), 28 + 10 * i);
		initialize();
		return;
	}
	x.fcw = static_cast<uint16_t>(x.fcw | exception_bits);
	summary();
}

void Core::load_environment(bool restore)
{
	const Operand &source = in.operands[0];
	x.fcw = control_word(load<uint16_t>(source, 0));
	x.fsw = load<uint16_t>(source, 4);
	const uint16_t tags = load<uint16_t>(source, 8);
	x.ftw = 0;
	for (unsigned r = 0; r < 8; ++r)
		if ((tags >> 2 * r & 3) != 3)
			x.ftw = static_cast<uint8_t>(x.ftw | 1u << r);
	if (restore) {
		const uint8_t tags_now = x.ftw;
		for (unsigned i = 0; i < 8; ++i)
			set_st(i, load80(source, 28 + 10 * i));
		x.ftw = tags_now;
	}
}

X87Result Core::run()
{
	if (waits() && (x.fsw & ES))
		return X87Result::fault;
	using enum Mnemonic;
	switch (in.mnemonic) {
	case fld: load_float(); break;
	case fst: case fstp: store_float(); break;
	case fxch: exchange(); break;
	case fld1: case fldz: case fldpi: case fldl2e: case fldl2t: case fldlg2: case fldln2: this->push(constant(in, rc()), {}); break;
	case ffree: case ffreep:
		x.ftw = static_cast<uint8_t>(x.ftw & ~(1u << physical(in.operands[0].reg)));
		if (in.mnemonic == ffreep)
			this->pop();
		x.fsw = static_cast<uint16_t>(x.fsw & ~C1);
		break;
	case fincstp: set_top(top() + 1); x.fsw = static_cast<uint16_t>(x.fsw & ~C1); break;
	case fdecstp: set_top(top() + 7); x.fsw = static_cast<uint16_t>(x.fsw & ~C1); break;
	case fnop: case fwait: break;
	case fldcw: x.fcw = control_word(load<uint16_t>(in.operands[0])); summary(); break;
	case fnstcw: store(in.operands[0], x.fcw); break;
	case fnstsw:
		if (in.operands[0].kind == OperandKind::reg)
			s.gpr[rax] = (s.gpr[rax] & ~uint64_t(0xffff)) | x.fsw;
		else
			store(in.operands[0], x.fsw);
		break;
	case fnclex: x.fsw = static_cast<uint16_t>(x.fsw & ~(exception_bits | SF | ES | B)); break;
	case fninit: initialize(); break;
	case fnstenv: store_environment(false); break;
	case fnsave: store_environment(true); break;
	case fldenv: load_environment(false); break;
	case frstor: load_environment(true); break;
	case fadd: case faddp: case fiadd: case fmul: case fmulp: case fimul: case fsub: case fsubp: case fisub: case fsubr: case fsubrp:
	case fisubr: case fdiv: case fdivp: case fidiv: case fdivr: case fdivrp: case fidivr:
		arithmetic_instruction();
		break;
	case fabs: case fchs: case fsqrt: case frndint: unary(); break;
	case fcom: compare(true, false, 0); break;
	case fcomp: compare(true, false, 1); break;
	case fcompp: compare(true, false, 2); break;
	case fucom: compare(false, false, 0); break;
	case fucomp: compare(false, false, 1); break;
	case fucompp: compare(false, false, 2); break;
	case ficom: compare(true, false, 0); break;
	case ficomp: compare(true, false, 1); break;
	case ftst: compare(true, false, 0); break;
	case fcomi: compare(true, true, 0); break;
	case fcompi: compare(true, true, 1); break;
	case fucomi: compare(false, true, 0); break;
	case fucompi: compare(false, true, 1); break;
	case fxam: examine(); break;
	case fcmovb: case fcmove: case fcmovbe: case fcmovu: case fcmovnb: case fcmovne: case fcmovnbe: case fcmovnu: conditional_move(); break;
	case fild: load_integer(); break;
	case fist: case fistp: case fisttp: store_integer(); break;
	case fscale: scale(); break;
	case fxtract: extract(); break;
	case fprem: remainder(false); break;
	case fprem1: remainder(true); break;
	default: break;
	}
	return X87Result::done;
}

}

bool x87_supported(const Instruction &instruction)
{
	using enum Mnemonic;
	switch (instruction.mnemonic) {
	case fbld: case fbstp: case fsin: case fcos: case fsincos: case fptan: case fpatan: case f2xm1: case fyl2x: case fyl2xp1:
		return false;
	default: return instruction.mnemonic >= x87_first && instruction.mnemonic <= x87_last;
	}
}

X87Result x87_execute(CpuState &state, GuestMemory &memory, const Instruction &instruction, uint64_t next)
{
	return Core(state, memory, instruction, next).run();
}

}
