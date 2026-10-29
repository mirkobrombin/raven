#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stddef.h>

LONG NTAPI NtQuerySystemInformation(ULONG,void *,ULONG,ULONG *);

typedef struct {
	ULONG reserved,timer,page,pages,lowest,highest,granularity;
	ULONG_PTR minimum,maximum,affinity;
	BYTE processors;
} BasicInformation;

__declspec(dllexport) volatile unsigned phase;

typedef HRESULT (WINAPI *sl_open)(void **);
typedef HRESULT (WINAPI *sl_close)(void *);
typedef HRESULT (WINAPI *sl_set_data)(void *,UINT,const BYTE *);
typedef HRESULT (WINAPI *sl_get_result)(void *,UINT *,BYTE **);
typedef HRESULT (WINAPI *sl_install)(void *,UINT,const BYTE *,GUID *);
typedef HRESULT (WINAPI *sl_list)(void *,int,const GUID *,int,UINT *,GUID **);
typedef HRESULT (WINAPI *sl_information)(void *,const GUID *,const WCHAR *,DWORD *,UINT *,BYTE **);
typedef HRESULT (WINAPI *sl_load_policies)(const GUID *,const GUID *,DWORD,void **);

static const GUID first_license={0x5d2c0a11,0x7e4b,0x4c1a,{0x9b,0x30,0x11,0x22,0x33,0x44,0x55,0x66}};
static const GUID second_license={0x0aa0b0c0,0x1d2e,0x4f30,{0x80,0x01,0xa1,0xb2,0xc3,0xd4,0xe5,0xf6}};

static int same_guid(const GUID *left,const GUID *right)
{
	const BYTE *a=(const BYTE *)left,*b=(const BYTE *)right;
	for (unsigned index=0;index<sizeof(GUID);++index) if(a[index]!=b[index]) return 0;
	return 1;
}

/* The license store keeps what is installed and reads it back; an authentication challenge is kept but cannot be answered. */
static int check_licensing(void)
{
	static const char file[]="<?xml version=\"1.0\" encoding=\"utf-8\"?><rg:licenseGroup xmlns:rg=\"urn:x\" xmlns:r=\"urn:y\">"
		"<r:license xmlns:r=\"urn:y\" licenseId=\"{5D2C0A11-7E4B-4C1A-9B30-112233445566}\"><r:otherInfo><tm:infoStr name=\"licenseType\">"
		"Test</tm:infoStr></r:otherInfo></r:license>"
		"<r:license licenseId='0aa0b0c0-1d2e-4f30-8001-a1b2c3d4e5f6'><r:otherInfo><tm:infoStr name=\"licenseVersion\">2.0</tm:infoStr>"
		"<tm:infoStr name=\"productDescription\">One &amp; two</tm:infoStr></r:otherInfo></r:license></rg:licenseGroup>";
	static const char empty[]="<rg:licenseGroup/>";
	static const BYTE challenge[4]={1,2,3,4};
	HMODULE sppc=LoadLibraryW(L"sppc.dll");
	if (!sppc) return 20;
	sl_open open=(sl_open)GetProcAddress(sppc,"SLOpen");
	sl_close close=(sl_close)GetProcAddress(sppc,"SLClose");
	sl_set_data set_data=(sl_set_data)GetProcAddress(sppc,"SLSetAuthenticationData");
	sl_get_result get_result=(sl_get_result)GetProcAddress(sppc,"SLGetAuthenticationResult");
	sl_install install=(sl_install)GetProcAddress(sppc,"SLInstallLicense");
	sl_list list=(sl_list)GetProcAddress(sppc,"SLGetSLIDList");
	sl_information information=(sl_information)GetProcAddress(sppc,"SLGetLicenseInformation");
	sl_load_policies load_policies=(sl_load_policies)GetProcAddress(sppc,"SLLoadApplicationPolicies");
	if (!open || !close || !set_data || !get_result || !install || !list || !information || !load_policies) return 21;
	void *session=NULL;
	if (open(&session)!=S_OK || !session) return 22;
	UINT size=7;
	BYTE *value=(BYTE *)1;
	if ((ULONG)get_result(session,&size,&value)!=0xc004f079UL || size || value) return 23;
	if (set_data(session,sizeof(challenge),challenge)!=S_OK || set_data(session,1,NULL)!=E_INVALIDARG) return 24;
	if ((ULONG)get_result(session,&size,&value)!=0xc004f07aUL || size || value) return 25;
	GUID id;
	if ((ULONG)install(session,sizeof(empty)-1,(const BYTE *)empty,&id)!=0xc004f01fUL) return 26;
	if (install(session,sizeof(file)-1,(const BYTE *)file,&id)!=S_OK || !same_guid(&id,&first_license)) return 27;
	UINT count=0;
	GUID *ids=NULL;
	if (list(session,2,&id,3,&count,&ids)!=S_OK || count!=2 || !ids || !same_guid(&ids[0],&first_license) ||
		!same_guid(&ids[1],&second_license))
		return 28;
	LocalFree(ids);
	if ((ULONG)list(session,2,&second_license,3,&count,&ids)!=0xc004f011UL || count || ids) return 29;
	DWORD type=0;
	if (information(session,&second_license,L"Description",&type,&size,&value)!=S_OK || type!=REG_SZ || size!=20 ||
		lstrcmpW((const WCHAR *)value,L"One & two"))
		return 30;
	LocalFree(value);
	if (information(session,&first_license,L"LicenseType",&type,&size,&value)!=S_OK || lstrcmpW((const WCHAR *)value,L"Test")) return 31;
	LocalFree(value);
	if ((ULONG)information(session,&first_license,L"Version",&type,&size,&value)!=0xc004f012UL || size || value) return 32;
	void *policies=(void *)1;
	if ((ULONG)load_policies(&first_license,NULL,0,&policies)!=0xc004f072UL || policies) return 33;
	if (close(session)!=S_OK || close(session)!=E_INVALIDARG) return 34;
	return 0;
}

