#include "environment.h"
#include "nls.h"
#include "nls-cp1252.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <stdexcept>

namespace raven {
namespace {

using NTSTATUS=int32_t;
using ULONG=uint32_t;
using WCHAR=char16_t;
using CHAR=char;
using BYTE=uint8_t;
constexpr NTSTATUS STATUS_SUCCESS=0,STATUS_SOME_NOT_MAPPED=0x107;
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL=NTSTATUS(0xc0000023U);
constexpr NTSTATUS STATUS_INVALID_PARAMETER=NTSTATUS(0xc000000dU);
constexpr NTSTATUS STATUS_INVALID_PARAMETER_4=NTSTATUS(0xc00000f2U);
constexpr NTSTATUS STATUS_INVALID_PARAMETER_5=NTSTATUS(0xc00000f3U);
constexpr WCHAR replacement=0xfffd;

// RtlUTF8ToUnicodeN as ntdll answers it, malformed input included, which MultiByteToWideChar below passes on:
// - a byte that starts no sequence (0x80 to 0xc1, 0xf5 and up) is one U+FFFD;
// - a sequence a byte breaks off is one U+FFFD, and that byte starts the next one;
// - a sequence the input ends inside is one U+FFFD and ends the conversion;
// - a complete sequence for a surrogate, a value past U+10FFFF or an overlong form is one U+FFFD for each continuation byte;
// - units that do not fit are dropped one by one, so a pair may lose its second half, while the input is still read.
// The status is that of the last of these events.
NTSTATUS RtlUTF8ToUnicodeN(WCHAR *target,ULONG capacity,ULONG *written,const CHAR *source,ULONG size)
{
	if (!source) return STATUS_INVALID_PARAMETER_4;
	if (!written) return STATUS_INVALID_PARAMETER;
	NTSTATUS status=STATUS_SUCCESS;
	ULONG units=0,room=capacity/2;
	const auto put=[&](WCHAR unit) {
		if (!target) ++units;
		else if (room) { target[units++]=unit;--room; }
		else status=STATUS_BUFFER_TOO_SMALL;
	};
	const auto *bytes=reinterpret_cast<const BYTE *>(source);
	for (ULONG at=0;at<size;) {
		const BYTE lead=bytes[at++];
		uint32_t value=lead,continuations=0;
		if (lead>=0xf5 || (lead>=0x80 && lead<0xc2)) { status=STATUS_SOME_NOT_MAPPED;put(replacement);continue; }
		if (lead>=0xf0) { value=lead&7;continuations=3; }
		else if (lead>=0xe0) { value=lead&15;continuations=2; }
		else if (lead>=0x80) { value=lead&31;continuations=1; }
		if (size-at<continuations) { status=STATUS_SOME_NOT_MAPPED;put(replacement);break; }
		uint32_t taken=0;
		for (;taken<continuations && (bytes[at]&0xc0)==0x80;++taken) value=value<<6 | (bytes[at++]&0x3f);
		if (taken<continuations) { status=STATUS_SOME_NOT_MAPPED;put(replacement);continue; }
		if (value>0x10ffff || (value>=0xd800 && value<=0xdfff) || (continuations==2 && value<0x800) || (continuations==3 && value<0x10000)) {
			status=STATUS_SOME_NOT_MAPPED;
			for (uint32_t index=0;index<continuations;++index) put(replacement);
			continue;
		}
		if (value>=0x10000) {
			value-=0x10000;
			put(WCHAR(0xd800+(value>>10)));put(WCHAR(0xdc00+(value&0x3ff)));
		} else put(WCHAR(value));
	}
	*written=units*2;
	return status;
}

// RtlUnicodeToUTF8N as ntdll answers it: a surrogate without its partner is U+FFFD, and once a character does not fit nothing more
// is written, while the input is still read. The status is that of the last event, as above.
NTSTATUS RtlUnicodeToUTF8N(CHAR *target,ULONG capacity,ULONG *written,const WCHAR *source,ULONG size)
{
	if (!source) return STATUS_INVALID_PARAMETER_4;
	if (!written) return STATUS_INVALID_PARAMETER;
	if (target && size%2) return STATUS_INVALID_PARAMETER_5;
	NTSTATUS status=STATUS_SUCCESS;
	ULONG count=0;
	const ULONG units=size/2;
	for (ULONG at=0;at<units;) {
		uint32_t value=source[at++];
		if (value>=0xd800 && value<=0xdfff) {
			if (value<0xdc00 && at<units && source[at]>=0xdc00 && source[at]<=0xdfff) value=0x10000+((value-0xd800)<<10)+(source[at++]-0xdc00);
			else { value=replacement;status=STATUS_SOME_NOT_MAPPED; }
		}
		BYTE sequence[4];
		ULONG length=0;
		if (value<0x80) sequence[length++]=BYTE(value);
		else {
			const ULONG tail=value<0x800 ? 1 : value<0x10000 ? 2 : 3;
			sequence[length++]=BYTE((0xff00>>(tail+1)) | value>>(6*tail));
			for (ULONG shift=tail;shift--;) sequence[length++]=BYTE(0x80 | (value>>(6*shift)&0x3f));
		}
		if (!target) count+=length;
		else if (capacity>=length) { std::memcpy(target+count,sequence,length);count+=length;capacity-=length; }
		else { capacity=0;status=STATUS_BUFFER_TOO_SMALL; }
	}
	*written=count;
	return status;
}

struct Decomposition { uint16_t value,first,second; };
constexpr std::array<Decomposition,58> decompositions{{
#include "upstream/unicode/cp1252-decomposition.inc"
}};

}

std::string encode_utf8(std::u16string_view text,bool replace_invalid)
{
	if (text.size()>32766) throw uint32_t(120);
	if (text.empty()) return {};
	uint32_t needed=0,written=0;
	const auto status=RtlUnicodeToUTF8N(nullptr,0,&needed,text.data(),uint32_t(text.size()*2));
	if (status!=STATUS_SUCCESS && !(replace_invalid && status==STATUS_SOME_NOT_MAPPED)) throw uint32_t(87);
	std::string result(needed,'\0');
	if (RtlUnicodeToUTF8N(result.data(),needed,&written,text.data(),uint32_t(text.size()*2))!=status || written!=needed)
		throw std::runtime_error("UTF-8 conversion differs from its query");
	return result;
}

bool NativeEnvironment::multibyte_information(std::string_view name)
{
	if (name!="MultiByteToWideChar") return false;
	require_current();
	const auto args=process.api_arguments(6);
	uint64_t result=0,fault_address=args[2],access=0;
	try {
		const auto length=int32_t(uint32_t(args[3])),capacity=int32_t(uint32_t(args[5]));
		if (!args[2] || !length || capacity<0 || (capacity && (!args[4] || args[2]==args[4]))) throw uint32_t(87);
		uint32_t code_page=uint32_t(args[0]),flags=uint32_t(args[1]);
		if (!code_page || code_page==3) code_page=ansi_code_page;
		else if (code_page==1) code_page=437;
		if (flags & ~15U) throw uint32_t(1004);
		const auto *single=native_single_byte_page(code_page);
		const bool ascii=code_page==20127,latin1=code_page==28591;
		if (code_page!=1252 && code_page!=65001 && !single && !ascii && !latin1) throw uint32_t(120);
		constexpr size_t limit=1024*1024;
		std::vector<uint8_t> input;
		if (length>0) {
			if (size_t(length)>limit) throw uint32_t(120);
			input=process.read_memory(args[2],size_t(length));
		} else {
			for (;;) {
				if (input.size()==limit) throw uint32_t(120);
				if (args[2]>UINT64_MAX-input.size()) throw uint32_t(87);
				fault_address=args[2]+input.size();
				const auto count=std::min(limit-input.size(),size_t(4096-(fault_address & 4095)));
				const auto bytes=process.read_memory(fault_address,count);
				const auto zero=std::find(bytes.begin(),bytes.end(),uint8_t(0));
				input.insert(input.end(),bytes.begin(),zero==bytes.end() ? zero : zero+1);
				if (zero!=bytes.end()) break;
			}
		}
		std::u16string text;
		bool invalid=false,insufficient=false;
		if (code_page==1252 || single || ascii || latin1) {
			text.reserve(input.size());
			for (const auto byte:input) {
				const auto value=ascii ? uint16_t(byte & 0x7f) : latin1 ? uint16_t(byte) : single ? single->bytes[byte] : native_mb1252[byte];
				const auto found=std::lower_bound(decompositions.begin(),decompositions.end(),value,
					[](const Decomposition &entry,uint16_t character) { return entry.value<character; });
				// The selected decompositions cover the CP1252 characters; the other pages keep their composed characters.
				if ((flags & 2) && !single && !ascii && !latin1 && found!=decompositions.end() && found->value==value) {
					text.push_back(char16_t(found->first));text.push_back(char16_t(found->second));
				} else text.push_back(char16_t(value));
			}
			result=text.size();
			if (capacity && text.size()>size_t(capacity)) { text.resize(size_t(capacity));insufficient=true; }
		} else {
			uint32_t needed=0,written=0;
			const auto source=reinterpret_cast<const char *>(input.data());
			const auto query=RtlUTF8ToUnicodeN(nullptr,0,&needed,source,uint32_t(input.size()));
			invalid=query==STATUS_SOME_NOT_MAPPED;
			if (query!=STATUS_SUCCESS && !invalid) throw std::runtime_error("unexpected UTF-8 query status");
			if (!capacity) result=needed/2;
			else {
				text.resize(std::min(size_t(needed/2),size_t(capacity)));
				const auto status=RtlUTF8ToUnicodeN(text.data(),uint32_t(text.size()*2),&written,source,uint32_t(input.size()));
				if (status!=STATUS_SUCCESS && status!=STATUS_SOME_NOT_MAPPED && status!=STATUS_BUFFER_TOO_SMALL)
					throw std::runtime_error("unexpected UTF-8 conversion status");
				text.resize(written/2);result=text.size();
				insufficient=status==STATUS_BUFFER_TOO_SMALL;
				// The pinned Win32 reference accepts a final high surrogate in a one-unit remainder.
				if (insufficient && result+1==needed/2 && !text.empty() && text.back()>=0xd800 && text.back()<=0xdbff)
					insufficient=false;
			}
		}
		if (capacity && !text.empty()) {
			std::vector<uint8_t> bytes(text.size()*2);
			for (size_t i=0;i<text.size();++i) { bytes[i*2]=uint8_t(text[i]);bytes[i*2+1]=uint8_t(text[i]>>8); }
			fault_address=args[4];access=1;process.write_memory(fault_address,bytes);
		}
		if (insufficient) { result=0;set_last_error(122); }
		else if (invalid && (flags & 8)) { result=0;set_last_error(1113); }
	} catch (uint32_t error) { result=0;set_last_error(error); }
	catch (const std::bad_alloc &) { result=0;set_last_error(8); }
	catch (const std::runtime_error &error) {
		const std::string_view message=error.what();
		if (message.find("readable")==message.npos && message.find("writable")==message.npos) throw;
		NativeResult fault{NativeStop::exception,0xc0000005U,process.context().registers.rip-1,0};
		fault.fault_address=fault_address;fault.exception_information={access,fault_address};throw fault;
	}
	process.return_api(result);
	return true;
}

}
