/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The pixel shaders of effects run on a Direct3D 11 device of this component's own, apart from the program's, so that nothing it set on
 * its own device changes. Each run draws one rectangle: Direct2D's vertex stage gives the shader the position of each pixel in the
 * transform's space (SCENE_POSITION) and, for each input, where that position falls in the input's texture with the size of one texel
 * (TEXCOORDn), which is what the shaders Direct2D's effect helpers build read. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>

#include "shader_run.h"

typedef HRESULT (WINAPI *create_device_function)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
	ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *compile_function)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, const char *,
	const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

/* The rectangle drawn in the transform's space, then for each input the position of its first pixel and the size of one of its texels. */
static const char vertex_source[] =
	"cbuffer area : register(b0) { float4 output; float4 input0, input1, input2, input3, input4, input5, input6, input7; };\n"
	"struct corner { float4 position : SV_POSITION; float4 scene : SCENE_POSITION; float4 texel0 : TEXCOORD0; float4 texel1 : TEXCOORD1;\n"
	"	float4 texel2 : TEXCOORD2; float4 texel3 : TEXCOORD3; float4 texel4 : TEXCOORD4; float4 texel5 : TEXCOORD5;\n"
	"	float4 texel6 : TEXCOORD6; float4 texel7 : TEXCOORD7; };\n"
	"float4 texel(float2 scene, float4 input) { return float4((scene - input.xy) * input.zw, input.zw); }\n"
	"corner main(uint id : SV_VertexID)\n"
	"{\n"
	"	corner result;\n"
	"	float2 at = float2(id & 1, id >> 1);\n"
	"	float2 scene = output.xy + at * output.zw;\n"
	"	result.position = float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1);\n"
	"	result.scene = float4(scene, 0, 0);\n"
	"	result.texel0 = texel(scene, input0);\n"
	"	result.texel1 = texel(scene, input1);\n"
	"	result.texel2 = texel(scene, input2);\n"
	"	result.texel3 = texel(scene, input3);\n"
	"	result.texel4 = texel(scene, input4);\n"
	"	result.texel5 = texel(scene, input5);\n"
	"	result.texel6 = texel(scene, input6);\n"
	"	result.texel7 = texel(scene, input7);\n"
	"	return result;\n"
	"}\n";

/* The pixel shaders made so far, by the code they were made from: a program loads each once and draws with it every frame. */
enum { KEPT_SHADERS = 64 };

static struct {
	INIT_ONCE once;
	CRITICAL_SECTION lock;
	BOOL tried;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	ID3D11VertexShader *vertex;
	ID3D11Buffer *area;
	struct {
		const BYTE *code;
		UINT32 size;
		ID3D11PixelShader *shader;
	} kept[KEPT_SHADERS];
	UINT32 kept_count, kept_next;
	/* The texture the last run drew into and the one it was read back through, kept for the next run of the same size: an effect
	   drawn each frame runs over the same area each time. */
	ID3D11Texture2D *target, *staging;
	ID3D11RenderTargetView *target_view;
	UINT32 target_width, target_height;
	BOOL target_bytes;
} runner = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK runner_init(INIT_ONCE *once, void *parameter, void **context)
{
	InitializeCriticalSection(&runner.lock);
	return TRUE;
}

