// error: undeclared ps_4_0 undeclared identifier 'missing'
// error: wrong_return ps_4_0 cannot convert
// error: bad_semantic ps_4_0 invalid pixel shader output semantic
// error: nowhere ps_4_0 entry point 'nowhere' not found
// error: recursive ps_4_0 recursive calls are not allowed

float4 undeclared() : SV_Target
{
	return missing;
}

struct S
{
	float a;
};

float4 wrong_return() : SV_Target
{
	S s;
	s.a = 1;
	return s;
}

float4 bad_semantic() : TEXCOORD0
{
	return 1;
}

float f(float x)
{
	return f(x - 1);
}

float4 recursive() : SV_Target
{
	return f(1);
}
