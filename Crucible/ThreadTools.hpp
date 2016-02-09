#pragma once

#include <Windows.h>

#include <functional>
#include <future>
#include <thread>

struct HandleDeleter
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};

struct JoiningThread
{
	std::thread t;
	std::function<void()> make_joinable;
	std::future<int> monitor;
	~JoiningThread()
	{
		Join();
	}
	JoiningThread() = default;
	JoiningThread(const JoiningThread &) = delete;
	JoiningThread(JoiningThread &&other)
		: t(std::move(other.t)), make_joinable(std::move(other.make_joinable)), monitor(std::move(other.monitor))
	{}

	JoiningThread &operator=(const JoiningThread &) = delete;
	JoiningThread &operator=(JoiningThread &&other)
	{
		Join();

		t = std::move(other.t);
		make_joinable = std::move(other.make_joinable);
		monitor = std::move(other.monitor);

		return *this;
	}

	void Join()
	{
		if (make_joinable)
		{
			make_joinable();
			make_joinable = nullptr;
		}

		if (t.joinable())
			t.join();
	}

	bool TryJoin()
	{
		if (!t.joinable() || !monitor.valid() || monitor.wait_for(std::chrono::milliseconds(0)) != future_status::ready)
			return false;

		make_joinable = nullptr;
		t.join();

		return true;
	}

	template <typename Func>
	void Run(Func &&f)
	{
		std::packaged_task<int()> task{ [f] { f(); return 0; } };
		monitor = task.get_future();
		t = std::thread(std::move(task));
	}
};