/* The device, its vertex stage and the buffer that places the rectangle, made on the first run. */
static BOOL runner_ready(void)
{
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
	create_device_function create;
	compile_function compile;
	HMODULE d3d11, compiler;
	ID3DBlob *code = NULL, *errors = NULL;
	D3D11_BUFFER_DESC desc = {sizeof(float) * 4 * (1 + SHADER_INPUTS), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	if (runner.tried)
		return runner.device != NULL;
	runner.tried = TRUE;
	if (!(d3d11 = LoadLibraryA("d3d11.dll")) || !(create = (create_device_function)GetProcAddress(d3d11, "D3D11CreateDevice")) ||
		!(compiler = LoadLibraryA("d3dcompiler_47.dll")) || !(compile = (compile_function)GetProcAddress(compiler, "D3DCompile")))
		return FALSE;
	if (FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &runner.device, NULL,
		&runner.context)))
		return FALSE;
	if (FAILED(compile(vertex_source, sizeof(vertex_source) - 1, "effects", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &errors)) ||
		FAILED(ID3D11Device_CreateVertexShader(runner.device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL,
			&runner.vertex)) ||
		FAILED(ID3D11Device_CreateBuffer(runner.device, &desc, NULL, &runner.area))) {
		if (code)
			ID3D10Blob_Release(code);
		if (errors)
			ID3D10Blob_Release(errors);
		ID3D11DeviceContext_Release(runner.context);
		ID3D11Device_Release(runner.device);
		runner.context = NULL;
		runner.device = NULL;
		return FALSE;
	}
	ID3D10Blob_Release(code);
	if (errors)
		ID3D10Blob_Release(errors);
	return TRUE;
}

static ID3D11PixelShader *pixel_shader(const BYTE *code, UINT32 size)
{
	ID3D11PixelShader *shader = NULL;
	UINT32 index;
	for (index = 0; index < runner.kept_count; ++index)
		if (runner.kept[index].code == code && runner.kept[index].size == size)
			return runner.kept[index].shader;
	if (FAILED(ID3D11Device_CreatePixelShader(runner.device, code, size, NULL, &shader)))
		return NULL;
	/* Past as many as are kept, the oldest gives its place. */
	index = runner.kept_count < KEPT_SHADERS ? runner.kept_count++ : runner.kept_next++ % KEPT_SHADERS;
	if (runner.kept[index].shader)
		ID3D11PixelShader_Release(runner.kept[index].shader);
	runner.kept[index].code = code;
	runner.kept[index].size = size;
	runner.kept[index].shader = shader;
	return shader;
}

/* A texture holding an input's pixels, of floats or of bytes as the input has them, with its view; an empty input is one transparent
 * pixel. */
