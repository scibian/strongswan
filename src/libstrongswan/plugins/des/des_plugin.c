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

#include "des_plugin.h"

#include <library.h>
#include "des_crypter.h"

typedef struct private_des_plugin_t private_des_plugin_t;

/**
 * private data of des_plugin
 */
struct private_des_plugin_t {

	/**
	 * public functions
	 */
	des_plugin_t public;
};

METHOD(plugin_t, get_name, char*,
	private_des_plugin_t *this)
{
	return "des";
}

METHOD(plugin_t, destroy, void,
	private_des_plugin_t *this)
{
	lib->crypto->remove_crypter(lib->crypto,
								(crypter_constructor_t)des_crypter_create);
	free(this);
}

/*
 * see header file
 */
plugin_t *des_plugin_create()
{
	private_des_plugin_t *this;

	INIT(this,
		.public = {
			.plugin = {
				.get_name = _get_name,
				.reload = (void*)return_false,
				.destroy = _destroy,
			},
		},
	);

	lib->crypto->add_crypter(lib->crypto, ENCR_3DES, get_name(this),
							 (crypter_constructor_t)des_crypter_create);
	lib->crypto->add_crypter(lib->crypto, ENCR_DES, get_name(this),
							 (crypter_constructor_t)des_crypter_create);
	lib->crypto->add_crypter(lib->crypto, ENCR_DES_ECB, get_name(this),
							 (crypter_constructor_t)des_crypter_create);

	return &this->public.plugin;
}

