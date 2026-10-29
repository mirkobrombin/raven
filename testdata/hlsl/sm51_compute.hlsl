// compile: gather cs_5_1
// listed only
// contains: dcl_uav_typed_buffer (uint,uint,uint,uint) u0[0:2], space=3
// contains: , t0[r1.x + 8].xyzw
// contains: store_uav_typed u0[r0.x + 0].xyzw

Texture2D<float> masks[4] : register(t8);
RWBuffer<uint> outputs[3] : register(u0, space3);

[numthreads(8, 1, 1)]
void gather(uint3 id : SV_DispatchThreadID)
{
	outputs[id.x % 3][id.x] = uint(masks[id.y & 3].Load(int3(id.xy, 0)) * 255);
}
