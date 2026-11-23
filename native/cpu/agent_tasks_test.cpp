#include "agent.h"
#include "agent_tasks.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// The table of tasks in the agent's hands, as threads of the runtime share it: one host thread waits for its task while another forgets
// the task's whole group, as the end of a guest does. The wait goes on with the task it found, whatever the table holds by then. Built
// with the address sanitizer too, where a wait that kept a task the table had freed stops the program.

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void forgotten_while_waiting()
{
	const pid_t child = fork();
	check(child >= 0, "cannot start a task to wait for");
	if (child == 0) {
		pause();
		_exit(0);
	}
	std::vector<uint64_t> words(512);
	raven::agent::Task task;
	task.task = child;
	task.group = child;
	task.process = child;
	task.block = words.data();
	raven::agent::hold(task);
	std::atomic<bool> waiting = false;
	pid_t result = 0;
	int error = 0;
	std::thread waiter([&] {
		int status = 0;
		waiting = true;
		result = raven::agent::wait(child, &status, 0);
		error = errno;
	});
	while (!waiting)
		std::this_thread::yield();
	// The wait sleeps in slices of 10 ms: the group goes while it sleeps, and it wakes into a table without its task.
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	raven::agent::release_group(child);
	// Killed before the join, so a wait that came too late to find the task in the table, and went to waitpid, ends as well.
	kill(child, SIGKILL);
	waiter.join();
	int status = 0;
	waitpid(child, &status, 0);
	check(!raven::agent::held(child), "a task whose group was forgotten stayed registered");
	check(result == -1 && error == EINTR, "a wait for a task forgotten while it slept did not come back cut short");
}

int main()
{
	try {
		for (int round = 0; round < 20; ++round)
			forgotten_while_waiting();
	} catch (const std::exception &failure) {
		std::cerr << failure.what() << '\n';
		return 1;
	}
	std::cout << "the agent's table of tasks outlives forgetting while threads wait\n";
	return 0;
}