static BOOL input_texture(const struct shader_image *image, ID3D11Texture2D **texture, ID3D11ShaderResourceView **view)
{
	static const float clear[4];
	const BOOL empty = (!image->pixels && !image->bytes) || !image->width || !image->height, bytes = !empty && !image->pixels;
	D3D11_TEXTURE2D_DESC desc = {empty ? 1 : image->width, empty ? 1 : image->height, 1, 1,
		bytes ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
	D3D11_SUBRESOURCE_DATA data = {empty ? (const void *)clear : bytes ? (const void *)image->bytes : (const void *)image->pixels,
		desc.Width * (bytes ? 4 : sizeof(float) * 4), 0};
	*texture = NULL;
	*view = NULL;
	if (FAILED(ID3D11Device_CreateTexture2D(runner.device, &desc, &data, texture)))
		return FALSE;
	if (FAILED(ID3D11Device_CreateShaderResourceView(runner.device, (ID3D11Resource *)*texture, NULL, view))) {
		ID3D11Texture2D_Release(*texture);
		*texture = NULL;
		return FALSE;
	}
	return TRUE;
}

HRESULT shader_run(const BYTE *code, UINT32 size, const BYTE *constants, UINT32 constant_size, const struct shader_image *inputs,
	UINT32 count, INT32 left, INT32 top, UINT32 width, UINT32 height, float *out, BYTE *out_bytes)
{
	ID3D11Texture2D *textures[SHADER_INPUTS] = {0}, *target, *staging;
	ID3D11ShaderResourceView *views[SHADER_INPUTS] = {0};
	ID3D11SamplerState *samplers[SHADER_INPUTS] = {0};
	ID3D11RenderTargetView *target_view;
	ID3D11Buffer *constant_buffer = NULL;
	ID3D11PixelShader *shader;
	const BOOL bytes = !out;
	const size_t pixel = bytes ? 4 : sizeof(float) * 4;
	D3D11_TEXTURE2D_DESC target_desc = {width, height, 1, 1, bytes ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
		D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
	D3D11_MAPPED_SUBRESOURCE mapped;
	D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
	float area[4 * (1 + SHADER_INPUTS)] = {(float)left, (float)top, (float)width, (float)height};
	HRESULT result = E_FAIL;
	UINT32 index, row;
	if (!code || !size || (!out && !out_bytes) || !width || !height || count > SHADER_INPUTS)
		return E_INVALIDARG;
	InitOnceExecuteOnce(&runner.once, runner_init, NULL, NULL);
	EnterCriticalSection(&runner.lock);
	if (!runner_ready() || !(shader = pixel_shader(code, size)))
		goto done;
	for (index = 0; index < count; ++index) {
		/* Where outside an input the shader samples, the input is transparent, as Direct2D gives nothing beyond an image's pixels. */
		D3D11_SAMPLER_DESC sampler = {(D3D11_FILTER)inputs[index].filter, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER,
			D3D11_TEXTURE_ADDRESS_BORDER, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
		if (!input_texture(&inputs[index], &textures[index], &views[index]) ||
			FAILED(ID3D11Device_CreateSamplerState(runner.device, &sampler, &samplers[index])))
			goto done;
		area[4 * (1 + index)] = (float)inputs[index].left;
		area[4 * (1 + index) + 1] = (float)inputs[index].top;
		area[4 * (1 + index) + 2] = 1.0f / (float)(inputs[index].width ? inputs[index].width : 1);
		area[4 * (1 + index) + 3] = 1.0f / (float)(inputs[index].height ? inputs[index].height : 1);
	}
	if (constant_size) {
		/* A constant buffer is a whole number of four-float registers; what the program gave is the start of it. */
		D3D11_BUFFER_DESC desc = {(constant_size + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
		BYTE *padded = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, desc.ByteWidth);
		D3D11_SUBRESOURCE_DATA data = {padded, 0, 0};
		if (!padded)
			goto done;
		memcpy(padded, constants, constant_size);
		result = ID3D11Device_CreateBuffer(runner.device, &desc, &data, &constant_buffer);
		HeapFree(GetProcessHeap(), 0, padded);
		if (FAILED(result))
			goto done;
		result = E_FAIL;
	}
	if (!runner.target || runner.target_width != width || runner.target_height != height || runner.target_bytes != bytes) {
		if (runner.target_view)
			ID3D11RenderTargetView_Release(runner.target_view);
		if (runner.target)
			ID3D11Texture2D_Release(runner.target);
		if (runner.staging)
			ID3D11Texture2D_Release(runner.staging);
		runner.target = runner.staging = NULL;
		runner.target_view = NULL;
		if (FAILED(ID3D11Device_CreateTexture2D(runner.device, &target_desc, NULL, &runner.target)) ||
			FAILED(ID3D11Device_CreateRenderTargetView(runner.device, (ID3D11Resource *)runner.target, NULL, &runner.target_view)))
			goto done;
		target_desc.Usage = D3D11_USAGE_STAGING;
		target_desc.BindFlags = 0;
		target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(ID3D11Device_CreateTexture2D(runner.device, &target_desc, NULL, &runner.staging)))
			goto done;
		runner.target_width = width;
		runner.target_height = height;
		runner.target_bytes = bytes;
	}
	target = runner.target;
	target_view = runner.target_view;
	staging = runner.staging;
	ID3D11DeviceContext_UpdateSubresource(runner.context, (ID3D11Resource *)runner.area, 0, NULL, area, 0, 0);
	ID3D11DeviceContext_IASetInputLayout(runner.context, NULL);
	ID3D11DeviceContext_IASetPrimitiveTopology(runner.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ID3D11DeviceContext_VSSetShader(runner.context, runner.vertex, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(runner.context, 0, 1, &runner.area);
	ID3D11DeviceContext_PSSetShader(runner.context, shader, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(runner.context, 0, 1, &constant_buffer);
	if (count) {
		ID3D11DeviceContext_PSSetShaderResources(runner.context, 0, count, views);
		ID3D11DeviceContext_PSSetSamplers(runner.context, 0, count, samplers);
	}
	ID3D11DeviceContext_RSSetViewports(runner.context, 1, &viewport);
	ID3D11DeviceContext_OMSetRenderTargets(runner.context, 1, &target_view, NULL);
	ID3D11DeviceContext_Draw(runner.context, 4, 0);
	ID3D11DeviceContext_OMSetRenderTargets(runner.context, 0, NULL, NULL);
	ID3D11DeviceContext_CopyResource(runner.context, (ID3D11Resource *)staging, (ID3D11Resource *)target);
	if (FAILED(ID3D11DeviceContext_Map(runner.context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
		goto done;
	for (row = 0; row < height; ++row)
		memcpy((bytes ? out_bytes : (BYTE *)out) + (size_t)row * width * pixel, (const BYTE *)mapped.pData + (size_t)row * mapped.RowPitch,
			(size_t)width * pixel);
	ID3D11DeviceContext_Unmap(runner.context, (ID3D11Resource *)staging, 0);
	result = S_OK;
done:
	if (runner.context) {
		/* Nothing of this run stays bound to the device. */
		static ID3D11ShaderResourceView *const none[SHADER_INPUTS];
		ID3D11DeviceContext_PSSetShaderResources(runner.context, 0, SHADER_INPUTS, (ID3D11ShaderResourceView *const *)none);
	}
	for (index = 0; index < SHADER_INPUTS; ++index) {
		if (views[index])
			ID3D11ShaderResourceView_Release(views[index]);
		if (textures[index])
			ID3D11Texture2D_Release(textures[index]);
		if (samplers[index])
			ID3D11SamplerState_Release(samplers[index]);
	}
	if (constant_buffer)
		ID3D11Buffer_Release(constant_buffer);
	LeaveCriticalSection(&runner.lock);
	return result;
}

/* ---- drawing into a program's own texture ----
 * A program that hands Direct2D a surface of its swap chain shows what is drawn there each frame. Its pixels stay on the graphics card:
 * the shader runs on the device the texture belongs to and writes into it, with nothing read back. Each such device keeps the stages
 * made for it. */
enum { PAINTERS = 4 };

static const char copy_source[] =
	"Texture2D image : register(t0);\n"
	"SamplerState point_sampler : register(s0);\n"
	"float4 main(float4 position : SV_POSITION, float4 scene : SCENE_POSITION, float4 texel : TEXCOORD0) : SV_TARGET\n"
	"{\n"
	"	return image.Sample(point_sampler, texel.xy);\n"
	"}\n";

static struct painter {
	ID3D11Device *device;
	ID3D11VertexShader *vertex;
	ID3D11PixelShader *copy;
	ID3D11Buffer *area;
	ID3D11BlendState *over;
	ID3D11RasterizerState *whole;
	struct {
		const BYTE *code;
		UINT32 size;
		ID3D11PixelShader *shader;
	} kept[KEPT_SHADERS];
	UINT32 kept_count, kept_next;
} painters[PAINTERS];
static UINT32 painter_next;

static void painter_release(struct painter *painter)
{
	UINT32 index;
	for (index = 0; index < painter->kept_count; ++index)
		if (painter->kept[index].shader)
			ID3D11PixelShader_Release(painter->kept[index].shader);
	if (painter->whole)
		ID3D11RasterizerState_Release(painter->whole);
	if (painter->over)
		ID3D11BlendState_Release(painter->over);
	if (painter->area)
		ID3D11Buffer_Release(painter->area);
	if (painter->copy)
		ID3D11PixelShader_Release(painter->copy);
	if (painter->vertex)
		ID3D11VertexShader_Release(painter->vertex);
	if (painter->device)
		ID3D11Device_Release(painter->device);
	memset(painter, 0, sizeof(*painter));
}

/* The stages a device draws with, made the first time one of its textures is drawn into. */
static struct painter *painter_of(ID3D11Device *device)
{
	static const D3D11_BUFFER_DESC area_desc = {sizeof(float) * 4 * (1 + SHADER_INPUTS), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	D3D11_BLEND_DESC blend;
	D3D11_RASTERIZER_DESC raster;
	compile_function compile;
	HMODULE compiler;
	ID3DBlob *vertex = NULL, *copy = NULL;
	struct painter *painter;
	UINT32 index;
	for (index = 0; index < PAINTERS; ++index)
		if (painters[index].device == device)
			return &painters[index];
	if (!(compiler = LoadLibraryA("d3dcompiler_47.dll")) || !(compile = (compile_function)GetProcAddress(compiler, "D3DCompile")))
		return NULL;
	painter = &painters[painter_next++ % PAINTERS];
	painter_release(painter);
	memset(&blend, 0, sizeof(blend));
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	memset(&raster, 0, sizeof(raster));
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;
	if (SUCCEEDED(compile(vertex_source, sizeof(vertex_source) - 1, "effects", NULL, NULL, "main", "vs_4_0", 0, 0, &vertex, NULL)) &&
		SUCCEEDED(compile(copy_source, sizeof(copy_source) - 1, "copy", NULL, NULL, "main", "ps_4_0", 0, 0, &copy, NULL)) &&
		SUCCEEDED(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vertex), ID3D10Blob_GetBufferSize(vertex), NULL,
			&painter->vertex)) &&
		SUCCEEDED(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(copy), ID3D10Blob_GetBufferSize(copy), NULL,
			&painter->copy)) &&
		SUCCEEDED(ID3D11Device_CreateBuffer(device, &area_desc, NULL, &painter->area)) &&
		SUCCEEDED(ID3D11Device_CreateBlendState(device, &blend, &painter->over)) &&
		SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &raster, &painter->whole))) {
		painter->device = device;
		ID3D11Device_AddRef(device);
	} else {
		painter_release(painter);
		painter = NULL;
	}
	if (vertex)
		ID3D10Blob_Release(vertex);
	if (copy)
		ID3D10Blob_Release(copy);
	return painter;
}

static ID3D11PixelShader *painter_shader(struct painter *painter, const BYTE *code, UINT32 size)
{
	ID3D11PixelShader *shader = NULL;
	UINT32 index;
	if (!code)
		return painter->copy;
	for (index = 0; index < painter->kept_count; ++index)
		if (painter->kept[index].code == code && painter->kept[index].size == size)
			return painter->kept[index].shader;
	if (FAILED(ID3D11Device_CreatePixelShader(painter->device, code, size, NULL, &shader)))
		return NULL;
	index = painter->kept_count < KEPT_SHADERS ? painter->kept_count++ : painter->kept_next++ % KEPT_SHADERS;
	if (painter->kept[index].shader)
		ID3D11PixelShader_Release(painter->kept[index].shader);
	painter->kept[index].code = code;
	painter->kept[index].size = size;
	painter->kept[index].shader = shader;
	return shader;
}

/* What the program's context was set to, in the stages a drawing here sets, taken before it and put back after. */
struct context_state {
	ID3D11InputLayout *layout;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	ID3D11VertexShader *vertex;
	ID3D11PixelShader *pixel;
	ID3D11Buffer *vertex_constants, *pixel_constants;
	ID3D11ShaderResourceView *views[SHADER_INPUTS];
	ID3D11SamplerState *samplers[SHADER_INPUTS];
	ID3D11RasterizerState *raster;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT viewport_count;
	ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth;
	ID3D11BlendState *blend;
	FLOAT blend_factor[4];
	UINT sample_mask;
};

static void state_take(ID3D11DeviceContext *context, struct context_state *state)
{
	memset(state, 0, sizeof(*state));
	ID3D11DeviceContext_IAGetInputLayout(context, &state->layout);
	ID3D11DeviceContext_IAGetPrimitiveTopology(context, &state->topology);
	ID3D11DeviceContext_VSGetShader(context, &state->vertex, NULL, NULL);
	ID3D11DeviceContext_PSGetShader(context, &state->pixel, NULL, NULL);
	ID3D11DeviceContext_VSGetConstantBuffers(context, 0, 1, &state->vertex_constants);
	ID3D11DeviceContext_PSGetConstantBuffers(context, 0, 1, &state->pixel_constants);
	ID3D11DeviceContext_PSGetShaderResources(context, 0, SHADER_INPUTS, state->views);
	ID3D11DeviceContext_PSGetSamplers(context, 0, SHADER_INPUTS, state->samplers);
	ID3D11DeviceContext_RSGetState(context, &state->raster);
	state->viewport_count = ARRAYSIZE(state->viewports);
	ID3D11DeviceContext_RSGetViewports(context, &state->viewport_count, state->viewports);
	ID3D11DeviceContext_OMGetRenderTargets(context, ARRAYSIZE(state->targets), state->targets, &state->depth);
	ID3D11DeviceContext_OMGetBlendState(context, &state->blend, state->blend_factor, &state->sample_mask);
}

static void state_put(ID3D11DeviceContext *context, struct context_state *state)
{
	UINT32 index;
	ID3D11DeviceContext_IASetInputLayout(context, state->layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(context, state->topology);
	ID3D11DeviceContext_VSSetShader(context, state->vertex, NULL, 0);
	ID3D11DeviceContext_PSSetShader(context, state->pixel, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &state->vertex_constants);
	ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &state->pixel_constants);
	ID3D11DeviceContext_PSSetShaderResources(context, 0, SHADER_INPUTS, state->views);
	ID3D11DeviceContext_PSSetSamplers(context, 0, SHADER_INPUTS, state->samplers);
	ID3D11DeviceContext_RSSetState(context, state->raster);
	if (state->viewport_count)
		ID3D11DeviceContext_RSSetViewports(context, state->viewport_count, state->viewports);
	ID3D11DeviceContext_OMSetRenderTargets(context, ARRAYSIZE(state->targets), state->targets, state->depth);
	ID3D11DeviceContext_OMSetBlendState(context, state->blend, state->blend_factor, state->sample_mask);
	if (state->layout)
		ID3D11InputLayout_Release(state->layout);
	if (state->vertex)
		ID3D11VertexShader_Release(state->vertex);
	if (state->pixel)
		ID3D11PixelShader_Release(state->pixel);
	if (state->vertex_constants)
		ID3D11Buffer_Release(state->vertex_constants);
	if (state->pixel_constants)
		ID3D11Buffer_Release(state->pixel_constants);
	for (index = 0; index < SHADER_INPUTS; ++index) {
		if (state->views[index])
			ID3D11ShaderResourceView_Release(state->views[index]);
		if (state->samplers[index])
			ID3D11SamplerState_Release(state->samplers[index]);
	}
	if (state->raster)
		ID3D11RasterizerState_Release(state->raster);
	for (index = 0; index < ARRAYSIZE(state->targets); ++index)
		if (state->targets[index])
			ID3D11RenderTargetView_Release(state->targets[index]);
	if (state->depth)
		ID3D11DepthStencilView_Release(state->depth);
	if (state->blend)
		ID3D11BlendState_Release(state->blend);
}

HRESULT shader_draw(void *target, const BYTE *code, UINT32 size, const BYTE *constants, UINT32 constant_size,
	const struct shader_image *inputs, UINT32 count, INT32 left, INT32 top, UINT32 width, UINT32 height, INT32 x, INT32 y)
{
	static const float no_factor[4];
	ID3D11Texture2D *texture = target;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	ID3D11Texture2D *textures[SHADER_INPUTS] = {0};
	ID3D11ShaderResourceView *views[SHADER_INPUTS] = {0};
	ID3D11SamplerState *samplers[SHADER_INPUTS] = {0};
	ID3D11RenderTargetView *view = NULL;
	ID3D11Buffer *constant_buffer = NULL;
	ID3D11PixelShader *shader;
	struct painter *painter;
	struct context_state state;
	D3D11_VIEWPORT viewport = {(float)x, (float)y, (float)width, (float)height, 0, 1};
	float area[4 * (1 + SHADER_INPUTS)] = {(float)left, (float)top, (float)width, (float)height};
	HRESULT result = E_FAIL;
	UINT32 index;
	if (!texture || !width || !height || count > SHADER_INPUTS || (!code && !count))
		return E_INVALIDARG;
	InitOnceExecuteOnce(&runner.once, runner_init, NULL, NULL);
	EnterCriticalSection(&runner.lock);
	ID3D11Texture2D_GetDevice(texture, &device);
	if (!device || !(painter = painter_of(device)) || !(shader = painter_shader(painter, code, size)) ||
		FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &view)))
		goto done;
	for (index = 0; index < count; ++index) {
		D3D11_SAMPLER_DESC sampler = {(D3D11_FILTER)inputs[index].filter, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_BORDER,
			D3D11_TEXTURE_ADDRESS_BORDER, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
		const BOOL empty = (!inputs[index].pixels && !inputs[index].bytes) || !inputs[index].width || !inputs[index].height;
		const BOOL bytes = !empty && !inputs[index].pixels;
		static const float clear[4];
		D3D11_TEXTURE2D_DESC desc = {empty ? 1 : inputs[index].width, empty ? 1 : inputs[index].height, 1, 1,
			bytes ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
		D3D11_SUBRESOURCE_DATA data = {empty ? (const void *)clear : bytes ? (const void *)inputs[index].bytes : (const void *)inputs[index].pixels,
			desc.Width * (bytes ? 4 : sizeof(float) * 4), 0};
		if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, &data, &textures[index])) ||
			FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)textures[index], NULL, &views[index])) ||
			FAILED(ID3D11Device_CreateSamplerState(device, &sampler, &samplers[index])))
			goto done;
		area[4 * (1 + index)] = (float)inputs[index].left;
		area[4 * (1 + index) + 1] = (float)inputs[index].top;
		area[4 * (1 + index) + 2] = 1.0f / (float)(inputs[index].width ? inputs[index].width : 1);
		area[4 * (1 + index) + 3] = 1.0f / (float)(inputs[index].height ? inputs[index].height : 1);
	}
	if (constant_size) {
		D3D11_BUFFER_DESC desc = {(constant_size + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
		BYTE *padded = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, desc.ByteWidth);
		D3D11_SUBRESOURCE_DATA data = {padded, 0, 0};
		HRESULT made;
		if (!padded)
			goto done;
		memcpy(padded, constants, constant_size);
		made = ID3D11Device_CreateBuffer(device, &desc, &data, &constant_buffer);
		HeapFree(GetProcessHeap(), 0, padded);
		if (FAILED(made))
			goto done;
	}
	ID3D11Device_GetImmediateContext(device, &context);
	if (!context)
		goto done;
	state_take(context, &state);
	ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)painter->area, 0, NULL, area, 0, 0);
	ID3D11DeviceContext_IASetInputLayout(context, NULL);
	ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ID3D11DeviceContext_VSSetShader(context, painter->vertex, NULL, 0);
	ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &painter->area);
	ID3D11DeviceContext_PSSetShader(context, shader, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &constant_buffer);
	if (count) {
		ID3D11DeviceContext_PSSetShaderResources(context, 0, count, views);
		ID3D11DeviceContext_PSSetSamplers(context, 0, count, samplers);
	}
	ID3D11DeviceContext_RSSetState(context, painter->whole);
	ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
	ID3D11DeviceContext_OMSetRenderTargets(context, 1, &view, NULL);
	ID3D11DeviceContext_OMSetBlendState(context, painter->over, no_factor, 0xffffffffU);
	ID3D11DeviceContext_Draw(context, 4, 0);
	state_put(context, &state);
	result = S_OK;
done:
	for (index = 0; index < SHADER_INPUTS; ++index) {
		if (views[index])
			ID3D11ShaderResourceView_Release(views[index]);
		if (textures[index])
			ID3D11Texture2D_Release(textures[index]);
		if (samplers[index])
			ID3D11SamplerState_Release(samplers[index]);
	}
	if (constant_buffer)
		ID3D11Buffer_Release(constant_buffer);
	if (view)
		ID3D11RenderTargetView_Release(view);
	if (context)
		ID3D11DeviceContext_Release(context);
	if (device)
		ID3D11Device_Release(device);
	LeaveCriticalSection(&runner.lock);
	return result;
}
