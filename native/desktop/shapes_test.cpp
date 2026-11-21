#include "shapes.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

// Every blend the shapes make, in order.
struct Recorder {
	std::vector<std::tuple<int64_t,int64_t,uint32_t,double>> marks;
	void blend(int64_t x,int64_t y,uint32_t color,double alpha) { marks.emplace_back(x,y,color,alpha); }
};

struct Discard {
	void blend(int64_t,int64_t,uint32_t,double) {}
};

static std::chrono::nanoseconds cpu_time()
{
	timespec now{};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now);
	return std::chrono::seconds(now.tv_sec)+std::chrono::nanoseconds(now.tv_nsec);
}

// The fastest of five runs of twenty outlines of a square of this side.
static std::chrono::nanoseconds outline_time(double side)
{
	Discard surface;
	raven::shapes::Shapes<Discard> shapes{surface};
	auto best=std::chrono::nanoseconds::max();
	for (int run=0;run<5;++run) {
		const auto start=cpu_time();
		for (int outline=0;outline<20;++outline) shapes.rounded(0,0,side,side,0,std::nullopt,0x808080);
		best=std::min(best,cpu_time()-start);
	}
	return best;
}

int main()
{
	try {
		// Each pixel of the bounds that the shape covers, in order, with the coverage it gives, is what rounded() must blend.
		const std::tuple<double,double,double,double,double,double,double,double> cases[]={{0,0,64,48,0,1,0,0},{2.5,3.25,90.5,70,6,1,0,0},
			{0,0,40,30,8,2.5,0,0},{1,1,9,7,3,1,0,0},{0,0,300,200,12,1,0,0},{-20.5,-10,30,25,4,1,7,-3}};
		for (const auto &[left,top,right,bottom,radius,width,ox,oy] : cases)
			for (const bool filled : {false,true}) {
				Recorder drawn,expected;
				raven::shapes::Shapes<Recorder> shapes{drawn,ox,oy};
				shapes.rounded(left,top,right,bottom,radius,filled ? std::optional<uint32_t>(0x112233) : std::nullopt,0x445566,width);
				const double l=left+ox,t=top+oy,r=right+ox,b=bottom+oy;
				for (int64_t y=int64_t(std::floor(t));y<int64_t(std::ceil(b));++y)
					for (int64_t x=int64_t(std::floor(l));x<int64_t(std::ceil(r));++x) {
						const double d=raven::shapes::Shapes<Recorder>::rounded_distance(double(x)+0.5,double(y)+0.5,l,t,r,b,radius);
						const double outer=std::clamp(0.5-d,0.0,1.0),inner=std::clamp(0.5-(d+width),0.0,1.0);
						if (outer<=0) continue;
						if (filled && inner>0.002) expected.blend(x,y,0x112233,std::min(inner,1.0));
						if (outer-inner>0.002) expected.blend(x,y,0x445566,std::min(outer-inner,1.0));
					}
				check(drawn.marks==expected.marks,"a rounded rectangle is not drawn as its coverage gives");
			}
		// A border alone costs its perimeter: ten times the side takes about ten times as long, not a hundred.
		const auto small=outline_time(200),large=outline_time(2000);
		std::cout<<"twenty outlines of 200: "<<small.count()/1000<<" us, of 2000: "<<large.count()/1000<<" us\n";
		check(small.count()>0,"the processor time of this thread is not measured");
		check(large<small*30,"the outline of a rectangle costs its area");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
