/*
 * Copyright (C) 2010 Martin Willi
 * Copyright (C) 2010 revosec AG
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

#include "tls_writer.h"

typedef struct private_tls_writer_t private_tls_writer_t;

/**
 * Private data of an tls_writer_t object.
 */
struct private_tls_writer_t {

	/**
	 * Public tls_writer_t interface.
	 */
	tls_writer_t public;

	/**
	 * Allocated buffer
	 */
	chunk_t buf;

	/**
	 * Used bytes in buffer
	 */
	size_t used;

	/**
	 * Number of bytes to increase buffer size
	 */
	size_t increase;
};

/**
 * Increase buffer size
 */
static void increase(private_tls_writer_t *this)
{
	this->buf.len += this->increase;
	this->buf.ptr = realloc(this->buf.ptr, this->buf.len);
}

METHOD(tls_writer_t, write_uint8, void,
	private_tls_writer_t *this, u_int8_t value)
{
	if (this->used + 1 > this->buf.len)
	{
		increase(this);
	}
	this->buf.ptr[this->used] = value;
	this->used += 1;
}

METHOD(tls_writer_t, write_uint16, void,
	private_tls_writer_t *this, u_int16_t value)
{
	if (this->used + 2 > this->buf.len)
	{
		increase(this);
	}
	htoun16(this->buf.ptr + this->used, value);
	this->used += 2;
}

METHOD(tls_writer_t, write_uint24, void,
	private_tls_writer_t *this, u_int32_t value)
{
	if (this->used + 3 > this->buf.len)
	{
		increase(this);
	}
	value = htonl(value);
	memcpy(this->buf.ptr + this->used, ((char*)&value) + 1, 3);
	this->used += 3;
}

METHOD(tls_writer_t, write_uint32, void,
	private_tls_writer_t *this, u_int32_t value)
{
	if (this->used + 4 > this->buf.len)
	{
		increase(this);
	}
	htoun32(this->buf.ptr + this->used, value);
	this->used += 4;
}

METHOD(tls_writer_t, write_data, void,
	private_tls_writer_t *this, chunk_t value)
{
	while (this->used + value.len > this->buf.len)
	{
		increase(this);
	}
	memcpy(this->buf.ptr + this->used, value.ptr, value.len);
	this->used += value.len;
}

METHOD(tls_writer_t, write_data8, void,
	private_tls_writer_t *this, chunk_t value)
{
	write_uint8(this, value.len);
	write_data(this, value);
}

METHOD(tls_writer_t, write_data16, void,
	private_tls_writer_t *this, chunk_t value)
{
	write_uint16(this, value.len);
	write_data(this, value);
}

METHOD(tls_writer_t, write_data24, void,
	private_tls_writer_t *this, chunk_t value)
{
	write_uint24(this, value.len);
	write_data(this, value);
}

METHOD(tls_writer_t, write_data32, void,
	private_tls_writer_t *this, chunk_t value)
{
	write_uint32(this, value.len);
	write_data(this, value);
}

METHOD(tls_writer_t, wrap8, void,
	private_tls_writer_t *this)
{
	if (this->used + 1 > this->buf.len)
	{
		increase(this);
	}
	memmove(this->buf.ptr + 1, this->buf.ptr, this->used);
	this->buf.ptr[0] = this->used;
	this->used += 1;
}

METHOD(tls_writer_t, wrap16, void,
	private_tls_writer_t *this)
{
	if (this->used + 2 > this->buf.len)
	{
		increase(this);
	}
	memmove(this->buf.ptr + 2, this->buf.ptr, this->used);
	htoun16(this->buf.ptr, this->used);
	this->used += 2;
}

METHOD(tls_writer_t, wrap24, void,
	private_tls_writer_t *this)
{
	u_int32_t len;

	if (this->used + 3 > this->buf.len)
	{
		increase(this);
	}
	memmove(this->buf.ptr + 3, this->buf.ptr, this->used);

	len = htonl(this->used);
	memcpy(this->buf.ptr, ((char*)&len) + 1, 3);
	this->used += 3;
}

METHOD(tls_writer_t, wrap32, void,
	private_tls_writer_t *this)
{
	if (this->used + 4 > this->buf.len)
	{
		increase(this);
	}
	memmove(this->buf.ptr + 4, this->buf.ptr, this->used);
	htoun32(this->buf.ptr, this->used);
	this->used += 4;
}

METHOD(tls_writer_t, get_buf, chunk_t,
	private_tls_writer_t *this)
{
	return chunk_create(this->buf.ptr, this->used);
}

METHOD(tls_writer_t, destroy, void,
	private_tls_writer_t *this)
{
	free(this->buf.ptr);
	free(this);
}

/**
 * See header
 */
tls_writer_t *tls_writer_create(u_int32_t bufsize)
{
	private_tls_writer_t *this;

	INIT(this,
		.public = {
			.write_uint8 = _write_uint8,
			.write_uint16 = _write_uint16,
			.write_uint24 = _write_uint24,
			.write_uint32 = _write_uint32,
			.write_data = _write_data,
			.write_data8 = _write_data8,
			.write_data16 = _write_data16,
			.write_data24 = _write_data24,
			.write_data32 = _write_data32,
			.wrap8 = _wrap8,
			.wrap16 = _wrap16,
			.wrap24 = _wrap24,
			.wrap32 = _wrap32,
			.get_buf = _get_buf,
			.destroy = _destroy,
		},
		.increase = bufsize ? max(bufsize, 4) : 32,
	);
	if (bufsize)
	{
		this->buf = chunk_alloc(bufsize);
	}

	return &this->public;
}
