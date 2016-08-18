#pragma once

#include <../deps/ipc-util/ipc-util/pipe.h>

#include <functional>
#include <memory>
#include <string>

namespace std {

template <>
struct default_delete<ipc_pipe_server>
{
	void operator()(ipc_pipe_server *server)
	{
		ipc_pipe_server_free(server);
	}
};

template <>
struct default_delete<ipc_pipe_client>
{
	void operator()(ipc_pipe_client *client)
	{
		ipc_pipe_client_free(client);
	}
};

}

struct IPCServer {
	std::unique_ptr<std::function<void(uint8_t*, size_t)>> func_;
	std::unique_ptr<ipc_pipe_server> server;

	IPCServer() = default;

	//IPCServer(IPCServer &&) = default;
	IPCServer(IPCServer &&other)
	{
		server = std::move(other.server);
		func_ = std::move(other.func_);
	}

	//IPCServer &operator=(IPCServer &&) = default;
	IPCServer &operator=(IPCServer &&other)
	{
		server = std::move(other.server);
		func_ = std::move(other.func_);

		return *this;
	}

	template <typename Func>
	IPCServer(const std::string &name, Func &&func)
	{
		Start(name, std::forward<Func>(func));
	}

	bool Start(const std::string &name, void (*func)(uint8_t*, size_t))
	{
		server.reset(new ipc_pipe_server{});

		func_.reset(new std::function<void(uint8_t*, size_t)>{func});

		return ipc_pipe_server_start(server.get(), name.c_str(),
			[](void *param, uint8_t *data, size_t size)
		{
			(*static_cast<std::function<void(uint8_t*, size_t)>*>(param))(data, size);
		}, static_cast<void*>(func_.get()));
	}

	template <typename Func>
	bool Start(const std::string &name, Func &&func, int buf=-1)
	{
		server.reset(new ipc_pipe_server{});

		func_.reset(new std::function<void(uint8_t*, size_t)>{func});

		using NoRef_t = std::remove_reference_t<Func>;

		return ipc_pipe_server_start_buf(server.get(), name.c_str(),
				[](void *param, uint8_t *data, size_t size)
		{
			(*static_cast<NoRef_t*>(param))(data, size);
		}, static_cast<void*>(func_->target<NoRef_t>()), buf);
	}
};

struct IPCClient {
	std::unique_ptr<ipc_pipe_client> client;

	IPCClient() = default;

	//IPCClient(IPCClient &&) = default;
	IPCClient(IPCClient &&other)
	{
		client = std::move(other.client);
	}

	//IPCClient &operator=(IPCClient &&) = default;
	IPCClient &operator=(IPCClient &&other)
	{
		client = std::move(other.client);

		return *this;
	}

	IPCClient(const std::string &name)
	{
		Open(name);
	}

	bool Open(const std::string &name)
	{
		client.reset(new ipc_pipe_client{});

		return ipc_pipe_client_open(client.get(), name.c_str());
	}

	void Close()
	{
		client.reset();
	}

	bool Write(const void *data, size_t size)
	{
		return client && ipc_pipe_client_write(client.get(), data, size);
	}

	bool Write(const std::string &str)
	{
		return Write(str.c_str(), str.length() + 1);
	}

	explicit operator bool()
	{
		return client && ipc_pipe_client_valid(client.get());
	}
};
