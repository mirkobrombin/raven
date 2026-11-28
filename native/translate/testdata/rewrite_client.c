// Guest code that rewrites an instruction of its own and runs it again: the store comes from translated code, as a JIT engine's does.
__attribute__((noinline)) unsigned int constant(void)
{
	return 1;
}

unsigned int raven_entry(void)
{
	unsigned int (*volatile function)(void) = constant;
	const unsigned int before = function();
	// mov eax, 1 is B8 01 00 00 00: its immediate starts one byte in.
	volatile unsigned char *code = (volatile unsigned char *)function;
	if (code[0] != 0xb8)
		return 0xbad;
	code[1] = 2;
	return before * 10 + function();
}
