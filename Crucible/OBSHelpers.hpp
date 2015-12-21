#pragma once

#include <obs.hpp>

#include <string>

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