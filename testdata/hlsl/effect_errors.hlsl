// error: - fx_4_0 technique11 needs fx_5_0
// error: - fx_2_0 technique11 needs fx_5_0
// error: - fx_3_0 is not an effect target
// error: - fx_5_0 unknown state 'Blending'

technique11 T
{
	pass P
	{
		Blending = 1;
	}
}
