#include "memory.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {

static constexpr uint32_t invalid = 0xc000000dU, unsupported = 0xc00000bbU;
static constexpr uint32_t no_memory = 0xc0000017U, conflict = 0xc0000018U;

static int protection(uint32_t value)
{
	switch (value) {
	case 1: return PROT_NONE;
	case 2: return PROT_READ;
	case 4: return PROT_READ | PROT_WRITE;
	case 0x20: return PROT_READ | PROT_EXEC;
	case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return -1;
	}
}

static void put(std::span<uint8_t> bytes, size_t at, uint64_t value, size_t width = 8)
{
	for (size_t byte=0; byte<width; ++byte)
		bytes[at+byte]=uint8_t(value>>(byte*8));
}

// x86 guests keep Windows' default 2 GiB user range so every allocation stays positive as a signed pointer.
static constexpr uint64_t x86_user_limit = 0x7fff0000ULL;

static bool range(uint64_t address, uint64_t size, uint64_t user_limit)
{
	return size && address<user_limit && size<=user_limit-address;
}

void NativeMemory::clear(uint64_t address, uint64_t size)
{
	process.write_memory(address, std::vector<uint8_t>(size));
}

NativeMemory::NativeMemory(NativeProcess &process, NativeEnvironment &environment, size_t limit)
	: process(process), environment(environment), limit(limit)
{
	if (process.mode()==NativeMode::x86) {
		user_limit=x86_user_limit;
		next=floor=0x20000000ULL;
	}
}

std::span<const std::string_view> NativeMemory::nt_exports()
{
	static constexpr std::array<std::string_view,4> names{
		"NtAllocateVirtualMemory","NtFreeVirtualMemory","NtProtectVirtualMemory","NtQueryVirtualMemory"};
	return names;
}

void NativeMemory::settle()
{
	if (!commit_block)
		return;
	auto *count=reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(commit_block)+commit_log);
	const uint32_t logged=std::min<uint32_t>(*count,commit_log_count);
	const uint64_t *entries=commit_block+(commit_log+8)/8;
	for (uint32_t index=0; index<logged; ++index) {
		// A start with its low bit set is a decommit, done by the guest on pages it keeps open.
		const bool decommit=entries[index*2] & 1;
		const uint64_t base=entries[index*2] & ~uint64_t(1), end=entries[index*2+1];
		const auto found=end>base ? region(base,end-base) : regions.end();
		if (found==regions.end())
			continue;
		const auto first=(base-found->first)/4096, pages=(end-base)/4096;
		const auto committed=found->second.pages.nonzero(first,pages);
		if (decommit) {
			used-=committed*4096;
			found->second.pages.fill(first,pages,0);
		} else {
			used+=(pages-committed)*4096;
			found->second.pages.fill(first,pages,4);
		}
	}
	*count=0;
}

void NativeMemory::open_window(uint64_t from, uint64_t to)
{
	if (!commit_block)
		return;
	uint64_t *windows=commit_block+commit_windows/8;
	// An allocator works through its reservation in order, so a range that touches one already open is joined to it.
	for (uint64_t index=0; index<windows[0]; ++index)
		if (windows[1+index*2]<=to && from<=windows[2+index*2]) {
			windows[1+index*2]=std::min(windows[1+index*2],from);
			windows[2+index*2]=std::max(windows[2+index*2],to);
			return;
		}
	if (windows[0]<commit_window_count) {
		window_next=windows[0];
		windows[0]+=1;
	} else {
		window_next=(window_next+1)%commit_window_count;
	}
	windows[1+window_next*2]=from;
	windows[2+window_next*2]=to;
}

bool NativeMemory::window_holds(uint64_t from, uint64_t to) const
{
	if (!commit_block)
		return false;
	const uint64_t *windows=commit_block+commit_windows/8;
	for (uint64_t index=0; index<windows[0]; ++index)
		if (windows[1+index*2]<=from && to<=windows[2+index*2])
			return true;
	return false;
}

