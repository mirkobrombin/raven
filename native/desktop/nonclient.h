#pragma once
#include "region.h"

namespace raven {

struct DesktopMetrics {
	int32_t border_x=1,border_y=1,dialog_x=3,dialog_y=3,frame_x=4,frame_y=4;
	int32_t caption=26,small_caption=18,menu=19;
	int32_t button_x=18,button_y=25,small_button_x=17,small_button_y=17;
	// Default screen size for backends without output geometry.
	int32_t screen_x=1920,screen_y=1080;
	// The size of the work area a maximized window fills, which the host gave the last maximized window: its panels lie outside. 0 is
	// the screen's size.
	int32_t work_x=0,work_y=0;
	int32_t get(int32_t index) const;
	DesktopRect work_area() const { return {0,0,work_x ? work_x : screen_x,work_y ? work_y : screen_y}; }
};

// What GetSystemMetrics answers for an index.
uint32_t system_metric(const DesktopMetrics &screen, int32_t index);

DesktopRect adjust_window_rect(const DesktopMetrics &metrics,DesktopRect rect,uint32_t style,bool menu,uint32_t ex_style);

}
