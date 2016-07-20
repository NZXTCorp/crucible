#pragma once

#include <utility>

template <class Fun>
class scopeguard
{
    Fun f_;
    bool active_;
public:
    scopeguard(Fun f) :
        f_(std::move(f)),
        active_(true)
    {}
    scopeguard() = delete;
    scopeguard(const scopeguard&) = delete;
    scopeguard& operator=(const scopeguard&) = delete;
    scopeguard(scopeguard&& rhs) :
        f_(std::move(rhs.f_)),
        active_(rhs.active_)
    { rhs.dismiss(); }
    ~scopeguard() { if(active_) f_(); }
    void dismiss() { active_ = false; }
};

template <class Fun>
scopeguard<Fun> guard(Fun f)
{
    return scopeguard<Fun>(std::move(f));
}

namespace detail
{
    enum class scopeguard_on_exit {};
    template <class Fun>
    scopeguard<Fun> operator+(scopeguard_on_exit, Fun&& f)
    {
        return guard(std::forward<Fun>(f));
    }
}

#define CONCAT_IMPL(s1, s2) s1##s2
#define CONCAT(s1, s2) CONCAT_IMPL(s1, s2)

#ifdef __COUNTER__
#define ANONYMOUS_VARIABLE(str) CONCAT(str, __COUNTER__)
#else
#define ANONYMOUS_VARIABLE(str) CONCAT(str, __LINE__)
#endif

#define DEFER auto ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) = ::detail::scopeguard_on_exit() + [&]()