void NativeMemory::close_windows(uint64_t from, uint64_t to)
{
	if (!commit_block)
		return;
	uint64_t *windows=commit_block+commit_windows/8;
	for (uint64_t index=0; index<windows[0];) {
		if (windows[1+index*2]<to && from<windows[2+index*2]) {
			windows[1+index*2]=windows[1+(windows[0]-1)*2];
			windows[2+index*2]=windows[2+(windows[0]-1)*2];
			windows[0]-=1;
		} else {
			++index;
		}
	}
}

std::map<uint64_t,NativeMemory::Region>::iterator NativeMemory::region(uint64_t address, uint64_t size)
{
	auto found=regions.upper_bound(address);
	if (found==regions.begin())
		return regions.end();
	--found;
	return address-found->first<found->second.size && size<=found->second.size-(address-found->first) ? found : regions.end();
}

NativeMemoryRange NativeMemory::allocate(uint64_t address, uint64_t size, uint32_t type, uint32_t access)
{
	settle();
	if (!range(address,size,user_limit))
		return {0,0,invalid};
	// MEM_TOP_DOWN only asks for the highest free address; any free address serves the program as well.
	type&=~0x100000U;
	// MEM_RESET and MEM_RESET_UNDO only say whether the program still needs what committed pages hold. Keeping it always is allowed, so
	// they change nothing once the range is known to be committed; the protection asked for is ignored, as Windows ignores it.
	if (type==0x80000U || type==0x1000000U) {
		const uint64_t base=address & ~uint64_t(4095), length=(size+address-base+4095) & ~uint64_t(4095);
		const auto found=address && size ? region(base,length) : regions.end();
		if (found==regions.end())
			return {0,0,invalid};
		const auto first=(base-found->first)/4096;
		if (found->second.pages.nonzero(first,length/4096)!=length/4096)
			return {0,0,conflict};
		return {base,length};
	}
	if (!(type & 0x3000) || (type & ~0x3000U))
		return {0,0,unsupported};
	const auto permissions=protection(access);
	if (permissions<0)
		return {0,0,0xc0000045U};
	const bool reserve=!address || (type & 0x2000);
	const uint64_t base=address & ~(reserve ? uint64_t(65535) : uint64_t(4095));
	const uint64_t length=(size+address-base+4095) & ~uint64_t(4095);
	if (!reserve) {
		const auto found=region(base,length);
		if (found==regions.end())
			return {0,0,conflict};
		const auto first=(base-found->first)/4096;
		// Programs such as a browser's allocators commit a few pages at a time, one block after the next, and each change of the guest's
		// protection stops it. Read-write commits of part of a 64 KiB granule therefore also open the neighbouring pages that are only
		// reserved, up to the 1 MiB around them; they stay reserved here, and committing them later changes nothing in the guest.
		// Commits of whole granules, as a WebAssembly memory grows with guard pages after it, open nothing more, and pages committed
		// otherwise are never touched.
		const uint64_t granule=((base | length) & 65535) ? uint64_t(1) << 20 : 4096;
		if (!process.has_protection(base,length,permissions)) {
			uint64_t from=base, to=base+length;
			if (permissions==(PROT_READ|PROT_WRITE)) {
				const uint64_t region_end=found->first+found->second.size;
				const uint64_t window_start=std::max(found->first,base & ~(granule-1));
				const uint64_t window_end=std::min(region_end,(base+length+granule-1) & ~(granule-1));
				while (from>window_start && !found->second.pages.at((from-4096-found->first)/4096))
					from-=4096;
				while (to<window_end && !found->second.pages.at((to-found->first)/4096))
					to+=4096;
			}
			close_windows(from,to);
			process.protect_memory(from,to-from,permissions);
			if (permissions==(PROT_READ|PROT_WRITE))
				open_window(from,to);
		} else if (permissions==(PROT_READ|PROT_WRITE)) {
			// Already open: its granule's open pages are published again, as the range that held them may have been dropped.
			const uint64_t region_end=found->first+found->second.size;
			const uint64_t window_start=std::max(found->first,base & ~(granule-1));
			const uint64_t window_end=std::min(region_end,(base+length+granule-1) & ~(granule-1));
			uint64_t from=base, to=base+length;
			while (from>window_start && process.has_protection(from-4096,4096,permissions))
				from-=4096;
			while (to<window_end && process.has_protection(to,4096,permissions))
				to+=4096;
			open_window(from,to);
		}
		// Only pages that were reserved until now count as new commitment.
		const uint64_t fresh=(length/4096-found->second.pages.nonzero(first,length/4096))*4096;
		if (fresh>limit-used)
			return {0,0,no_memory};
		found->second.pages.fill(first,length/4096,access);
		used+=fresh;
		return {base,length};
	}
	const bool commit_now=type & 0x1000;
	if ((commit_now && length>limit-used) || regions.size()>=10000)
		return {0,0,no_memory};
	uint64_t selected=base;
	if (!address) {
		// The search starts after the last range handed out; at the top of the address space it goes on once from the bottom, over
		// the ranges freed since, as a 32-bit program that starts and ends many threads frees their stacks there.
		selected=next;
		bool wrapped=false;
		for (size_t tries=0; tries<20000; ++tries) {
			if (!range(selected,length,user_limit)) {
				if (wrapped)
					return {0,0,no_memory};
				wrapped=true;
				selected=floor;
				continue;
			}
			if (process.memory_available(selected,length))
				break;
			selected+=((length+65535)&~uint64_t(65535));
			if (tries==19999)
				return {0,0,no_memory};
		}
	}
	if (!selected || !process.memory_available(selected,length))
		return {0,0,conflict};
	const bool commit=commit_now;
	auto [found,inserted]=regions.emplace(selected,Region{length,access,PageMap(length/4096,commit ? access : 0)});
	if (!inserted)
		return {0,0,conflict};
	try { process.map_memory(length,commit ? permissions : PROT_NONE,selected); }
	catch (...) { regions.erase(found); throw; }
	if (commit)
		used+=length;
	reserved_total+=length;
	if (!address)
		next=selected+((length+65535)&~uint64_t(65535));
	return {selected,length};
}

