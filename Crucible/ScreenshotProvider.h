#pragma once

#include <functional>

typedef std::function<void(bool success, uint32_t cx, uint32_t cy, const std::string filename)> request_completion_t;
namespace Screenshot {	
	void SetSource(obs_source_t *source);
	
	void Request(uint32_t cx, uint32_t cy, const std::string &filename, request_completion_t callback);
};