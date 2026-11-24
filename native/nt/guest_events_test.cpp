#include "guest_events.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

using raven::GuestEvents;

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

int main()
{
	try {
		std::vector<uint8_t> memory(GuestEvents::size + 8);
		auto *const table = reinterpret_cast<uint8_t *>((uintptr_t(memory.data()) + 7) & ~uintptr_t(7));
		// The guest's descriptor table is this process's own: giving one dups it, dropping one closes it.
		GuestEvents events(table, [](int descriptor) { return ::dup(descriptor); }, [](int descriptor) { ::close(descriptor); });
		const auto slot = [&](uint64_t handle) { return table + GuestEvents::header + GuestEvents::slot_of(handle) * GuestEvents::slot_size; };
		const auto held = [&](uint64_t handle) { uint64_t value; std::memcpy(&value, slot(handle), 8); return value; };
		const auto descriptor = [&](uint64_t handle) { int32_t value; std::memcpy(&value, slot(handle) + 8, 4); return value; };
		const auto users = [&](uint64_t handle) { return reinterpret_cast<std::atomic<uint32_t> *>(slot(handle) + 12); };
		int pipe_ends[2];
		check(::pipe(pipe_ends) == 0, "no pipe");
		const int event_one = 1, event_two = 2;

		// An offer puts the handle and the guest's own descriptor in the handle's slot; another handle of that slot is refused.
		const auto ticket = events.ticket(0x40);
		check(ticket && events.offer(0x40, &event_one, pipe_ends[0], *ticket, [] { return true; }), "the offer was refused");
		check(held(0x40) == 0x40 && descriptor(0x40) > pipe_ends[1] && fcntl(descriptor(0x40), F_GETFD) != -1, "the slot was not filled");
		check(!events.ticket(0x40 + 4 * GuestEvents::slots), "a taken slot gave a ticket");
		const int given = descriptor(0x40);

		// A refusal by the caller, and a handle closed since the ticket, leave the slot empty and the descriptor given back.
		auto other = events.ticket(0x80);
		check(other && !events.offer(0x80, &event_two, pipe_ends[0], *other, [] { return false; }) && !held(0x80), "a refused offer stayed");
		other = events.ticket(0x80);
		events.withdraw(0x80);
		check(!events.offer(0x80, &event_two, pipe_ends[0], *other, [] { return true; }) && !held(0x80), "an offer for a closed handle stayed");

		// Taking a slot back waits for the guest among its users, and closes the guest's descriptor at the next flush.
		users(0x40)->store(1);
		std::thread guest([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			users(0x40)->store(0);
		});
		const auto started = std::chrono::steady_clock::now();
		events.withdraw(0x40);
		check(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(40), "the slot was taken back from a user");
		guest.join();
		check(!held(0x40) && fcntl(given, F_GETFD) != -1, "the slot stayed, or its descriptor went before the flush");
		events.flush();
		check(fcntl(given, F_GETFD) == -1, "the flush left the guest's descriptor open");

		// Holding sets the header for the guest and waits for the users of every slot; an event's slots go with it.
		other = events.ticket(0x100);
		auto third = events.ticket(0x104);
		check(other && third && events.offer(0x100, &event_two, pipe_ends[0], *other, [] { return true; }) &&
			events.offer(0x104, &event_two, pipe_ends[0], *third, [] { return true; }), "two handles of one event were refused");
		users(0x104)->store(1);
		std::thread late([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			users(0x104)->store(0);
		});
		const auto holding = std::chrono::steady_clock::now();
		events.hold();
		check(std::chrono::steady_clock::now() - holding >= std::chrono::milliseconds(40) && table[GuestEvents::barred] == 1,
			"holding did not wait for the guest or set the header");
		late.join();
		events.hold();
		events.release();
		check(table[GuestEvents::barred] == 1, "the header cleared while still held");
		events.release();
		check(table[GuestEvents::barred] == 0, "the header stayed set");
		// A paused event keeps its slots with their handles marked, until nothing holds it back.
		events.pause_event(&event_two);
		check(held(0x100) == (0x100 | GuestEvents::paused) && held(0x104) == (0x104 | GuestEvents::paused), "the event was not paused");
		events.resume_event(&event_two, [] { return false; });
		check(held(0x100) & GuestEvents::paused, "the event resumed while still held back");
		events.resume_event(&event_two, [] { return true; });
		check(held(0x100) == 0x100 && held(0x104) == 0x104, "the event did not resume");
		events.pause_event(&event_two);
		events.withdraw(0x104);
		check(!held(0x104), "a paused handle stayed when closed");
		events.withdraw_event(&event_two);
		check(!held(0x100) && !held(0x104), "an event's slots stayed");
		events.flush();
		// Once the process is going, a slot's users are not waited for: one of them may have been ended there.
		auto last = events.ticket(0x200);
		check(last && events.offer(0x200, &event_one, pipe_ends[0], *last, [] { return true; }), "the last offer was refused");
		users(0x200)->store(1);
		events.shut();
		events.withdraw(0x200);
		check(!held(0x200) && table[GuestEvents::barred] == 1, "shutting down waited for a user or left the guest working events");
		events.flush();
		std::cout << "guest events contract passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
