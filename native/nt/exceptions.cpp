#include "exceptions.h"

#include <csignal>
#include <stdexcept>

namespace raven {

NativeResult native_raise_exception(NativeProcess &process)
{
	const auto arguments = process.api_arguments(4);
	NativeResult result{NativeStop::exception, static_cast<uint32_t>(arguments[0]),
		process.context().registers.rip - 1, 0};
	result.exception_flags = static_cast<uint32_t>(arguments[1]);
	if (arguments[3]) {
		const auto count = static_cast<uint32_t>(arguments[2]);
		if (count > 15)
			throw std::runtime_error("RaiseException exceeds the Windows parameter limit");
		if (!count)
			return result;
		const auto bytes = process.read_memory(arguments[3], size_t(count) * 8);
		result.exception_information.resize(count);
		for (size_t index = 0; index < bytes.size(); ++index)
			result.exception_information[index / 8] |= uint64_t(bytes[index]) << ((index % 8) * 8);
	}
	return result;
}

NativeResult native_raise_rtl_exception(NativeProcess &process, bool status_only)
{
	const auto arguments=process.api_arguments(1);
	NativeResult result{NativeStop::exception,uint32_t(arguments[0]),process.context().registers.rip-1,0};
	if (status_only) { result.exception_flags=1; return result; }
	const auto bytes=process.read_memory(arguments[0],152);
	const auto field=[&](size_t offset,size_t size) {
		uint64_t value=0;
		for (size_t byte=0; byte<size; ++byte) value|=uint64_t(bytes[offset+byte])<<(byte*8);
		return value;
	};
	result.value=field(0,4);
	result.exception_flags=uint32_t(field(4,4));
	result.instruction=field(16,8);
	const auto count=field(24,4);
	if (count>15) throw std::runtime_error("RtlRaiseException exceeds the Windows parameter limit");
	for (size_t index=0; index<count; ++index) result.exception_information.push_back(field(32+index*8,8));
	return result;
}

namespace {

// The instructions user mode may not run, as the processor reports them with a general protection fault: port input and output, the
// interrupt flag, hlt, invd, wbinvd, the control and debug register moves and the model-specific registers. Prefixes come first.
bool privileged_instruction(const std::vector<uint8_t> &code)
{
	size_t at = 0;
	while (at < code.size() && (code[at] == 0x26 || code[at] == 0x2e || code[at] == 0x36 || code[at] == 0x3e || code[at] == 0x64 ||
		code[at] == 0x65 || code[at] == 0x66 || code[at] == 0x67 || code[at] == 0xf2 || code[at] == 0xf3 || (code[at] & 0xf0) == 0x40))
		++at;
	if (at >= code.size())
		return false;
	const uint8_t first = code[at];
	if ((first >= 0x6c && first <= 0x6f) || (first >= 0xe4 && first <= 0xe7) || (first >= 0xec && first <= 0xef) || first == 0xf4 ||
		first == 0xfa || first == 0xfb)
		return true;
	if (first != 0x0f || at + 1 >= code.size())
		return false;
	const uint8_t second = code[at + 1];
	return second == 0x08 || second == 0x09 || second == 0x30 || second == 0x32 || (second >= 0x20 && second <= 0x23);
}

}

ProcessorException processor_exception(NativeProcess &process, const NativeResult &fault, uint64_t ip)
{
	ProcessorException exception;
	exception.address = ip;
	switch (fault.signal) {
	case SIGSEGV:
		if (fault.signal_code == SI_KERNEL) {
			std::vector<uint8_t> code;
			try {
				code = process.read_memory(ip, 16);
			} catch (const std::runtime_error &) {
			}
			if (privileged_instruction(code)) {
				exception.status = 0xc0000096U;
				break;
			}
		}
		exception.parameters = {0, fault.fault_address};
		break;
	case SIGBUS:
		exception.status = 0xc0000006U;
		exception.parameters = {0, fault.fault_address, 0xc0000006U};
		break;
	case SIGFPE:
		exception.status = fault.signal_code == FPE_INTDIV ? 0xc0000094U : fault.signal_code == FPE_INTOVF ? 0xc0000095U :
			fault.signal_code == FPE_FLTDIV ? 0xc000008eU : fault.signal_code == FPE_FLTOVF ? 0xc0000091U : 0xc000008fU;
		break;
	case SIGILL:
		exception.status = 0xc000001dU;
		break;
	case SIGTRAP:
		// A single step and a hardware breakpoint are STATUS_SINGLE_STEP on the instruction about to run.
		if (fault.signal_code == TRAP_TRACE || fault.signal_code == TRAP_HWBKPT) {
			exception.status = 0x80000004U;
			break;
		}
		// The processor leaves the instruction pointer after an int3; Windows moves it back onto the breakpoint.
		exception.status = 0x80000003U;
		exception.parameters = {0};
		try {
			if (ip && process.read_memory(ip - 1, 1).front() == 0xcc)
				exception.address = ip - 1;
		} catch (const std::runtime_error &) {
		}
		break;
	default:
		break;
	}
	return exception;
}

}
