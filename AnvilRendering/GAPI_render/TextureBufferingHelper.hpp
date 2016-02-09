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

	typename Textures_t::iterator Next(const typename Textures_t::iterator &iter)
	{
		auto res = iter == textures.end() ? textures.begin() : iter + 1;
		return res == textures.end() ? textures.begin() : res;
	}

	template <typename Func/*=bool(T&)*/>
	bool Buffer(Func &&func)
	{
		if (buffering_texture == textures.end())
			buffering_texture = textures.begin();

		auto next = Next(draw_texture);
		if (did_buffer && next != buffering_texture)
			draw_texture = next;

		if (!func(*buffering_texture))
			return false;

		did_buffer = true;
		buffering_texture = Next(buffering_texture);
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