NativeMemoryRange NativeMemory::map_view(int descriptor, uint64_t offset, uint64_t size, uint32_t access)
{
	settle();
	const auto permissions=protection(access);
	if (permissions<0 || !size || (offset & 4095))
		return {0,0,invalid};
	const uint64_t length=(size+4095)&~uint64_t(4095);
	if (length>limit-used || regions.size()>=10000)
		return {0,0,no_memory};
	const auto address=process.map_shared(descriptor,length,offset);
	if (permissions!=(PROT_READ|PROT_WRITE))
		process.protect_memory(address,length,permissions);
	regions.emplace(address,Region{length,access,PageMap(length/4096,access)});
	used+=length;
	reserved_total+=length;
	return {address,length};
}

NativeMemoryRange NativeMemory::free(uint64_t address, uint64_t size, uint32_t type)
{
	settle();
	if (type!=0x4000 && type!=0x8000)
		return {0,0,unsupported};
	const auto found=region(address,size);
	if (found==regions.end())
		return {0,0,conflict};
	if (type==0x8000) {
		if (size || address!=found->first)
			return {0,0,invalid};
		const auto length=found->second.size;
		close_windows(address,address+length);
		process.unmap_memory(address);
		used-=found->second.pages.nonzero(0,found->second.pages.size())*4096;
		reserved_total-=length;
		regions.erase(found);
		return {address,length};
	}
	if (!size) {
		if (address!=found->first)
			return {0,0,invalid};
		size=found->second.size;
	}
	const auto base=address & ~uint64_t(4095), length=(size+address-base+4095)&~uint64_t(4095);
	if (region(base,length)==regions.end())
		return {0,0,conflict};
	// Pages inside an open range keep it: they are emptied, so a later commit finds them zeroed, and stay readable and writable, as the
	// pages opened around a commit are. Allocators decommit and commit the same pages over and over, and the next commit is then
	// done by the guest. Pages outside one become inaccessible again.
	if (window_holds(base,base+length) && process.has_protection(base,length,PROT_READ|PROT_WRITE)) {
		process.discard_memory(base,length);
	} else {
		close_windows(base,base+length);
		process.discard_memory(base,length);
		process.protect_memory(base,length,PROT_NONE);
	}
	const auto first=(base-found->first)/4096;
	used-=found->second.pages.nonzero(first,length/4096)*4096;
	found->second.pages.fill(first,length/4096,0);
	return {base,length};
}

