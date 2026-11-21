#pragma once

namespace raven {

// How deeply the calling thread holds the desktop lock shared (see native/nt/desktop_lock.h). The desktop's objects read and change
// their table under a lock of its own while it is not zero, as other threads that hold the desktop lock shared may add and remove
// objects at the same time (see DesktopObjects).
inline thread_local unsigned desktop_shared_depth = 0;

}
