#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int member(BYTE count,DWORD first,DWORD second,int authority)
{
	SID_IDENTIFIER_AUTHORITY nt={{0,0,0,0,0,(BYTE)authority}};
	PSID sid=NULL;
	BOOL held=FALSE;
	if (!AllocateAndInitializeSid(&nt,count,first,second,0,0,0,0,0,0,&sid)) return -1;
	if (!CheckTokenMembership(NULL,sid,&held)) held=-1;
	FreeSid(sid);
	return held;
}

// A program run by the desktop user holds the groups of an interactive logon; some check INTERACTIVE before they save settings.
int raven_entry(void)
{
	if (member(1,SECURITY_INTERACTIVE_RID,0,5)!=1) return 1;
	if (member(2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_USERS,5)!=1) return 2;
	if (member(1,SECURITY_AUTHENTICATED_USER_RID,0,5)!=1) return 3;
	if (member(1,0,0,1)!=1) return 4;
	if (member(1,SECURITY_LOCAL_SYSTEM_RID,0,5)!=0) return 5;
	if (member(1,SECURITY_SERVICE_RID,0,5)!=0) return 6;
	return 0;
}
