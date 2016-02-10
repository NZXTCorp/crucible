#pragma once

#include <mutex>
#include <type_traits>

template <typename T, typename Mutex=std::recursive_mutex>
struct ProtectedObject
{
	struct Proxy
	{
		Proxy() = default;
		Proxy(const Proxy &) = delete;
		Proxy(Proxy &&o)
			: obj(std::move(o.obj)), lock(std::move(o.lock))
		{
			o.obj = nullptr;
		}

		Proxy &operator=(const Proxy &) = delete;
		Proxy &operator=(Proxy &&o)
		{
			if (lock)
			{
				obj = nullptr;
				lock.unlock();
			}

			obj = std::move(o.obj);
			lock = std::move(o.lock);

			o.obj = nullptr;

			return *this;
		}

		Proxy(ProtectedObject &obj)
			: obj(&obj), lock(obj.mutex)
		{}

		explicit operator bool() const
		{
			return obj && lock;
		}

		T *operator->()
		{
			return obj ? &obj->object : nullptr;
		}

		T &operator*()
		{
			return obj->object;
		}

		/*operator T*()
		{
			return obj ? obj->object : nullptr;
		}*/

	protected:
		ProtectedObject *obj = nullptr;
		std::unique_lock<Mutex> lock;
	};

	ProtectedObject() = default;
	ProtectedObject(const ProtectedObject &) = delete;
	ProtectedObject(ProtectedObject &&) = delete;
	
	ProtectedObject &operator=(const ProtectedObject &) = delete;
	ProtectedObject &operator=(ProtectedObject &&) = delete;

	template <typename T, typename ... Ts>
	ProtectedObject(T &&t, Ts &&... ts)
		: object(std::forward<T>(t), std::forward<Ts>(ts)...)
	{}

	Proxy Lock()
	{
		return{ *this };
	}

protected:
	T object;
	Mutex mutex;
};