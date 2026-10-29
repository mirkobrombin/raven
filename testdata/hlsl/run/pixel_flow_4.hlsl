// run: main ps_4_0 16
// expect: 0.0 0.0 -10.0 0.0 0.0 2.0 -10.0 22.0 1.0 0.0 10.0 31.0 2.0 2.0 10.0 33.0

float4 main(float4 position : SV_Position, float4 t : TEXCOORD0) : SV_Target
{
	// x is 0, 2, 4 and 6 across the four pixels.
	float x = t.x * 4 + 3;
	int i = int(x + 0.5);
	float4 r;
	r.x = i / 3;
	r.y = i % 4;
	r.z = x > 3 ? 10 : -10;
	r.w = 0;
	if (x >= 4)
		r.w += 1;
	if (i == 2 || i == 6)
		r.w += 2;
	for (int k = 0; k < 3; ++k)
		if (k < i)
			r.w += 10;
	return r;
}
