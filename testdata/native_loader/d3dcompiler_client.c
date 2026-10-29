#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

/* The MinGW reflection header declares the interfaces without their C call macros. */
#define CALL(object, method, ...) ((object)->lpVtbl->method((object), ##__VA_ARGS__))
#define ID3D11ShaderReflection_GetDesc(p, ...) CALL(p, GetDesc, __VA_ARGS__)
#define ID3D11ShaderReflection_GetConstantBufferByName(p, ...) CALL(p, GetConstantBufferByName, __VA_ARGS__)
#define ID3D11ShaderReflection_GetVariableByName(p, ...) CALL(p, GetVariableByName, __VA_ARGS__)
#define ID3D11ShaderReflection_GetInputParameterDesc(p, ...) CALL(p, GetInputParameterDesc, __VA_ARGS__)
#define ID3D11ShaderReflection_GetOutputParameterDesc(p, ...) CALL(p, GetOutputParameterDesc, __VA_ARGS__)
#define ID3D11ShaderReflection_GetThreadGroupSize(p, ...) CALL(p, GetThreadGroupSize, __VA_ARGS__)
#define ID3D11ShaderReflection_Release(p) CALL(p, Release)
#define ID3D11ShaderReflectionConstantBuffer_GetDesc(p, ...) CALL(p, GetDesc, __VA_ARGS__)
#define ID3D11ShaderReflectionConstantBuffer_GetVariableByIndex(p, ...) CALL(p, GetVariableByIndex, __VA_ARGS__)
#define ID3D11ShaderReflectionVariable_GetDesc(p, ...) CALL(p, GetDesc, __VA_ARGS__)
#define ID3D11ShaderReflectionVariable_GetType(p) CALL(p, GetType)
#define ID3D11ShaderReflectionType_GetDesc(p, ...) CALL(p, GetDesc, __VA_ARGS__)
#define VERTEX_SHADER_TYPE 1

static int same(const char *left, const char *right)
{
	while (*left && *left == *right) ++left, ++right;
	return *left == *right;
}

static int contains(const char *text, const char *part)
{
	for (; *text; ++text) {
		const char *a = text, *b = part;
		while (*a && *b && *a == *b) ++a, ++b;
		if (!*b)
			return 1;
	}
	return 0;
}

/* The program's own include handler, serving one header from memory. */
static const char header[] = "#define SCALE 2.0\nfloat4 scaled(float4 v) { return v * SCALE; }\n";
static int opened, closed;

