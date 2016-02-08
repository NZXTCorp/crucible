#pragma once

#include <array>

template <typename T, std::size_t Buffers=3>
struct TextureBufferingHelper
{
	using Textures_t = std::array<T, Buffers>;
	Textures_t textures;
	typename Textures_t::iterator buffering_texture = textures.end();
	typename Textures_t::iterator draw_texture = textures.end();
	bool did_buffer = false;

	template <typename Func/*=void(T&)*/>
	void Apply(Func &&func)
	{
		for (auto &texture : textures)
			func(texture);
	}

	template <typename Func/*=void(T&)*/>
	void Reset(Func &&func)
	{
		Apply(std::forward<Func>(func));

		buffering_texture = textures.end();
		draw_texture = textures.end();
		did_buffer = false;
	}

	template <typename Func/*=bool(T&)*/>
	bool Buffer(Func &&func)
	{
		if (buffering_texture == textures.end())
			buffering_texture = textures.begin();

		if (!func(*buffering_texture))
			return false;

		if (did_buffer)
		{
			if (draw_texture != textures.end())
				draw_texture++;

			if (draw_texture == textures.end())
				draw_texture = textures.begin();
		}

		did_buffer = true;
		buffering_texture++;
		return true;
	}

	template <typename Func/*=bool(T&)*/>
	bool Draw(Func &&func)
	{
		if (draw_texture == textures.end())
			return false;

		return func(*draw_texture);
	}
};