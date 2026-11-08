#include "text.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <initializer_list>
#include <stdexcept>

namespace raven {

static uint32_t field(std::span<const uint8_t> bytes,size_t at,size_t count=4)
{
	uint32_t value=0;
	for (size_t i=0;i<count;++i) value|=uint32_t(bytes[at+i])<<(i*8);
	return value;
}

struct TextRenderer::State {
	std::array<sk_sp<SkTypeface>,3> faces;
	// Every bundled style: family (sans, mono, serif, then the dialog face) by regular, bold, italic, bold italic.
	std::array<std::array<sk_sp<SkTypeface>,4>,4> styled;
	// Optional host faces that supply glyphs the bundled family lacks; only the general (non-qualified) text path uses them.
	std::vector<sk_sp<SkTypeface>> fallbacks;

	static bool contains_any(const std::string &name,std::initializer_list<const char *> words)
	{
		for (const auto *word : words)
			if (name.find(word)!=name.npos) return true;
		return false;
	}

	// Maps any logical font onto the bundled families and style, sizing it the way GDI does.
	SkFont logical(std::span<const uint8_t,92> definition)
	{
		const auto height=int32_t(field(definition,0)),weight=int32_t(field(definition,16));
		const bool italic=definition[20]!=0;
		const uint8_t quality=definition[26],pitch=definition[27];
		std::string name;
		for (size_t i=0;i<32;++i) {
			const auto c=field(definition,28+i*2,2); if (!c) break;
			name.push_back(c<128 ? char(c>='A' && c<='Z' ? c+32 : c) : '?');
		}
		size_t family=0;
		if (contains_any(name,{"courier","consolas","mono","fixed","lucida console","terminal","menlo","monaco","typewriter"}) || ((pitch&0x30)==0x30 && !contains_any(name,{"sans","arial","tahoma","segoe"})))
			family=1;
		else if (contains_any(name,{"times","serif","georgia","cambria","garamond","palatino","book antiqua","century"}) && !contains_any(name,{"sans"}))
			family=2;
		const size_t style=(weight>=600 ? 1U : 0U)|(italic ? 2U : 0U);
		// Dialogs laid out for Tahoma get Raven Sans, which has its widths and heights, when it is bundled. Like Tahoma it has
		// no italic, so an italic request keeps the upright face.
		sk_sp<SkTypeface> face;
		if (name=="tahoma" || name=="ms shell dlg 2" || name=="raven sans")
			face=styled[3][style&1U];
		if (!face) face=styled[family][style];
		if (!face) face=styled[family][0];
		if (!face) face=faces[family];
		float size=height<0 ? float(-height) : height>0 ? float(height) : 12.0f;
		SkFont probe(face,100.0f);
		SkFontMetrics source; probe.getMetrics(&source);
		if (height>0) {
			// A positive height is the whole cell (ascent plus descent); scale so the rounded cell matches it.
			const float cell=(-source.fAscent+source.fDescent)/100.0f;
			size=float(height)/cell;
			SkFont trial(face,size); SkFontMetrics measured; trial.getMetrics(&measured);
			const float adjusted=float(height)/std::max(1.0f,float(std::lround(-measured.fAscent)+std::lround(measured.fDescent)))*size;
			if (std::fabs(adjusted-size)<size*0.15f) size=adjusted;
		}
		SkFont result(face,std::max(size,1.0f));
		result.setHinting(SkFontHinting::kFull); result.setSubpixel(false); result.setLinearMetrics(false);
		result.setEdging(quality==3 ? SkFont::Edging::kAlias : SkFont::Edging::kAntiAlias);
		return result;
	}

