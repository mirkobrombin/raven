#include "windows.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace raven {

const DesktopWindow *DesktopObjects::window(uint64_t handle) const
{
	const auto found=objects.find(handle);
	return found==objects.end() ? nullptr : std::get_if<DesktopWindow>(&found->second);
}

uint64_t DesktopObjects::window_root(uint64_t handle) const
{
	for (unsigned depth=0;depth<64;++depth) {
		const auto *record=window(handle);
		if (!record) return 0;
		if (!record->parent) return handle;
		handle=record->parent;
	}
	throw std::runtime_error("window ancestry exceeds its owner limit");
}

std::vector<uint64_t> DesktopObjects::child_windows(uint64_t parent) const
{
	std::vector<std::pair<uint64_t,uint64_t>> found;
	for (const auto handle:window_set)
		if (const auto *record=window(handle); record && record->parent==parent) found.emplace_back(record->order,handle);
	std::sort(found.begin(),found.end());
	std::vector<uint64_t> result;
	for (const auto &item:found) result.push_back(item.second);
	return result;
}

void DesktopObjects::restack(uint64_t handle,bool top)
{
	auto *record=window(handle);
	if (!record) return;
	const auto siblings=child_windows(record->parent);
	if (siblings.empty()) return;
	if (top) record->order=++next_order;
	else record->order=window(siblings.front())->order>0 ? window(siblings.front())->order-1 : 0;
	if (!top && record->order==0) for (const auto sibling:siblings) if (sibling!=handle) ++window(sibling)->order;
}

bool DesktopObjects::is_child(uint64_t parent,uint64_t child) const
{
	if (!parent || !child || parent==child) return false;
	for (unsigned depth=0;depth<64;++depth) {
		const auto *record=window(child);
		if (!record || !record->parent) return false;
		if (record->parent==parent) return true;
		child=record->parent;
	}
	throw std::runtime_error("window ancestry exceeds its owner limit");
}

bool DesktopObjects::window_visible(uint64_t handle) const
{
	for (unsigned depth=0;depth<64;++depth) {
		const auto *record=window(handle);
		if (!record || !record->visible) return false;
		if (!record->parent) return true;
		handle=record->parent;
	}
	throw std::runtime_error("window ancestry exceeds its owner limit");
}

void DesktopObjects::configure_window_dc(uint64_t handle,DesktopDC &device,bool frame) const
{
	const auto *record=window(handle),*root=window(window_root(handle));
	if (!record || !root) throw uint32_t(1400);
	device.window_frame=frame;
	if (record->parent && !window_visible(handle)) { device.bounds=DesktopRect{};return; }
	const auto x=frame ? record->outer.left : record->x,y=frame ? record->outer.top : record->y;
	const auto dx=int64_t(x)-root->outer.left,dy=int64_t(y)-root->outer.top;
	if (dx<INT32_MIN || dx>INT32_MAX || dy<INT32_MIN || dy>INT32_MAX) throw uint32_t(120);
	device.origin_x=int32_t(dx+device.user_dx);device.origin_y=int32_t(dy+device.user_dy);
	if (frame && !record->parent) { device.bounds.reset();return; }
	int64_t left=0,top=0,right=frame ? int64_t(record->outer.right)-x : record->width;
	int64_t bottom=frame ? int64_t(record->outer.bottom)-y : record->height;
	for (auto parent=record->parent;parent;) {
		const auto *ancestor=window(parent);if (!ancestor) throw uint32_t(1400);
		left=std::max(left,int64_t(ancestor->x)-x);top=std::max(top,int64_t(ancestor->y)-y);
		right=std::min(right,int64_t(ancestor->x)+ancestor->width-x);bottom=std::min(bottom,int64_t(ancestor->y)+ancestor->height-y);
		parent=ancestor->parent;
	}
	device.bounds=right<=left || bottom<=top ? DesktopRect{} : DesktopRect{int32_t(left),int32_t(top),int32_t(right),int32_t(bottom)};
	// WS_CLIPCHILDREN keeps drawing off the visible children, except those with WS_EX_TRANSPARENT, which show what their parent draws
	// under them; WS_CLIPSIBLINGS keeps it off the siblings stacked above. A window with WS_EX_COMPOSITED paints under its children,
	// which then paint over it, as Windows paints such a tree from the bottom up.
	device.visible.reset();
	const bool clip_children=(record->style & 0x02000000U)!=0 && !(record->ex_style & 0x02000000U),clip_siblings=(record->style & 0x04000000U)!=0 && record->parent;
	if (clip_children || clip_siblings) {
		DesktopRegion region;
		region.add({0,0,int32_t(std::min<int64_t>(frame ? int64_t(record->outer.right)-x : record->width,INT32_MAX)),
			int32_t(std::min<int64_t>(frame ? int64_t(record->outer.bottom)-y : record->height,INT32_MAX))});
		const auto cut=[&](const DesktopWindow &other) {
			if (!other.visible) return;
			region.subtract({int32_t(int64_t(other.outer.left)-x),int32_t(int64_t(other.outer.top)-y),
				int32_t(int64_t(other.outer.right)-x),int32_t(int64_t(other.outer.bottom)-y)});
		};
		if (clip_children)
			for (const auto child:child_windows(handle)) if (const auto *other=window(child); other && !(other->ex_style & 0x20U)) cut(*other);
		if (clip_siblings) {
			bool above=false;
			for (const auto sibling:child_windows(record->parent)) {
				if (sibling==handle) { above=true; continue; }
				if (above) if (const auto *other=window(sibling)) cut(*other);
			}
		}
		device.visible=std::move(region);
	}
}

