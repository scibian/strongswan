/*
 * Copyright (C) 2008 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "random_plugin.h"

#include <library.h>
#include "random_rng.h"

typedef struct private_random_plugin_t private_random_plugin_t;

/**
 * private data of random_plugin
 */
struct private_random_plugin_t {

	/**
	 * public functions
	 */
	random_plugin_t public;
};

METHOD(plugin_t, get_name, char*,
	private_random_plugin_t *this)
{
	return "random";
}

METHOD(plugin_t, destroy, void,
	private_random_plugin_t *this)
{
	lib->crypto->remove_rng(lib->crypto,
							(rng_constructor_t)random_rng_create);
	free(this);
}

/*
 * see header file
 */
plugin_t *random_plugin_create()
{
	private_random_plugin_t *this;

	INIT(this,
		.public = {
			.plugin = {
				.get_name = _get_name,
				.reload = (void*)return_false,
				.destroy = _destroy,
			},
		},
	);

	lib->crypto->add_rng(lib->crypto, RNG_STRONG, get_name(this),
						 (rng_constructor_t)random_rng_create);
	lib->crypto->add_rng(lib->crypto, RNG_TRUE, get_name(this),
						 (rng_constructor_t)random_rng_create);

	return &this->public.plugin;
}

