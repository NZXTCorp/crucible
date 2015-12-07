#pragma once

#include <obs.hpp>

#include <string>

inline OBSData OBSDataTransferOwned(obs_data_t *data)
{
	OBSData obj = data;
	obs_data_release(obj);
	return obj;
}

inline OBSData OBSDataCreate(const std::string &json = {})
{
	return OBSDataTransferOwned(json.empty() ? obs_data_create() : obs_data_create_from_json(json.c_str()));
}

inline OBSData OBSDataGetObj(obs_data_t *data, const char *name)
{
	return OBSDataTransferOwned(obs_data_get_obj(data, name));
}