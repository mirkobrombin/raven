#pragma once

namespace raven {

// Takes the memory the program that started the runtime shares with the Windows program (--link-fd): maps it in the runtime, where waits
// sleep on its words, and keeps it from the runtime's children. False when the descriptor names no memory that can be mapped.
bool adopt_link(int descriptor);

}