static HRESULT STDMETHODCALLTYPE include_open(ID3DInclude *self, D3D_INCLUDE_TYPE type, const char *name, const void *parent, const void **data, UINT *bytes)
{
	if (type != D3D_INCLUDE_LOCAL || !same(name, "common.hlsli"))
		return E_FAIL;
	++opened;
	*data = header;
	*bytes = sizeof(header) - 1;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE include_close(ID3DInclude *self, const void *data)
{
	++closed;
	return S_OK;
}

static ID3DIncludeVtbl include_vtbl = {include_open, include_close};
static ID3DInclude include = {&include_vtbl};

static const char vertex[] =
	"#include \"common.hlsli\"\n"
	"cbuffer Frame : register(b0) { float4x4 world; float4 tint; };\n"
	"struct Out { float4 position : SV_Position; float4 color : COLOR0; };\n"
	"Out main(float4 position : POSITION, float4 color : COLOR0)\n"
	"{ Out o; o.position = mul(scaled(position), world); o.color = color * tint; return o; }\n";

static const char broken[] = "float4 main() : SV_Target { return undefined_name; }\n";

static const char compute[] =
	"RWStructuredBuffer<uint> output : register(u0);\n"
	"[numthreads(8, 4, 1)] void main(uint3 id : SV_DispatchThreadID) { output[id.y * 8 + id.x] = id.x + VALUE; }\n";

int raven_entry(void)
{
	ID3DBlob *code = NULL, *errors = NULL, *part = NULL;
	ID3D11ShaderReflection *reflector = NULL;
	ID3D11ShaderReflectionConstantBuffer *buffer;
	ID3D11ShaderReflectionVariable *variable;
	ID3D11ShaderReflectionType *type;
	D3D11_SHADER_DESC desc;
	D3D11_SHADER_BUFFER_DESC buffer_desc;
	D3D11_SHADER_VARIABLE_DESC variable_desc;
	D3D11_SHADER_TYPE_DESC type_desc;
	D3D11_SIGNATURE_PARAMETER_DESC parameter;
	D3D_SHADER_MACRO macros[] = {{"VALUE", "7"}, {NULL, NULL}};
	UINT x = 0, y = 0, z = 0;
	const unsigned char *bytes;

	/* A vertex shader through the program's include handler, then its reflection. */
	if (D3DCompile(vertex, sizeof(vertex) - 1, "vertex.hlsl", NULL, &include, "main", "vs_4_0", 0, 0, &code, &errors) != S_OK || !code)
		return 1;
	if (opened != 1 || closed != 1)
		return 2;
	bytes = ID3D10Blob_GetBufferPointer(code);
	if (ID3D10Blob_GetBufferSize(code) < 32 || bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C')
		return 3;
	if (D3DReflect(bytes, ID3D10Blob_GetBufferSize(code), &IID_ID3D11ShaderReflection, (void **)&reflector) != S_OK)
		return 4;
	if (ID3D11ShaderReflection_GetDesc(reflector, &desc) != S_OK || desc.Version >> 16 != VERTEX_SHADER_TYPE || (desc.Version >> 4 & 15) != 4)
		return 5;
	if (desc.ConstantBuffers != 1 || desc.InputParameters != 2 || desc.OutputParameters != 2 || !desc.InstructionCount || !desc.Creator)
		return 6;
	buffer = ID3D11ShaderReflection_GetConstantBufferByName(reflector, "Frame");
	if (ID3D11ShaderReflectionConstantBuffer_GetDesc(buffer, &buffer_desc) != S_OK || buffer_desc.Variables != 2 || buffer_desc.Size != 80)
		return 7;
	variable = ID3D11ShaderReflection_GetVariableByName(reflector, "tint");
	if (ID3D11ShaderReflectionVariable_GetDesc(variable, &variable_desc) != S_OK || variable_desc.StartOffset != 64 || variable_desc.Size != 16)
		return 8;
	type = ID3D11ShaderReflectionVariable_GetType(ID3D11ShaderReflectionConstantBuffer_GetVariableByIndex(buffer, 0));
	if (ID3D11ShaderReflectionType_GetDesc(type, &type_desc) != S_OK || type_desc.Class != D3D_SVC_MATRIX_COLUMNS || type_desc.Rows != 4 ||
		type_desc.Columns != 4 || type_desc.Type != D3D_SVT_FLOAT)
		return 9;
	if (ID3D11ShaderReflection_GetInputParameterDesc(reflector, 1, &parameter) != S_OK || !same(parameter.SemanticName, "COLOR") ||
		parameter.Register != 1)
		return 10;
	if (ID3D11ShaderReflection_GetOutputParameterDesc(reflector, 0, &parameter) != S_OK || parameter.SystemValueType != D3D_NAME_POSITION)
		return 11;
	/* A lookup that finds nothing still returns an object, whose description fails. */
	if (ID3D11ShaderReflectionVariable_GetDesc(ID3D11ShaderReflection_GetVariableByName(reflector, "missing"), &variable_desc) == S_OK)
		return 12;
	ID3D11ShaderReflection_Release(reflector);

	/* Parts of the container: a signature, a stripped copy and a listing. */
	if (D3DGetInputSignatureBlob(bytes, ID3D10Blob_GetBufferSize(code), &part) != S_OK || ((unsigned char *)ID3D10Blob_GetBufferPointer(part))[0] != 'D')
		return 13;
	ID3D10Blob_Release(part);
	if (D3DStripShader(bytes, ID3D10Blob_GetBufferSize(code), D3DCOMPILER_STRIP_REFLECTION_DATA, &part) != S_OK ||
		ID3D10Blob_GetBufferSize(part) >= ID3D10Blob_GetBufferSize(code))
		return 14;
	ID3D10Blob_Release(part);
	if (D3DGetBlobPart(bytes, ID3D10Blob_GetBufferSize(code), D3D_BLOB_PRIVATE_DATA, 0, &part) == S_OK)
		return 15;
	if (D3DDisassemble(bytes, ID3D10Blob_GetBufferSize(code), 0, NULL, &part) != S_OK || !contains(ID3D10Blob_GetBufferPointer(part), "vs_4_0"))
		return 16;
	ID3D10Blob_Release(part);
	ID3D10Blob_Release(code);
	if (errors)
		ID3D10Blob_Release(errors);

	/* A failing compile gives no code and a message naming the file. */
	code = errors = NULL;
	if (D3DCompile(broken, sizeof(broken) - 1, "broken.hlsl", NULL, NULL, "main", "ps_4_0", 0, 0, &code, &errors) != E_FAIL || code || !errors)
		return 17;
	if (!contains(ID3D10Blob_GetBufferPointer(errors), "broken.hlsl(") || !contains(ID3D10Blob_GetBufferPointer(errors), "error"))
		return 18;
	ID3D10Blob_Release(errors);
	/* An include with no handler fails the same way. */
	code = errors = NULL;
	if (D3DCompile(vertex, sizeof(vertex) - 1, "vertex.hlsl", NULL, NULL, "main", "vs_4_0", 0, 0, &code, &errors) != E_FAIL || code || !errors ||
		!contains(ID3D10Blob_GetBufferPointer(errors), "common.hlsli"))
		return 19;
	ID3D10Blob_Release(errors);

	/* Macros, preprocessing and a compute shader's thread group. */
	if (D3DPreprocess(compute, sizeof(compute) - 1, "compute.hlsl", macros, NULL, &part, NULL) != S_OK ||
		!contains(ID3D10Blob_GetBufferPointer(part), "+ 7") || contains(ID3D10Blob_GetBufferPointer(part), "VALUE"))
		return 20;
	ID3D10Blob_Release(part);
	if (D3DCompile(compute, sizeof(compute) - 1, NULL, macros, NULL, "main", "cs_5_0", 0, 0, &code, NULL) != S_OK)
		return 21;
	if (D3DReflect(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &IID_ID3D11ShaderReflection, (void **)&reflector) != S_OK)
		return 22;
	if (ID3D11ShaderReflection_GetThreadGroupSize(reflector, &x, &y, &z) != 32 || x != 8 || y != 4 || z != 1)
		return 23;
	ID3D11ShaderReflection_GetDesc(reflector, &desc);
	if (desc.BoundResources != 1)
		return 24;
	ID3D11ShaderReflection_Release(reflector);
	ID3D10Blob_Release(code);

	/* Blobs made by the program. */
	if (D3DCreateBlob(12, &part) != S_OK || ID3D10Blob_GetBufferSize(part) != 12)
		return 25;
	ID3D10Blob_Release(part);
	return 0;
}
