#include <d3dx9mesh.h>

#include <cstdio>

namespace
{
bool expect_element(const D3DVERTEXELEMENT9& actual, WORD offset, BYTE type, BYTE usage, BYTE usage_index)
{
	return actual.Stream == 0 && actual.Offset == offset && actual.Type == type
		&& actual.Method == D3DDECLMETHOD_DEFAULT && actual.Usage == usage
		&& actual.UsageIndex == usage_index;
}

bool check_skybox_declaration()
{
	const DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2
		| D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE3(1);
	const D3DVERTEXELEMENT9 end = D3DDECL_END();
	D3DVERTEXELEMENT9 declaration[MAX_FVF_DECL_SIZE] = {};
	if (FAILED(D3DXDeclaratorFromFVF(fvf, declaration)))
		return false;

	return expect_element(declaration[0], 0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0)
		&& expect_element(declaration[1], 12, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0)
		&& expect_element(declaration[2], 16, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, 0)
		&& expect_element(declaration[3], 28, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, 1)
		&& declaration[4].Stream == end.Stream
		&& D3DXGetDeclVertexSize(declaration, 0) == 40;
}

bool check_other_texcoord_sizes()
{
	D3DVERTEXELEMENT9 declaration[MAX_FVF_DECL_SIZE] = {};
	if (FAILED(D3DXDeclaratorFromFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE1(0), declaration)))
		return false;
	if (!expect_element(declaration[1], 12, D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, 0)
		|| D3DXGetDeclVertexSize(declaration, 0) != 16)
		return false;

	if (FAILED(D3DXDeclaratorFromFVF(D3DFVF_XYZ | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0), declaration)))
		return false;
	return expect_element(declaration[1], 12, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, 0)
		&& D3DXGetDeclVertexSize(declaration, 0) == 28;
}
}

int main()
{
	if (!check_skybox_declaration() || !check_other_texcoord_sizes())
	{
		std::fputs("D3DX FVF declaration conversion failed\n", stderr);
		return 1;
	}

	std::puts("D3DX FVF declaration conversion passed");
	return 0;
}