void NativeMemory::add_image(uint64_t base, uint64_t size, std::span<const std::pair<std::pair<uint64_t,uint64_t>,uint32_t>> sections)
{
	images[base]=Image{size,std::vector<std::pair<std::pair<uint64_t,uint64_t>,uint32_t>>(sections.begin(),sections.end()),{}};
}

uint32_t NativeMemory::Image::access_of(uint64_t offset) const
{
	if (const auto found=pages.find(offset & ~uint64_t(4095)); found!=pages.end())
		return found->second;
	for (const auto &[range,access] : sections)
		if (offset>=range.first && offset<range.first+range.second)
			return access;
	return 2;
}

std::map<uint64_t,NativeMemory::Image>::iterator NativeMemory::image_of(uint64_t address, uint64_t size)
{
	auto found=images.upper_bound(address);
	if (found==images.begin())
		return images.end();
	--found;
	if (address-found->first>found->second.size || size>found->second.size-(address-found->first))
		return images.end();
	return found;
}

NativeMemoryRange NativeMemory::protect(uint64_t address, uint64_t size, uint32_t access)
{
	settle();
	if (!range(address,size,user_limit))
		return {0,0,invalid};
	if (const auto image=image_of(address,size); image!=images.end() && region(address & ~uint64_t(4095),1)==regions.end()) {
		// Image storage belongs to this process, including a writable copy requested for an interception patch.
		const auto permissions=protection(access==8 ? 4 : access==0x80 ? 0x40 : access);
		if (permissions<0)
			return {0,0,0xc0000045U};
		const auto base=address & ~uint64_t(4095), length=(size+address-base+4095)&~uint64_t(4095);
		const auto previous=image->second.access_of(base-image->first);
		process.protect_image(base,length,permissions);
		for (uint64_t page=base;page<base+length;page+=4096)
			image->second.pages[page-image->first]=access;
		return {base,length,0,previous};
	}
	const auto permissions=protection(access);
	if (permissions<0)
		return {0,0,0xc0000045U};
	const auto base=address & ~uint64_t(4095), length=(size+address-base+4095)&~uint64_t(4095);
	const auto found=region(base,length);
	if (found==regions.end())
		return {0,0,conflict};
	const auto first=(base-found->first)/4096, count=length/4096;
	if (found->second.pages.any_zero(first,count))
		return {0,0,0xc000002dU};
	const auto previous=found->second.pages.at(first);
	close_windows(base,base+length);
	process.protect_memory(base,length,permissions);
	found->second.pages.fill(first,count,access);
	return {base,length,0,previous};
}

