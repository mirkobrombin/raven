#include "region.h"
#include <stdexcept>
#include <new>
#ifdef RAVEN_PIXMAN
#include <pixman.h>
#endif
namespace raven {
struct DesktopRegion::State {
#ifdef RAVEN_PIXMAN
	pixman_region32_t region;
	State() { pixman_region32_init(&region); }
	~State() { pixman_region32_fini(&region); }
#endif
};
DesktopRegion::DesktopRegion()=default;
DesktopRegion::~DesktopRegion()=default;
DesktopRegion::DesktopRegion(DesktopRegion &&) noexcept=default;
DesktopRegion &DesktopRegion::operator=(DesktopRegion &&) noexcept=default;
DesktopRegion::DesktopRegion(const DesktopRegion &other)
{
#ifdef RAVEN_PIXMAN
	if (other.state) {
		state=std::make_unique<State>();
		if (!pixman_region32_copy(&state->region,&other.state->region)) throw std::bad_alloc();
	}
#else
	(void)other;
#endif
}
DesktopRegion &DesktopRegion::operator=(const DesktopRegion &other)
{
	DesktopRegion copy(other); state.swap(copy.state); return *this;
}
void DesktopRegion::change(DesktopRect rect,bool remove)
{
	if (rect.right<=rect.left || rect.bottom<=rect.top) return;
#ifdef RAVEN_PIXMAN
	if (int64_t(rect.right)-rect.left>INT32_MAX || int64_t(rect.bottom)-rect.top>INT32_MAX)
		throw std::runtime_error("region rectangle exceeds policy");
	DesktopRegion next(*this);
	if (!next.state) next.state=std::make_unique<State>();
	pixman_region32_t operand;
	pixman_region32_init_rect(&operand,rect.left,rect.top,uint32_t(rect.right-rect.left),uint32_t(rect.bottom-rect.top));
	const bool changed=remove ? pixman_region32_subtract(&next.state->region,&next.state->region,&operand) :
		pixman_region32_union(&next.state->region,&next.state->region,&operand);
	pixman_region32_fini(&operand);
	if (!changed || pixman_region32_n_rects(&next.state->region)>4096) throw std::bad_alloc();
	state.swap(next.state);
#else
	(void)remove; throw std::runtime_error("native region support not built");
#endif
}
void DesktopRegion::add(DesktopRect rect) { change(rect,false); }
void DesktopRegion::subtract(DesktopRect rect) { change(rect,true); }
void DesktopRegion::combine(const DesktopRegion &first,const DesktopRegion &second,uint32_t mode)
{
	if (mode<1 || mode>5) throw std::runtime_error("invalid region operation");
#ifdef RAVEN_PIXMAN
	DesktopRegion next;next.state=std::make_unique<State>();State empty;
	auto *a=first.state ? &first.state->region : &empty.region;
	auto *b=second.state ? &second.state->region : &empty.region;
	bool changed=false;
	if (mode==1) changed=pixman_region32_intersect(&next.state->region,a,b);
	else if (mode==2) changed=pixman_region32_union(&next.state->region,a,b);
	else if (mode==4) changed=pixman_region32_subtract(&next.state->region,a,b);
	else if (mode==5) changed=pixman_region32_copy(&next.state->region,a);
	else {
		State left,right;
		changed=pixman_region32_subtract(&left.region,a,b) && pixman_region32_subtract(&right.region,b,a) &&
			pixman_region32_union(&next.state->region,&left.region,&right.region);
	}
	if (!changed || pixman_region32_n_rects(&next.state->region)>4096) throw std::bad_alloc();
	state.swap(next.state);
#else
	(void)first;(void)second;throw std::runtime_error("native region support not built");
#endif
}
void DesktopRegion::offset(int32_t x,int32_t y)
{
#ifdef RAVEN_PIXMAN
	if (!state || empty()) return;
	const auto box=bounds();
	if (int64_t(box.left)+x<INT32_MIN || int64_t(box.right)+x>INT32_MAX ||
		int64_t(box.top)+y<INT32_MIN || int64_t(box.bottom)+y>INT32_MAX)
		throw std::runtime_error("region offset exceeds policy");
	pixman_region32_translate(&state->region,x,y);
#else
	(void)x;(void)y;throw std::runtime_error("native region support not built");
#endif
}
uint32_t DesktopRegion::complexity() const
{
#ifdef RAVEN_PIXMAN
	return !state || !pixman_region32_not_empty(&state->region) ? 1 : pixman_region32_n_rects(&state->region)==1 ? 2 : 3;
#else
	throw std::runtime_error("native region support not built");
#endif
}
bool DesktopRegion::equals(const DesktopRegion &other) const
{
#ifdef RAVEN_PIXMAN
	if (empty() || other.empty()) return empty()==other.empty();
	return pixman_region32_equal(&state->region,&other.state->region);
#else
	(void)other;throw std::runtime_error("native region support not built");
#endif
}
bool DesktopRegion::empty() const
{
#ifdef RAVEN_PIXMAN
	return !state || !pixman_region32_not_empty(&state->region);
#else
	return true;
#endif
}
bool DesktopRegion::contains(int32_t x,int32_t y) const
{
#ifdef RAVEN_PIXMAN
	return state && pixman_region32_contains_point(&state->region,x,y,nullptr);
#else
	(void)x; (void)y; return false;
#endif
}
std::vector<DesktopRect> DesktopRegion::rectangles() const
{
	std::vector<DesktopRect> result;
#ifdef RAVEN_PIXMAN
	if (!state) return result;
	int count=0;
	const auto *boxes=pixman_region32_rectangles(&state->region,&count);
	result.reserve(size_t(count));
	for (int i=0;i<count;++i) result.push_back({boxes[i].x1,boxes[i].y1,boxes[i].x2,boxes[i].y2});
#endif
	return result;
}
DesktopRect DesktopRegion::bounds() const
{
#ifdef RAVEN_PIXMAN
	if (!empty()) { const auto *rect=pixman_region32_extents(&state->region); return {rect->x1,rect->y1,rect->x2,rect->y2}; }
#endif
	return {};
}
size_t DesktopRegion::bytes() const
{
#ifdef RAVEN_PIXMAN
	return state ? size_t(pixman_region32_n_rects(&state->region))*sizeof(pixman_box32_t) : 0;
#else
	return 0;
#endif
}
}
