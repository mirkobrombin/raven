// run: main ps_2_0 16
// expect: 5.0 10.0 0.0 1.0 16.0 9.0 2.0 1.0 10.0 8.0 4.0 1.0 -1.0 7.0 99.0 1.0

float4 main(float4 t : TEXCOORD0) : COLOR
{
	// i is 0, 1, 2 and 3 across the four pixels.
	const int i = int(t.x * 2 + 1.5);
	float4 r = 0;
	switch (i) {
	case 0:
		r.x = 5;
		break;
	case 1:
		r.x = 6;
	case 2:
		r.x += 10;
		break;
	default:
		r.x = -1;
		break;
	}
	for (int k = 0; k < 6; ++k) {
		if (k == i)
			continue;
		if (k > 4)
			break;
		r.y += k;
	}
	if (i == 3)
		return float4(r.xy, 99, 1);
	r.z = i * 2;
	r.w = 1;
	return r;
}