std::optional<std::array<uint8_t,48>> NativeMemory::query(uint64_t address) const
{
	// The guest's own commits count before anything is reported; the accounting is the only thing that changes.
	const_cast<NativeMemory *>(this)->settle();
	if (regions.upper_bound(address)==regions.begin() || [&]{ auto it=regions.upper_bound(address); --it; return address-it->first>=it->second.size; }()) {
		auto found=images.upper_bound(address);
		if (found!=images.begin()) {
			--found;
			if (address-found->first<found->second.size) {
				const auto page=(address-found->first)&~uint64_t(4095);
				const auto access=found->second.access_of(page);
				uint64_t end=page+4096;
				while (end<found->second.size && found->second.access_of(end)==access)
					end+=4096;
				std::array<uint8_t,48> bytes{};
				put(bytes,0,found->first+page); put(bytes,8,found->first);
				put(bytes,16,access,4); put(bytes,24,end-page);
				put(bytes,32,0x1000,4); put(bytes,36,access,4); put(bytes,40,0x1000000,4);
				return bytes;
			}
		}
	}
	auto found=regions.upper_bound(address);
	// Address space that nothing holds is free, up to the next reservation or image, as Windows reports it to programs that look for room.
	// The stack the first thread starts on is the exception: committed read-write memory of its own allocation, which a runtime that
	// measures its stack finds from any address in it.
	const auto free_space=[&]() -> std::optional<std::array<uint8_t,48>> {
		const uint64_t page=address&~uint64_t(4095);
		if (page<0x10000 || page>=user_limit)
			return std::nullopt;
		const auto [stack_low,stack_high]=process.stack_bounds();
		if (page>=stack_low && page<stack_high) {
			std::array<uint8_t,48> bytes{};
			put(bytes,0,page); put(bytes,8,stack_low);
			put(bytes,16,4,4); put(bytes,24,stack_high-page);
			put(bytes,32,0x1000,4); put(bytes,36,4,4); put(bytes,40,0x20000,4);
			return bytes;
		}
		uint64_t end=user_limit;
		if (page<stack_low)
			end=stack_low;
		if (const auto next_region=regions.upper_bound(address); next_region!=regions.end())
			end=std::min(end,next_region->first);
		if (const auto next_image=images.upper_bound(address); next_image!=images.end())
			end=std::min(end,next_image->first);
		std::array<uint8_t,48> bytes{};
		put(bytes,0,page); put(bytes,24,end-page);
		put(bytes,32,0x10000,4); put(bytes,36,1,4);
		return bytes;
	};
	if (found==regions.begin())
		return free_space();
	--found;
	if (address-found->first>=found->second.size)
		return free_space();
	const auto first=(address-found->first)/4096;
	const auto access=found->second.pages.at(first);
	const uint64_t end=found->second.pages.run_end(first);
	std::array<uint8_t,48> bytes{};
	put(bytes,0,found->first+first*4096); put(bytes,8,found->first);
	put(bytes,16,found->second.allocation_protection,4); put(bytes,24,(end-first)*4096);
	put(bytes,32,access ? 0x1000 : 0x2000,4); put(bytes,36,access,4); put(bytes,40,0x20000,4);
	return bytes;
}

static uint32_t error(uint32_t status)
{
	if (status==unsupported) return 120;
	if (status==no_memory) return 8;
	if (status==conflict || status==0xc000002dU) return 487;
	if (status==0xc0000005U) return 998;
	return 87;
}

