#pragma once

namespace Display {
	void SetSource(const char *name, obs_source_t *source);
	bool Connect(const char *name, const char *server);

	void SetEnabled(const char *name, bool enable);

	void Resize(const char *name, uint32_t cx, uint32_t cy);

	std::vector<std::string> List();

	void Stop(const char *name);
	void StopAll();
}
