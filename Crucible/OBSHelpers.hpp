#pragma once

#include <util/profiler.hpp>
#include <obs.hpp>

#include <memory>
#include <string>

namespace std {

template <>
struct default_delete<obs_data_item_t> {
	void operator()(obs_data_item_t *item)
	{
		obs_data_item_release(&item);
	}
};

template <>
struct default_delete<obs_properties_t> {
	void operator()(obs_properties_t *item)
	{
		obs_properties_destroy(item);
	}
};

template <>
struct default_delete<profiler_name_store_t> {
	void operator()(profiler_name_store_t *store)
	{
		profiler_name_store_free(store);
	}
};

template <>
struct default_delete<profiler_snapshot_t> {
	void operator()(profiler_snapshot_t *snap)
	{
		profile_snapshot_free(snap);
	}
};

template <>
struct default_delete<obs_volmeter_t> {
	void operator()(obs_volmeter_t *meter)
	{
		obs_volmeter_destroy(meter);
	}
};


}

inline OBSEncoder OBSTransferOwned(obs_encoder_t *encoder)
{
	OBSEncoder enc = encoder;
	obs_encoder_release(enc);
	return enc;
}

inline OBSData OBSTransferOwned(obs_data_t *data)
{
	OBSData obj = data;
	obs_data_release(obj);
	return obj;
}

inline OBSDataArray OBSTransferOwned(obs_data_array_t *data)
{
	OBSDataArray obj = data;
	obs_data_array_release(obj);
	return obj;
}

inline OBSSource OBSTransferOwned(obs_source_t *source)
{
	OBSSource src = source;
	obs_source_release(source);
	return src;
}

inline OBSDataArray OBSDataArrayCreate()
{
	return OBSTransferOwned(obs_data_array_create());
}

inline OBSData OBSDataCreate(const std::string &json = {})
{
	return OBSTransferOwned(json.empty() ? obs_data_create() : obs_data_create_from_json(json.c_str()));
}

inline OBSData OBSDataGetObj(obs_data_t *data, const char *name)
{
	return OBSTransferOwned(obs_data_get_obj(data, name));
}

template <typename Fun>
inline void OBSEnumHotkeys(Fun &&fun)
{
	obs_enum_hotkeys([](void *data, obs_hotkey_id id, obs_hotkey_t *key)
	{
		(*static_cast<Fun*>(data))(id, key);
		return true;
	}, static_cast<void*>(&fun));
}

inline std::unique_ptr<profiler_snapshot_t> ProfileSnapshotCreate()
{
	return std::unique_ptr<profiler_snapshot_t>{profile_snapshot_create()};
}

inline std::unique_ptr<obs_volmeter_t> OBSVolMeterCreate(obs_fader_type type)
{
	return std::unique_ptr<obs_volmeter_t>{obs_volmeter_create(type)};
}

inline OBSSource OBSGetOutputSource(uint32_t channel)
{
	return OBSTransferOwned(obs_get_output_source(channel));
}
