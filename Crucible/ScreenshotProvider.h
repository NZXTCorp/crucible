#pragma once

#include <functional>
#include <boost/optional.hpp>

typedef std::function<void(boost::optional<std::string> error, uint32_t cx, uint32_t cy, const std::string filename)> request_completion_t;
namespace Screenshot {
	void Request(OBSSource source, uint32_t cx, uint32_t cy, const std::string &filename, request_completion_t callback);
};