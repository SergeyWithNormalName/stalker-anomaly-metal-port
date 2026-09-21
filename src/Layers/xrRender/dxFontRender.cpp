#include "stdafx.h"
#include "dxFontRender.h"

#include "dxUIRender.h"

#include "../../xrEngine/GameFont.h"
#include "../../Include/xrRender/R4BackendSelection.h"

dxFontRender::dxFontRender()
{
}

dxFontRender::~dxFontRender()
{
	pShader.destroy();
	pGeom.destroy();
}

void dxFontRender::Initialize(LPCSTR cShader, LPCSTR cTexture)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		m_metalShader.shader = cShader ? cShader : "";
		m_metalShader.texture = cTexture ? cTexture : "";
		Msg("* METAL_UI_CAPTURE font_init shader=%s texture=%s", m_metalShader.shader.c_str(), m_metalShader.texture.c_str());
		return;
	}
	pShader.create(cShader, cTexture);
	pGeom.create(FVF::F_TL, RCache.Vertex.Buffer(), RCache.QuadIB);
}

extern ENGINE_API xr_atomic_bool g_bRendering;
extern ENGINE_API Fvector2 g_current_font_scale;

void dxFontRender::OnRender(CGameFont& owner)
{
	VERIFY(g_bRendering);
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		if (!(owner.uFlags & CGameFont::fsValid))
		{
			// The texture dimensions are intentionally deferred to the future Metal texture consumer.
			// Capture atlas UVs in source pixels so no D3D texture lookup is needed here.
			owner.vTS.set(1, 1);
			owner.fTCHeight = owner.fHeight;
			owner.uFlags |= CGameFont::fsValid;
		}

		for (u32 i = 0; i < owner.strings.size();)
		{
			int count = 1;
			int length = owner.smart_strlen(owner.strings[i].string);
			while ((i + count) < owner.strings.size())
			{
				const int nextLength = owner.smart_strlen(owner.strings[i + count].string);
				if ((nextLength + length) >= MAX_MB_CHARS)
					break;
				++count;
				length += nextLength;
			}

			std::vector<xray::metal::ui::Vertex> vertices;
			std::vector<std::uint32_t> indices;
			vertices.reserve(static_cast<std::size_t>(length) * 4u);
			indices.reserve(static_cast<std::size_t>(length) * 6u);
			const u32 last = i + count;
			for (; i < last; ++i)
			{
				CGameFont::String& string = owner.strings[i];
				wide_char wideString[MAX_MB_CHARS];
				const int len = owner.IsMultibyte() ? mbhMulti2Wide(wideString, NULL, MAX_MB_CHARS, string.string) : xr_strlen(string.string);
				if (!len)
					continue;

				float x = float(iFloor(string.x));
				float y = float(iFloor(string.y));
				const float glyphHeight = string.height * g_current_font_scale.y;
				float y2 = y + glyphHeight;
				float textWidth = 0.0f;
				const float heightScale = fis_zero(owner.fHeight) ? 1.0f : (string.height / owner.fHeight);
				if (string.align)
				{
					textWidth = owner.IsMultibyte() ? owner.SizeOf_(wideString) : owner.SizeOf_(string.string);
					if (!fis_zero(owner.GetHeight()))
						textWidth *= string.height / owner.GetHeight();
				}
				switch (string.align)
				{
				case CGameFont::alCenter:
					x -= iFloor(textWidth * 0.5f) * g_current_font_scale.x;
					break;
				case CGameFont::alRight:
					x -= iFloor(textWidth);
					break;
				default:
					break;
				}

				u32 bottomColor = string.c;
				u32 topColor = bottomColor;
				if (owner.uFlags & CGameFont::fsGradient)
					topColor = color_rgba(color_get_R(bottomColor) / 2, color_get_G(bottomColor) / 2,
						color_get_B(bottomColor) / 2, color_get_A(bottomColor));

#if defined(USE_DX10) || defined(USE_DX11)
				x -= 0.5f;
				y -= 0.5f;
				y2 -= 0.5f;
#endif

				for (int character = 0; character < len; ++character)
				{
					const Fvector glyph = owner.IsMultibyte() ? owner.GetCharTC(wideString[1 + character]) :
						owner.GetCharTC((u16)(u8)string.string[character]);
					const float glyphWidth = glyph.z * heightScale * g_current_font_scale.x;
					if (!fis_zero(glyph.z))
					{
						const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
						vertices.push_back({x, y2, .0001f, topColor, glyph.x, glyph.y + owner.fHeight});
						vertices.push_back({x, y, .0001f, bottomColor, glyph.x, glyph.y});
						vertices.push_back({x + glyphWidth, y2, .0001f, topColor, glyph.x + glyph.z, glyph.y + owner.fHeight});
						vertices.push_back({x + glyphWidth, y, .0001f, bottomColor, glyph.x + glyph.z, glyph.y});
						indices.insert(indices.end(), {base, base + 1u, base + 2u, base + 3u, base + 2u, base + 1u});
					}
					x += glyphWidth * owner.vInterval.x;
					if (owner.IsMultibyte())
					{
						x -= 2.0f;
						if (IsNeedSpaceCharacter(wideString[1 + character]))
							x += owner.fXStep;
					}
				}
			}

			if (!vertices.empty())
			{
				xray::metal::ui::DrawRecord record;
				record.shader = m_metalShader;
				record.primitive = xray::metal::ui::PrimitiveType::TriangleList;
				record.pointType = xray::metal::ui::PointType::TL;
				record.textureCoordinateSpace = xray::metal::ui::TextureCoordinateSpace::AtlasPixels;
				record.scissor = UIRenderImpl.MetalScissorSnapshot();
				record.alphaRef = 0;
				record.cullMode = UIRenderImpl.MetalCullMode();
				record.font = true;
				xray::metal::ui::AppendDraw(std::move(record), vertices, indices);
			}
		}
		return;
	}
	if (pShader) RCache.set_Shader(pShader);

	if (!(owner.uFlags & CGameFont::fsValid))
	{
		CTexture* T = RCache.get_ActiveTexture(0);
		owner.vTS.set((int)T->get_Width(), (int)T->get_Height());
		owner.fTCHeight = owner.fHeight / float(owner.vTS.y);
		owner.uFlags |= CGameFont::fsValid;
	}

	for (u32 i = 0; i < owner.strings.size();)
	{
		// calculate first-fit
		int count = 1;

		int length = owner.smart_strlen(owner.strings[i].string);

		while ((i + count) < owner.strings.size())
		{
			int L = owner.smart_strlen(owner.strings[i + count].string);

			if ((L + length) < MAX_MB_CHARS)
			{
				count ++;
				length += L;
			}
			else break;
		}

		// lock AGP memory
		u32 vOffset;
		FVF::TL* v = (FVF::TL*)RCache.Vertex.Lock(length * 4, pGeom.stride(), vOffset);
		FVF::TL* start = v;

		// fill vertices
		u32 last = i + count;
		for (; i < last; i++)
		{
			CGameFont::String& PS = owner.strings[i];
			wide_char wsStr[ MAX_MB_CHARS ];

			int len = owner.IsMultibyte() ? mbhMulti2Wide(wsStr, NULL, MAX_MB_CHARS, PS.string) : xr_strlen(PS.string);

			if (len)
			{
				float X = float(iFloor(PS.x));
				float Y = float(iFloor(PS.y));
				float S = PS.height * g_current_font_scale.y;
				float Y2 = Y + S;
				float fSize = 0;

				const float hs = fis_zero(owner.fHeight) ? 1.0f : (PS.height / owner.fHeight);

				if (PS.align)
				{
					fSize = owner.IsMultibyte() ? owner.SizeOf_(wsStr) : owner.SizeOf_(PS.string);
					if (!fis_zero(owner.GetHeight()))
						fSize *= PS.height / owner.GetHeight();
				}

				switch (PS.align)
				{
				case CGameFont::alCenter:
					X -= (iFloor(fSize * 0.5f)) * g_current_font_scale.x;
					break;
				case CGameFont::alRight:
					X -= iFloor(fSize);
					break;
				}

				u32 clr, clr2;
				clr2 = clr = PS.c;
				if (owner.uFlags & CGameFont::fsGradient)
				{
					u32 _R = color_get_R(clr) / 2;
					u32 _G = color_get_G(clr) / 2;
					u32 _B = color_get_B(clr) / 2;
					u32 _A = color_get_A(clr);
					clr2 = color_rgba(_R, _G, _B, _A);
				}

#if defined(USE_DX10) || defined(USE_DX11)		//	Vertex shader will cancel a DX9 correction, so make fake offset
				X -= 0.5f;
				Y -= 0.5f;
				Y2 -= 0.5f;
#endif	//	USE_DX10

				float tu, tv;
				for (int j = 0; j < len; j++)
				{
					Fvector l;

					l = owner.IsMultibyte() ? owner.GetCharTC(wsStr[1 + j]) : owner.GetCharTC((u16)(u8)PS.string[j]);

					float scw = l.z * hs * g_current_font_scale.x;

					float fTCWidth = l.z / owner.vTS.x;

					if (!fis_zero(l.z))
					{
						//						tu			= ( l.x / owner.vTS.x ) + ( 0.5f / owner.vTS.x );
						//						tv			= ( l.y / owner.vTS.y ) + ( 0.5f / owner.vTS.y );
						tu = (l.x / owner.vTS.x);
						tv = (l.y / owner.vTS.y);
#if !defined(USE_DX10) && !defined(USE_DX11)
						//	Make half pixel offset for 1 to 1 mapping
						tu += (0.5f / owner.vTS.x);
						tv += (0.5f / owner.vTS.y);
#endif

						v->set(X, Y2, clr2, tu, tv + owner.fTCHeight);
						v++;
						v->set(X, Y, clr, tu, tv);
						v++;
						v->set(X + scw, Y2, clr2, tu + fTCWidth, tv + owner.fTCHeight);
						v++;
						v->set(X + scw, Y, clr, tu + fTCWidth, tv);
						v++;
					}
					X += scw * owner.vInterval.x;
					if (owner.IsMultibyte())
					{
						X -= 2;
						if (IsNeedSpaceCharacter(wsStr[1 + j]))
							X += owner.fXStep;
					}
				}
			}
		}

		// Unlock and draw
		u32 vCount = (u32)(v - start);
		RCache.Vertex.Unlock(vCount, pGeom.stride());
		if (vCount)
		{
			RCache.set_Geometry(pGeom);
			RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vCount, 0, vCount / 2);
		}
	}
}
