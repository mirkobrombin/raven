__declspec(dllimport) int RavenMissingImport(void);

__declspec(dllexport) int probe(void)
{
	return RavenMissingImport();
}