	std::pair<SkFont,size_t> font(std::span<const uint8_t,92> definition)
	{
		const auto height=int32_t(field(definition,0));
		if (height>=0 || height < -256 || field(definition,4) || field(definition,8) || field(definition,12) ||
			(field(definition,16)!=400 && field(definition,16)!=0) || definition[20] || definition[21] || definition[22] ||
			definition[23] || definition[24] || definition[25] || definition[26]!=3 || definition[27]) throw uint32_t(120);
		std::string name;
		for (size_t i=0;i<32;++i) {
			const auto c=field(definition,28+i*2,2); if (!c) break;
			if (c>127) throw uint32_t(120);
			name.push_back(char(c>='A' && c<='Z' ? c+32 : c));
		}
		size_t family;
		if (name=="liberation sans" || name=="arial") family=0;
		else if (name=="liberation mono" || name=="courier new") family=1;
		else if (name=="liberation serif" || name=="times new roman") family=2;
		else throw uint32_t(120);
		SkFont result(faces[family],SkScalar(-height));
		result.setHinting(SkFontHinting::kFull); result.setSubpixel(false); result.setLinearMetrics(false);
		result.setEdging(SkFont::Edging::kAntiAlias);
		return {result,family};
	}
};

TextRenderer::TextRenderer(const std::string &directory) : state(std::make_unique<State>())
{
	auto manager=SkFontMgr_New_Custom_Empty();
	const std::array<const char *,3> files{"LiberationSans-Regular.ttf","LiberationMono-Regular.ttf","LiberationSerif-Regular.ttf"};
	for (size_t i=0;i<files.size();++i) {
		state->faces[i]=manager->makeFromFile((directory+"/"+files[i]).c_str());
		if (!state->faces[i]) throw std::runtime_error("cannot load qualified native font file");
		state->styled[i][0]=state->faces[i];
	}
	const std::array<std::array<const char *,3>,3> styles{{
		{"LiberationSans-Bold.ttf","LiberationSans-Italic.ttf","LiberationSans-BoldItalic.ttf"},
		{"LiberationMono-Bold.ttf","LiberationMono-Italic.ttf","LiberationMono-BoldItalic.ttf"},
		{"LiberationSerif-Bold.ttf","LiberationSerif-Italic.ttf","LiberationSerif-BoldItalic.ttf"}}};
	for (size_t family=0;family<3;++family)
		for (size_t style=0;style<3;++style)
			state->styled[family][style+1]=manager->makeFromFile((directory+"/"+styles[family][style]).c_str());
	state->styled[3][0]=manager->makeFromFile((directory+"/RavenSans-Regular.ttf").c_str());
	state->styled[3][1]=manager->makeFromFile((directory+"/RavenSans-Bold.ttf").c_str());
	for (const auto *path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/local/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"})
		if (auto face=manager->makeFromFile(path)) state->fallbacks.push_back(std::move(face));
}

TextRenderer::~TextRenderer() = default;

TextMetrics TextRenderer::metrics(std::span<const uint8_t,92> definition)
{
	auto [font,family]=state->font(definition); SkFontMetrics source; font.getMetrics(&source);
	const auto rounded=[](float value) { return int32_t(std::lround(value)); };
	const auto ascent=rounded(-source.fAscent),descent=rounded(source.fDescent);
	std::array<uint8_t,96> table{};
	if (state->faces[family]->getTableData(0x4f532f32,0,table.size(),table.data())!=table.size())
		throw std::runtime_error("qualified font has no complete OS/2 metrics");
	const auto character=[&](size_t at) { return uint16_t((uint16_t(table[at])<<8)|table[at+1]); };
	return {ascent+descent,ascent,descent,rounded(-source.fAscent+source.fDescent-font.getSize()),rounded(source.fLeading),
		rounded(source.fAvgCharWidth),rounded(source.fMaxCharWidth),character(64),character(66),31,character(92),
		uint8_t(family==0 ? 39 : family==1 ? 54 : 23)};
}

static std::vector<SkGlyphID> glyphs(const SkFont &font,std::span<const uint16_t> text)
{
	if (text.size()>4096) throw uint32_t(87);
	for (const auto c : text)
		if (!((c>=32 && c<=126) || (c>=0x00a0 && c<=0x024f) || (c>=0x0370 && c<=0x03ff) || (c>=0x1e00 && c<=0x1eff))) throw uint32_t(120);
	std::vector<SkGlyphID> result(text.size());
	const auto count=font.textToGlyphs(text.data(),text.size_bytes(),SkTextEncoding::kUTF16,result);
	if (count!=text.size()) throw uint32_t(120);
	for (const auto glyph : result) if (!glyph) throw uint32_t(120);
	return result;
}

TextExtent TextRenderer::extent(std::span<const uint8_t,92> definition,std::span<const uint16_t> text)
{
	auto [font,family]=state->font(definition); (void)family;
	const auto ids=glyphs(font,text); std::vector<SkScalar> widths(ids.size()); font.getWidths(ids,widths);
	float width=0; for (const auto value : widths) width+=value;
	return {int32_t(std::lround(width)),metrics(definition).height};
}

std::vector<uint8_t> TextRenderer::mask(std::span<const uint8_t,92> definition,std::span<const uint16_t> text,
	uint32_t width,uint32_t height,int32_t x,int32_t y,std::span<const std::array<int32_t,2>> positions)
{
	if (!width || !height || width>8192 || height>8192 || uint64_t(width)*height>16*1024*1024) throw uint32_t(87);
	auto [font,family]=state->font(definition); (void)family;
	const auto ids=glyphs(font,text);
	font.setEdging(SkFont::Edging::kAlias);
	std::vector<uint8_t> pixels(size_t(width)*height*4);
	SkBitmap bitmap;
	if (!bitmap.installPixels(SkImageInfo::Make(int(width),int(height),kBGRA_8888_SkColorType,kOpaque_SkAlphaType),pixels.data(),size_t(width)*4))
		throw std::bad_alloc();
	SkCanvas canvas(bitmap); canvas.clear(SK_ColorWHITE); SkPaint paint; paint.setColor(SK_ColorBLACK);
	const auto ascent=metrics(definition).ascent;
	if(positions.empty())canvas.drawSimpleText(text.data(),text.size_bytes(),SkTextEncoding::kUTF16,SkScalar(x),SkScalar(int64_t(y)+ascent),font,paint);
	else{
		if(positions.size()!=ids.size())throw uint32_t(87);
		std::vector<SkPoint> points;points.reserve(positions.size());
		for(const auto &position:positions)points.push_back({SkScalar(int64_t(x)+position[0]),SkScalar(int64_t(y)+ascent+position[1])});
		canvas.drawGlyphs({ids.data(),ids.size()},{points.data(),points.size()},{0,0},font,paint);
	}
	std::vector<uint8_t> coverage(size_t(width)*height);
	for (size_t i=0;i<coverage.size();++i) {
		if (pixels[i*4]!=0 && pixels[i*4]!=255) throw std::runtime_error("qualified monochrome text produced intermediate coverage");
		coverage[i]=pixels[i*4]==0;
	}
	return coverage;
}

std::vector<uint8_t> TextRenderer::smooth(std::span<const uint8_t,92> definition,std::span<const uint16_t> text,
	uint32_t width,uint32_t height,int32_t x,int32_t y)
{
	if (!width || !height || width>8192 || height>8192 || uint64_t(width)*height>16*1024*1024) throw uint32_t(87);
	auto [font,family]=state->font(definition); (void)family;
	font.setEdging(SkFont::Edging::kAntiAlias);
	std::vector<uint8_t> alpha(size_t(width)*height);
	SkBitmap bitmap;
	if (!bitmap.installPixels(SkImageInfo::MakeA8(int(width),int(height)),alpha.data(),width)) throw std::bad_alloc();
	SkCanvas canvas(bitmap); canvas.clear(SK_ColorTRANSPARENT); SkPaint paint; paint.setColor(SK_ColorBLACK); paint.setAntiAlias(true);
	canvas.drawSimpleText(text.data(),text.size_bytes(),SkTextEncoding::kUTF16,SkScalar(x),SkScalar(int64_t(y)+metrics(definition).ascent),font,paint);
	return alpha;
}

std::vector<int32_t> TextRenderer::advances(std::span<const uint8_t,92> definition,std::span<const uint16_t> text)
{
	auto [font,family]=state->font(definition);(void)family;
	const auto ids=glyphs(font,text);std::vector<SkScalar> widths(ids.size());font.getWidths(ids,widths);
	std::vector<int32_t> result;result.reserve(widths.size());float total=0;
	for(const auto width:widths){total+=width;result.push_back(int32_t(std::lround(total)));}
	return result;
}


// The glyphs for UTF-16 text; characters the face lacks come from a fallback face when one has them, otherwise they use the
// replacement glyph, and controls draw nothing.
struct Resolved {
	SkGlyphID id=0;
	int fallback=-1;
};

static std::vector<Resolved> logical_glyphs(const SkFont &font,std::span<const uint16_t> text,const std::vector<sk_sp<SkTypeface>> &fallbacks)
{
	std::vector<Resolved> result(text.size());
	std::vector<uint16_t> printable;
	for (size_t i=0;i<text.size();++i) {
		const uint16_t c=text[i];
		printable.push_back(c<32 ? 32 : c);
	}
	// Surrogate pairs map to one glyph; the second unit keeps glyph 0 so positions stay unit-indexed.
	printable.push_back(0);
	for (size_t i=0;i+1<printable.size();++i) {
		SkGlyphID id=0;
		font.textToGlyphs(&printable[i],(printable[i]>=0xd800 && printable[i]<=0xdbff && printable[i+1]>=0xdc00 && printable[i+1]<=0xdfff) ? 4 : 2,SkTextEncoding::kUTF16,{&id,1});
		result[i].id=id;
		if (id || text[i]<32 || (text[i]>=0xdc00 && text[i]<=0xdfff)) continue;
		for (size_t index=0;index<fallbacks.size();++index) {
			SkFont alternative(fallbacks[index],font.getSize());
			alternative.textToGlyphs(&printable[i],(printable[i]>=0xd800 && printable[i]<=0xdbff && printable[i+1]>=0xdc00 && printable[i+1]<=0xdfff) ? 4 : 2,SkTextEncoding::kUTF16,{&id,1});
			if (id) { result[i]={id,int(index)}; break; }
		}
	}
	return result;
}

// Advance of every resolved glyph in the face that supplies it.
static std::vector<SkScalar> resolved_widths(const SkFont &font,const std::vector<Resolved> &ids,const std::vector<sk_sp<SkTypeface>> &fallbacks)
{
	std::vector<SkScalar> widths(ids.size(),0);
	for (size_t i=0;i<ids.size();++i) {
		if (ids[i].fallback<0) {
			font.getWidths({&ids[i].id,1},{&widths[i],1});
		} else {
			SkFont alternative(fallbacks[size_t(ids[i].fallback)],font.getSize());
			alternative.getWidths({&ids[i].id,1},{&widths[i],1});
		}
	}
	return widths;
}


// The Marlett symbol face is drawn procedurally: frame parts, check marks, arrows and caption glyphs on a square cell. Webdings is drawn
// the same way for the glyphs the two faces share: the four triangles ('3' to '6') and the close cross ('r').
static bool face_named(std::span<const uint8_t,92> definition,std::string_view name)
{
	for (size_t i=0;i<=name.size();++i) {
		auto c=field(definition,28+i*2,2);
		if (c>='A' && c<='Z') c+=32;
		if (c!=(i<name.size() ? uint32_t(uint8_t(name[i])) : 0)) return false;
	}
	return true;
}

static bool is_webdings(std::span<const uint8_t,92> definition)
{
	return face_named(definition,"webdings");
}

static bool is_marlett(std::span<const uint8_t,92> definition)
{
	return face_named(definition,"marlett") || is_webdings(definition);
}

static int32_t marlett_size(std::span<const uint8_t,92> definition)
{
	const auto height=int32_t(field(definition,0));
	return std::max<int32_t>(height<0 ? -height : height,4);
}

static void marlett_glyph(std::vector<uint8_t> &alpha,uint32_t stride,int32_t left,int32_t top,int32_t size,char16_t glyph,uint32_t width,uint32_t height)
{
	const auto put=[&](int32_t x,int32_t y) {
		x+=left; y+=top;
		if (x>=0 && y>=0 && uint32_t(x)<width && uint32_t(y)<height) alpha[size_t(y)*stride+uint32_t(x)]=255;
	};
	const auto box=[&](int32_t x0,int32_t y0,int32_t x1,int32_t y1) { for (int32_t y=y0;y<y1;++y) for (int32_t x=x0;x<x1;++x) put(x,y); };
	const auto line=[&](int32_t x0,int32_t y0,int32_t x1,int32_t y1,int32_t thickness) {
		const int32_t dx=std::abs(x1-x0),dy=std::abs(y1-y0),sx=x0<x1 ? 1 : -1,sy=y0<y1 ? 1 : -1;
		int32_t error=dx-dy;
		for (;;) {
			box(x0,y0,x0+thickness,y0+thickness);
			if (x0==x1 && y0==y1) break;
			const int32_t twice=2*error;
			if (twice>-dy) { error-=dy; x0+=sx; }
			if (twice<dx) { error+=dx; y0+=sy; }
		}
	};
	const int32_t s=size;
	const int32_t thick=std::max(1,s/8);
	// Distance of a cell pixel from the circle of the given diameter, used by the radio button rings.
	const auto ring=[&](int32_t inset,bool upper_left,bool lower_right,bool fill) {
		const double center=(s-1)/2.0,outer=(s-2*inset)/2.0;
		for (int32_t y=0;y<s;++y)
			for (int32_t x=0;x<s;++x) {
				const double dx=x-center,dy=y-center,distance=std::sqrt(dx*dx+dy*dy);
				const bool in_disc=distance<=outer-0.2,on_ring=in_disc && distance>outer-1.4;
				const bool toward_upper_left=(dx+dy)<0;
				if (fill ? in_disc : (on_ring && (toward_upper_left ? upper_left : lower_right))) put(x,y);
			}
	};
	switch (glyph) {
	case 'c': line(0,0,s-2,0,1); line(0,0,0,s-2,1); break;
	case 'd': line(0,s-1,s-1,s-1,1); line(s-1,0,s-1,s-1,1); break;
	case 'e': line(1,1,s-3,1,1); line(1,1,1,s-3,1); break;
	case 'f': line(1,s-2,s-2,s-2,1); line(s-2,1,s-2,s-2,1); break;
	case 'g': box(2,2,s-2,s-2); break;
	case 'b': line(s/5,s/2,s*2/5,s*7/10,thick); line(s*2/5,s*7/10,s*4/5,s*3/10,thick); break;
	case 'j': ring(0,false,true,false); break;
	case 'k': ring(0,true,false,false); break;
	case 'l': ring(1,false,true,false); break;
	case 'm': ring(1,true,false,false); break;
	case 'n': ring(2,false,false,true); break;
	case 'i': case 'h': ring(s/4,false,false,true); break;
	case 'a': line(s/5,s/2,s*2/5,s*7/10,thick); line(s*2/5,s*7/10,s*4/5,s*3/10,thick); break;
	case 'r': line(s/5,s/5,s*4/5-thick,s*4/5-thick,thick); line(s*4/5-thick,s/5,s/5,s*4/5-thick,thick); break;
	case '0': box(s/5,s*3/4,s*3/5,s*3/4+std::max(2,thick)); break;
	case '1': box(s/5,s/5,s*4/5,s/5+std::max(2,thick)); line(s/5,s/5,s/5,s*4/5,1); line(s*4/5-1,s/5,s*4/5-1,s*4/5,1); line(s/5,s*4/5-1,s*4/5-1,s*4/5-1,1); break;
	case '2': box(s*2/5,s/5,s*4/5,s/5+std::max(2,thick)); line(s*4/5-1,s/5,s*4/5-1,s*3/5,1); line(s/5,s*2/5,s*3/5,s*2/5,2); line(s/5,s*2/5,s/5,s*4/5,1); line(s*3/5-1,s*2/5,s*3/5-1,s*4/5,1); line(s/5,s*4/5-1,s*3/5,s*4/5-1,1); break;
	case '3': for (int32_t i=0;i<=s/4;++i) line(s*3/8+i,s/2-i,s*3/8+i,s/2+i,1); break;
	case '4': for (int32_t i=0;i<=s/4;++i) line(s*5/8-i,s/2-i,s*5/8-i,s/2+i,1); break;
	case '5': for (int32_t i=0;i<=s/4;++i) line(s/2-i,s*3/8+i,s/2+i,s*3/8+i,1); break;
	case '6': for (int32_t i=0;i<=s/4;++i) line(s/2-i,s*5/8-i,s/2+i,s*5/8-i,1); break;
	case 'o': case 'p': case 'x': case 'y':
		// Size grip: diagonal dotted stripes toward the lower right corner.
		for (int32_t k=1;k<=3;++k) line(s-1-k*s/4,s-1,s-1,s-1-k*s/4,1);
		break;
	default: break;
	}
}

TextMetrics TextRenderer::logical_metrics(std::span<const uint8_t,92> definition)
{
	if (is_marlett(definition)) {
		const auto size=marlett_size(definition);
		return {size,size,0,0,0,size,size,32,255,31,32,0x32};
	}
	// Fonts inside the qualified subset keep their measured OS/2 metrics.
	try { return metrics(definition); } catch (const uint32_t &) {}
	auto font=state->logical(definition); SkFontMetrics source; font.getMetrics(&source);
	const auto rounded=[](float value) { return int32_t(std::lround(value)); };
	const auto ascent=rounded(-source.fAscent),descent=rounded(source.fDescent);
	const auto average=source.fAvgCharWidth>0 ? source.fAvgCharWidth : font.getSize()*0.5f;
	const auto maximum=source.fMaxCharWidth>0 ? source.fMaxCharWidth : font.getSize();
	// Fixed-pitch faces report no variable-pitch flag.
	const auto *face=font.getTypeface();
	const bool fixed=face==state->styled[1][0].get() || face==state->styled[1][1].get() ||
		face==state->styled[1][2].get() || face==state->styled[1][3].get();
	return {ascent+descent,ascent,descent,std::max(0,rounded(-source.fAscent+source.fDescent-font.getSize())),rounded(source.fLeading),
		rounded(average),rounded(maximum),32,255,31,32,uint8_t(fixed ? 0x36 : 0x27)};
}

std::vector<uint8_t> TextRenderer::logical_table(std::span<const uint8_t,92> definition,uint32_t tag)
{
	if (is_marlett(definition)) return {};
	const auto font=state->logical(definition);
	const auto *face=font.getTypeface();
	const size_t size=face ? face->getTableSize(tag) : 0;
	std::vector<uint8_t> bytes(size);
	if (size && face->getTableData(tag,0,size,bytes.data())!=size) bytes.clear();
	return bytes;
}

uint32_t TextRenderer::logical_units_per_em(std::span<const uint8_t,92> definition)
{
	if (is_marlett(definition)) return 0;
	const auto font=state->logical(definition);
	return font.getTypeface() ? uint32_t(font.getTypeface()->getUnitsPerEm()) : 0;
}

std::vector<int32_t> TextRenderer::logical_advances(std::span<const uint8_t,92> definition,std::span<const uint16_t> text)
{
	if (is_marlett(definition)) {
		std::vector<int32_t> result;
		const auto size=marlett_size(definition);
		for (size_t i=0;i<text.size();++i) result.push_back(int32_t((i+1)*size));
		return result;
	}
	auto font=state->logical(definition);
	const auto ids=logical_glyphs(font,text,state->fallbacks);
	const auto widths=resolved_widths(font,ids,state->fallbacks);
	std::vector<int32_t> result; result.reserve(ids.size());
	// Prefixes round the running float sum, like the qualified measurement, so widths never drift per glyph.
	float total=0;
	for (size_t i=0;i<ids.size();++i) {
		if (text[i]>=32 && !(text[i]>=0xdc00 && text[i]<=0xdfff)) total+=widths[i];
		result.push_back(int32_t(std::lround(total)));
	}
	return result;
}

std::vector<uint8_t> TextRenderer::render(std::span<const uint8_t,92> definition,std::span<const uint16_t> text,
	uint32_t width,uint32_t height,int32_t origin_x,int32_t origin_y,std::span<const int32_t> offsets)
{
	if (!width || !height || width>16384 || height>16384 || uint64_t(width)*height>32*1024*1024) throw uint32_t(87);
	if (is_marlett(definition)) {
		std::vector<uint8_t> alpha(size_t(width)*height);
		const auto size=marlett_size(definition);
		const bool webdings=is_webdings(definition);
		for (size_t i=0;i<text.size();++i) {
			const auto glyph=char16_t(text[i]);
			if (webdings && glyph!='r' && (glyph<'3' || glyph>'6')) continue;
			marlett_glyph(alpha,width,(offsets.empty() ? int32_t(i)*size : offsets[i])+origin_x,origin_y,size,glyph,width,height);
		}
		return alpha;
	}
	auto font=state->logical(definition);
	const auto ids=logical_glyphs(font,text,state->fallbacks);
	std::vector<uint8_t> alpha(size_t(width)*height);
	SkBitmap bitmap;
	if (!bitmap.installPixels(SkImageInfo::MakeA8(int(width),int(height)),alpha.data(),width)) throw std::bad_alloc();
	SkCanvas canvas(bitmap); canvas.clear(SK_ColorTRANSPARENT);
	SkPaint paint; paint.setColor(SK_ColorBLACK); paint.setAntiAlias(font.getEdging()!=SkFont::Edging::kAlias);
	SkFontMetrics source; font.getMetrics(&source);
	const auto baseline=SkScalar(origin_y)+SkScalar(std::lround(-source.fAscent));
	const auto widths=resolved_widths(font,ids,state->fallbacks);
	std::vector<SkGlyphID> drawn; std::vector<SkPoint> points;
	SkScalar cursor=0;
	for (size_t i=0;i<ids.size();++i) {
		const SkScalar step=text[i]>=32 && !(text[i]>=0xdc00 && text[i]<=0xdfff) ? SkScalar(std::lround(widths[i])) : 0;
		const SkPoint at{SkScalar(origin_x)+(offsets.empty() ? cursor : SkScalar(offsets[i])),baseline};
		if (text[i]>=32 && ids[i].id) {
			if (ids[i].fallback<0) {
				drawn.push_back(ids[i].id);
				points.push_back(at);
			} else {
				SkFont alternative(state->fallbacks[size_t(ids[i].fallback)],font.getSize());
				alternative.setEdging(font.getEdging()); alternative.setHinting(font.getHinting());
				canvas.drawGlyphs({&ids[i].id,1},{&at,1},{0,0},alternative,paint);
			}
		}
		cursor+=step;
	}
	if (!drawn.empty()) canvas.drawGlyphs({drawn.data(),drawn.size()},{points.data(),points.size()},{0,0},font,paint);
	return alpha;
}

// The symbol face draws its own shapes by character, so its glyph identifiers are the characters themselves.
std::vector<uint16_t> TextRenderer::logical_glyph_ids(std::span<const uint8_t,92> definition,std::span<const uint16_t> text)
{
	if (is_marlett(definition)) return std::vector<uint16_t>(text.begin(),text.end());
	const auto font=state->logical(definition);
	std::vector<uint16_t> result(text.size(),0);
	for (size_t i=0;i<text.size();++i) {
		// A surrogate pair names one glyph, which the leading unit carries; the trailing unit maps to none of its own.
		const bool pair=text[i]>=0xd800 && text[i]<=0xdbff && i+1<text.size() && text[i+1]>=0xdc00 && text[i+1]<=0xdfff;
		SkGlyphID id=0;
		font.textToGlyphs(&text[i],pair ? 4 : 2,SkTextEncoding::kUTF16,{&id,1});
		result[i]=id;
		if (pair) ++i;
	}
	return result;
}

uint32_t TextRenderer::logical_glyph_count(std::span<const uint8_t,92> definition)
{
	if (is_marlett(definition)) return 0x10000;
	const auto font=state->logical(definition);
	return font.getTypeface() ? uint32_t(font.getTypeface()->countGlyphs()) : 0;
}

std::vector<int32_t> TextRenderer::logical_glyph_advances(std::span<const uint8_t,92> definition,std::span<const uint16_t> glyphs)
{
	if (is_marlett(definition)) return std::vector<int32_t>(glyphs.size(),marlett_size(definition));
	const auto font=state->logical(definition);
	std::vector<SkGlyphID> ids(glyphs.begin(),glyphs.end());
	std::vector<SkScalar> widths(ids.size());
	font.getWidths(ids,widths);
	std::vector<int32_t> result; result.reserve(widths.size());
	for (const auto width : widths) result.push_back(int32_t(std::lround(width)));
	return result;
}

TextRenderer::GlyphBox TextRenderer::logical_glyph_box(std::span<const uint8_t,92> definition,uint16_t glyph)
{
	if (is_marlett(definition)) {
		const auto size=marlett_size(definition);
		return {0,-size,size,0,size};
	}
	const auto font=state->logical(definition);
	const SkGlyphID id=glyph;
	SkRect bounds; SkScalar width=0;
	font.getWidthsBounds({&id,1},{&width,1},{&bounds,1},nullptr);
	if (bounds.isEmpty()) return {0,0,0,0,int32_t(std::lround(width))};
	return {int32_t(std::floor(bounds.left())),int32_t(std::floor(bounds.top())),int32_t(std::ceil(bounds.right())),int32_t(std::ceil(bounds.bottom())),
		int32_t(std::lround(width))};
}

std::vector<uint8_t> TextRenderer::render_glyph_box(std::span<const uint8_t,92> definition,uint16_t glyph,const GlyphBox &box)
{
	const int64_t width=int64_t(box.right)-box.left,height=int64_t(box.bottom)-box.top;
	if (width<=0 || height<=0) return {};
	if (width>16384 || height>16384 || width*height>32*1024*1024) throw uint32_t(87);
	if (is_marlett(definition)) return render(definition,std::span<const uint16_t>(&glyph,1),uint32_t(width),uint32_t(height),0,0);
	auto font=state->logical(definition);
	std::vector<uint8_t> alpha(size_t(width*height));
	SkBitmap bitmap;
	if (!bitmap.installPixels(SkImageInfo::MakeA8(int(width),int(height)),alpha.data(),size_t(width))) throw std::bad_alloc();
	SkCanvas canvas(bitmap); canvas.clear(SK_ColorTRANSPARENT);
	SkPaint paint; paint.setColor(SK_ColorBLACK); paint.setAntiAlias(font.getEdging()!=SkFont::Edging::kAlias);
	const SkGlyphID id=glyph;
	const SkPoint at{SkScalar(-box.left),SkScalar(-box.top)};
	canvas.drawGlyphs({&id,1},{&at,1},{0,0},font,paint);
	return alpha;
}

std::vector<uint8_t> TextRenderer::render_glyphs(std::span<const uint8_t,92> definition,std::span<const uint16_t> glyphs,
	uint32_t width,uint32_t height,int32_t origin_x,int32_t origin_y,std::span<const int32_t> offsets)
{
	if (is_marlett(definition)) return render(definition,glyphs,width,height,origin_x,origin_y,offsets);
	if (!width || !height || width>16384 || height>16384 || uint64_t(width)*height>32*1024*1024) throw uint32_t(87);
	auto font=state->logical(definition);
	std::vector<uint8_t> alpha(size_t(width)*height);
	SkBitmap bitmap;
	if (!bitmap.installPixels(SkImageInfo::MakeA8(int(width),int(height)),alpha.data(),width)) throw std::bad_alloc();
	SkCanvas canvas(bitmap); canvas.clear(SK_ColorTRANSPARENT);
	SkPaint paint; paint.setColor(SK_ColorBLACK); paint.setAntiAlias(font.getEdging()!=SkFont::Edging::kAlias);
	SkFontMetrics source; font.getMetrics(&source);
	const auto baseline=SkScalar(origin_y)+SkScalar(std::lround(-source.fAscent));
	std::vector<SkGlyphID> ids(glyphs.begin(),glyphs.end());
	std::vector<SkScalar> widths(ids.size());
	font.getWidths(ids,widths);
	std::vector<SkPoint> points(ids.size());
	SkScalar cursor=0;
	for (size_t i=0;i<ids.size();++i) {
		points[i]={SkScalar(origin_x)+(offsets.size()>i ? SkScalar(offsets[i]) : cursor),baseline};
		cursor+=SkScalar(std::lround(widths[i]));
	}
	if (!ids.empty()) canvas.drawGlyphs({ids.data(),ids.size()},{points.data(),points.size()},{0,0},font,paint);
	return alpha;
}

}