bool NativeWindows::tree_dispatch(std::string_view name)
{
	if (name!="GetParent" && name!="GetDlgItem" && name!="GetDlgCtrlID" && name!="IsChild") return false;
	const auto args=process.api_arguments(name=="GetParent" || name=="GetDlgCtrlID" ? 1 : 2);
	uint64_t result=0;
	const auto *record=std::as_const(objects).window(args[0]);
	if (name=="IsChild") result=objects.is_child(args[0],args[1]);
	else if (!record) environment.set_last_error(1400);
	else if (name=="GetParent") result=record->parent ? record->parent : (record->style & 0x80000000U) && std::as_const(objects).window(record->owner) ? record->owner : 0;
	else if (name=="GetDlgCtrlID") result=uint32_t(record->child_id);
	else for (const auto child:objects.child_windows(args[0]))
		if (uint32_t(objects.window(child)->child_id)==uint32_t(args[1])) { result=child;break; }
	process.return_api(result);return true;
}

void NativeWindows::parent_notify(uint64_t handle,uint32_t message)
{
	const auto *record=std::as_const(objects).window(handle);
	if (!record || !record->parent || (record->ex_style & 4)) return;
	const auto parent=record->parent;
	if (!std::as_const(objects).window(parent) || std::as_const(objects).window(parent)->destroying) return;
	send(parent,0x210,message | (uint64_t(uint16_t(record->child_id))<<16),handle);
}

void NativeWindows::discard_tree(uint64_t handle)
{
	for (const auto child:objects.child_windows(handle)) discard_tree(child);
	release_surface(handle);objects.erase(handle);
}

void NativeWindows::reject_tree(uint64_t handle)
{
	auto *record=objects.window(handle);if (!record) return;
	record->destroying=true;record->visible=false;record->style&=~0x10000000U;
	for (const auto child:objects.child_windows(handle)) if (std::as_const(objects).window(child)) reject_tree(child);
	if (std::as_const(objects).window(handle)) send(handle,0x82,0,0);
	release_surface(handle);objects.erase(handle);
}

uint64_t NativeWindows::pointer_child(uint64_t handle,int32_t x,int32_t y)
{
	const auto *parent=std::as_const(objects).window(handle);if (!parent) return 0;
	if (x<parent->x || y<parent->y || int64_t(x)>=int64_t(parent->x)+parent->width || int64_t(y)>=int64_t(parent->y)+parent->height) return handle;
	const auto children=objects.child_windows(handle);
	for (auto child=children.rbegin();child!=children.rend();++child) {
		const auto *record=std::as_const(objects).window(*child);
		if (record && objects.window_visible(*child) && x>=record->outer.left && y>=record->outer.top && x<record->outer.right && y<record->outer.bottom) {
			const auto inner=pointer_child(*child,x,y);
			// A window that answers HTTRANSPARENT (a group box) leaves the point to the siblings beneath it.
			if (inner==*child && int32_t(send(*child,0x84,0,uint16_t(x)|(uint64_t(uint16_t(y))<<16)))==-1) continue;
			return inner;
		}
	}
	return handle;
}

}