int raven_entry(void)
{
	BasicInformation info;
	ULONG length=0;
	SetLastError(71);
	if (sizeof(info)!=64 || NtQuerySystemInformation(0,&info,sizeof(info),&length)!=0 || length!=sizeof(info) ||
		GetLastError()!=71 || info.reserved || info.page!=4096 || info.granularity!=65536 ||
		!info.pages || info.highest<info.lowest || info.minimum!=65536 ||
		info.maximum<=info.minimum || !info.affinity || !info.processors)
		return 1;
	length=0;
	if ((ULONG)NtQuerySystemInformation(0,&info,sizeof(info)-1,&length)!=0xc0000004UL || length!=sizeof(info))
		return 2;
	if (NtQuerySystemInformation(0,&info,sizeof(info),NULL)!=0)
		return 3;
	{
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX group;
		DWORD size=0;
		const DWORD required=offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,Group.GroupInfo)+sizeof(PROCESSOR_GROUP_INFO);
		if (GetLogicalProcessorInformationEx(RelationGroup,NULL,&size) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || size!=required)
			return 4;
		unsigned char *bytes=(unsigned char *)&group;
		for (unsigned index=0;index<sizeof(group);++index) bytes[index]=0xa5;
		size=required-1;
		if (GetLogicalProcessorInformationEx(RelationGroup,&group,&size) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || size!=required)
			return 5;
		for (unsigned index=0;index<sizeof(group);++index) if(bytes[index]!=0xa5) return 6;
		SetLastError(71);
		if (!GetLogicalProcessorInformationEx(RelationGroup,&group,&size) || GetLastError()!=71 || size!=required ||
			group.Relationship!=RelationGroup || group.Size!=required || group.Group.ActiveGroupCount!=1 || group.Group.MaximumGroupCount!=1)
			return 7;
		PROCESSOR_GROUP_INFO *processors=&group.Group.GroupInfo[0];
		SYSTEM_INFO system;
		GetSystemInfo(&system);
		if (processors->ActiveProcessorCount!=system.dwNumberOfProcessors || processors->MaximumProcessorCount<processors->ActiveProcessorCount ||
			processors->MaximumProcessorCount>64 || processors->ActiveProcessorMask!=system.dwActiveProcessorMask)
			return 8;
		for (unsigned index=0;index<sizeof(group.Group.Reserved);++index) if(group.Group.Reserved[index]) return 9;
		for (unsigned index=0;index<sizeof(processors->Reserved);++index) if(processors->Reserved[index]) return 10;
		if (GetLogicalProcessorInformationEx(RelationGroup,&group,NULL) || GetLastError()!=ERROR_INVALID_PARAMETER)
			return 11;
		size=sizeof(group);
		if (GetLogicalProcessorInformationEx((LOGICAL_PROCESSOR_RELATIONSHIP)0x1234,&group,&size) || GetLastError()!=ERROR_INVALID_PARAMETER || size!=sizeof(group))
			return 12;
		static unsigned char all[4096];
		size=sizeof(all);
		if (!GetLogicalProcessorInformationEx(RelationAll,(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)all,&size))
			return 13;
		unsigned offset=0,groups=0;
		while(offset<size) {
			PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entry=(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(all+offset);
			if(size-offset<8 || entry->Size<8 || entry->Size>size-offset) return 14;
			if(entry->Relationship==RelationGroup) {
				if(entry->Size!=required) return 15;
				for(unsigned index=0;index<required;++index) if(all[offset+index]!=bytes[index]) return 16;
				++groups;
			}
			offset+=entry->Size;
		}
		if(groups!=1) return 17;
	}
	{
		const int licensing=check_licensing();
		if (licensing) return licensing;
	}
	phase=1;
	return 0;
}