bool NativeMemory::dispatch(std::string_view name)
{
	const bool nt=name.starts_with("Nt");
	const bool alloc=name=="VirtualAlloc" || name=="NtAllocateVirtualMemory";
	const bool release=name=="VirtualFree" || name=="NtFreeVirtualMemory";
	const bool change=name=="VirtualProtect" || name=="NtProtectVirtualMemory";
	// Other process handles are served by the process API through their owner's channel.
	const bool ex=name=="VirtualQueryEx";
	const bool inspect=name=="VirtualQuery" || name=="NtQueryVirtualMemory" || ex;
	if (!alloc && !release && !change && !inspect)
		return false;
	auto args=process.api_arguments(nt ? (alloc || inspect ? 6 : change ? 5 : 4) : alloc || change || ex ? 4 : 3);
	const bool foreign=ex && args[0]!=UINT64_MAX;
	if (foreign)
		return false;
	if (ex)
		args.erase(args.begin());
	uint64_t result=0;
	uint32_t status=0;
	try {
		if (nt && args[0]!=UINT64_MAX)
			status=0xc0000008U;
		else if (alloc && nt && args[2])
			status=unsupported;
		else if (inspect) {
			const auto output=nt ? args[3] : args[1], size=nt ? args[4] : args[2];
			if ((nt && args[2]) || size<48)
				status=nt && args[2] ? unsupported : 0xc0000004U;
			else if (const auto info=query(args[nt ? 1 : 0])) {
				if (nt && args[5]) {
					const auto previous=process.read_memory(args[5],8);
					process.write_memory(args[5],previous);
				}
				process.write_memory(output,*info);
				if (nt && args[5]) {
					std::array<uint8_t,8> length{}; put(length,0,48); process.write_memory(args[5],length);
				}
				result=48;
			} else status=unsupported;
		} else {
			uint64_t address=args[nt ? 1 : 0], size=args[nt ? (alloc ? 3 : 2) : 1];
			const auto read=[&](uint64_t pointer) {
				const auto bytes=process.read_memory(pointer,8); process.write_memory(pointer,bytes);
				uint64_t value=0; for (size_t byte=0; byte<8; ++byte) value|=uint64_t(bytes[byte])<<(byte*8);
				return value;
			};
			if (nt) { address=read(address); size=read(size); }
			const auto old=change ? args[nt ? 4 : 3] : 0;
			if (change) {
				const auto bytes=process.read_memory(old,4); process.write_memory(old,bytes);
			}
			const auto base=address & ~uint64_t(4095);
			uint64_t length=(size+address-base+4095)&~uint64_t(4095);
			if (release && !size)
				if (const auto info=query(address)) {
					length=0; for (size_t byte=0; byte<8; ++byte) length|=uint64_t((*info)[24+byte])<<(byte*8);
				}
			const auto overlaps=[&](uint64_t pointer, size_t width) { return pointer<base+length && pointer+width>base; };
			if ((change && overlaps(old,4)) || (nt && (overlaps(args[1],8) || overlaps(args[alloc ? 3 : 2],8))))
				status=unsupported;
			else {
				const auto operation=alloc ? allocate(address,size,uint32_t(args[nt ? 4 : 2]),uint32_t(args[nt ? 5 : 3])) :
					release ? free(address,size,uint32_t(args[nt ? 3 : 2])) : protect(address,size,uint32_t(args[nt ? 3 : 2]));
				status=operation.status;
				if (!status) {
					if (nt) {
						std::array<uint8_t,8> output{}; put(output,0,operation.address); process.write_memory(args[1],output);
						put(output,0,operation.size); process.write_memory(args[alloc ? 3 : 2],output);
					}
					if (change) { std::array<uint8_t,4> output{}; put(output,0,operation.previous,4); process.write_memory(old,output); }
					result=alloc ? operation.address : 1;
				}
			}
		}
	} catch (const std::bad_alloc &) {
		status=no_memory;
	} catch (const std::runtime_error &failure) {
		const std::string_view message=failure.what();
		if (message.find("readable")==message.npos && message.find("writable")==message.npos)
			throw;
		status=0xc0000005U;
	}
	// RAVEN_TRACE_MEMORY lists every virtual memory request with the arguments the program passed and the status it got.
	static const bool trace=std::getenv("RAVEN_TRACE_MEMORY")!=nullptr;
	if (trace)
		std::fprintf(stderr,"mem %.*s [%llx %llx %llx %llx %llx %llx] status %x result %llx\n",int(name.size()),name.data(),
			(unsigned long long)args[0],(unsigned long long)(args.size()>1 ? args[1] : 0),(unsigned long long)(args.size()>2 ? args[2] : 0),
			(unsigned long long)(args.size()>3 ? args[3] : 0),(unsigned long long)(args.size()>4 ? args[4] : 0),
			(unsigned long long)(args.size()>5 ? args[5] : 0),status,(unsigned long long)result);
	if (!nt && status)
		environment.set_last_error(status==0xc0000004U ? 24 : error(status));
	process.return_api(nt ? status : status ? 0 : result);
	return true;
}

}
