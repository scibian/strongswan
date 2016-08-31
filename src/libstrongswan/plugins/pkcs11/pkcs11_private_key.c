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

#include "pkcs11_private_key.h"

#include "pkcs11_library.h"
#include "pkcs11_manager.h"

#include <debug.h>
#include <threading/mutex.h>

typedef struct private_pkcs11_private_key_t private_pkcs11_private_key_t;

/**
 * Private data of an pkcs11_private_key_t object.
 */
struct private_pkcs11_private_key_t {

	/**
	 * Public pkcs11_private_key_t interface.
	 */
	pkcs11_private_key_t public;

	/**
	 * PKCS#11 module
	 */
	pkcs11_library_t *lib;

	/**
	 * Token session
	 */
	CK_SESSION_HANDLE session;

	/**
	 * Mutex to lock session
	 */
	mutex_t *mutex;

	/**
	 * Key object on the token
	 */
	CK_OBJECT_HANDLE object;

	/**
	 * Key requires reauthentication for each signature/decryption
	 */
	CK_BBOOL reauth;

	/**
	 * Keyid of the key we use
	 */
	identification_t *keyid;

	/**
	 * Associated public key
	 */
	public_key_t *pubkey;

	/**
	 * References to this key
	 */
	refcount_t ref;
};

METHOD(private_key_t, get_type, key_type_t,
	private_pkcs11_private_key_t *this)
{
	return this->pubkey->get_type(this->pubkey);
}

METHOD(private_key_t, get_keysize, int,
	private_pkcs11_private_key_t *this)
{
	return this->pubkey->get_keysize(this->pubkey);
}

/**
 * See header.
 */
CK_MECHANISM_PTR pkcs11_signature_scheme_to_mech(signature_scheme_t scheme)
{
	static struct {
		signature_scheme_t scheme;
		CK_MECHANISM mechanism;
	} mappings[] = {
		{SIGN_RSA_EMSA_PKCS1_NULL,		{CKM_RSA_PKCS,				NULL, 0}},
		{SIGN_RSA_EMSA_PKCS1_SHA1,		{CKM_SHA1_RSA_PKCS,			NULL, 0}},
		{SIGN_RSA_EMSA_PKCS1_SHA256,	{CKM_SHA256_RSA_PKCS,		NULL, 0}},
		{SIGN_RSA_EMSA_PKCS1_SHA384,	{CKM_SHA384_RSA_PKCS,		NULL, 0}},
		{SIGN_RSA_EMSA_PKCS1_SHA512,	{CKM_SHA512_RSA_PKCS,		NULL, 0}},
		{SIGN_RSA_EMSA_PKCS1_MD5,		{CKM_MD5_RSA_PKCS,			NULL, 0}},
	};
	int i;

	for (i = 0; i < countof(mappings); i++)
	{
		if (mappings[i].scheme == scheme)
		{
			return &mappings[i].mechanism;
		}
	}
	return NULL;
}

/**
 * See header.
 */
CK_MECHANISM_PTR pkcs11_encryption_scheme_to_mech(encryption_scheme_t scheme)
{
	static struct {
		encryption_scheme_t scheme;
		CK_MECHANISM mechanism;
	} mappings[] = {
		{ENCRYPT_RSA_PKCS1,			{CKM_RSA_PKCS,				NULL, 0}},
		{ENCRYPT_RSA_OAEP_SHA1,		{CKM_RSA_PKCS_OAEP,			NULL, 0}},
	};
	int i;

	for (i = 0; i < countof(mappings); i++)
	{
		if (mappings[i].scheme == scheme)
		{
			return &mappings[i].mechanism;
		}
	}
	return NULL;
}

/**
 * Reauthenticate to do a signature
 */
static bool reauth(private_pkcs11_private_key_t *this)
{
	enumerator_t *enumerator;
	shared_key_t *shared;
	chunk_t pin;
	CK_RV rv;
	bool found = FALSE, success = FALSE;

	enumerator = lib->credmgr->create_shared_enumerator(lib->credmgr,
												SHARED_PIN, this->keyid, NULL);
	while (enumerator->enumerate(enumerator, &shared, NULL, NULL))
	{
		found = TRUE;
		pin = shared->get_key(shared);
		rv = this->lib->f->C_Login(this->session, CKU_CONTEXT_SPECIFIC,
								   pin.ptr, pin.len);
		if (rv == CKR_OK)
		{
			success = TRUE;
			break;
		}
		DBG1(DBG_CFG, "reauthentication login failed: %N", ck_rv_names, rv);
	}
	enumerator->destroy(enumerator);

	if (!found)
	{
		DBG1(DBG_CFG, "private key requires reauthentication, but no PIN found");
		return FALSE;
	}
	return success;
}

METHOD(private_key_t, sign, bool,
	private_pkcs11_private_key_t *this, signature_scheme_t scheme,
	chunk_t data, chunk_t *signature)
{
	CK_MECHANISM_PTR mechanism;
	CK_BYTE_PTR buf;
	CK_ULONG len;
	CK_RV rv;

	mechanism = pkcs11_signature_scheme_to_mech(scheme);
	if (!mechanism)
	{
		DBG1(DBG_LIB, "signature scheme %N not supported",
			 signature_scheme_names, scheme);
		return FALSE;
	}
	this->mutex->lock(this->mutex);
	rv = this->lib->f->C_SignInit(this->session, mechanism, this->object);
	if (this->reauth && !reauth(this))
	{
		return FALSE;
	}
	if (rv != CKR_OK)
	{
		this->mutex->unlock(this->mutex);
		DBG1(DBG_LIB, "C_SignInit() failed: %N", ck_rv_names, rv);
		return FALSE;
	}
	len = (get_keysize(this) + 7) / 8;
	buf = malloc(len);
	rv = this->lib->f->C_Sign(this->session, data.ptr, data.len, buf, &len);
	this->mutex->unlock(this->mutex);
	if (rv != CKR_OK)
	{
		DBG1(DBG_LIB, "C_Sign() failed: %N", ck_rv_names, rv);
		free(buf);
		return FALSE;
	}
	*signature = chunk_create(buf, len);
	return TRUE;
}

METHOD(private_key_t, decrypt, bool,
	private_pkcs11_private_key_t *this, encryption_scheme_t scheme,
	chunk_t crypt, chunk_t *plain)
{
	CK_MECHANISM_PTR mechanism;
	CK_BYTE_PTR buf;
	CK_ULONG len;
	CK_RV rv;

	mechanism = pkcs11_encryption_scheme_to_mech(scheme);
	if (!mechanism)
	{
		DBG1(DBG_LIB, "encryption scheme %N not supported",
			 encryption_scheme_names, scheme);
		return FALSE;
	}
	this->mutex->lock(this->mutex);
	rv = this->lib->f->C_DecryptInit(this->session, mechanism, this->object);
	if (this->reauth && !reauth(this))
	{
		return FALSE;
	}
	if (rv != CKR_OK)
	{
		this->mutex->unlock(this->mutex);
		DBG1(DBG_LIB, "C_DecryptInit() failed: %N", ck_rv_names, rv);
		return FALSE;
	}
	len = (get_keysize(this) + 7) / 8;
	buf = malloc(len);
	rv = this->lib->f->C_Decrypt(this->session, crypt.ptr, crypt.len, buf, &len);
	this->mutex->unlock(this->mutex);
	if (rv != CKR_OK)
	{
		DBG1(DBG_LIB, "C_Decrypt() failed: %N", ck_rv_names, rv);
		free(buf);
		return FALSE;
	}
	*plain = chunk_create(buf, len);
	return TRUE;
}

METHOD(private_key_t, get_public_key, public_key_t*,
	private_pkcs11_private_key_t *this)
{
	return this->pubkey->get_ref(this->pubkey);
}

METHOD(private_key_t, get_fingerprint, bool,
	private_pkcs11_private_key_t *this, cred_encoding_type_t type,
	chunk_t *fingerprint)
{
	return this->pubkey->get_fingerprint(this->pubkey, type, fingerprint);
}

METHOD(private_key_t, get_encoding, bool,
	private_pkcs11_private_key_t *this, cred_encoding_type_t type,
	chunk_t *encoding)
{
	return FALSE;
}

METHOD(private_key_t, get_ref, private_key_t*,
	private_pkcs11_private_key_t *this)
{
	ref_get(&this->ref);
	return &this->public.key;
}

METHOD(private_key_t, destroy, void,
	private_pkcs11_private_key_t *this)
{
	if (ref_put(&this->ref))
	{
		if (this->pubkey)
		{
			this->pubkey->destroy(this->pubkey);
		}
		this->mutex->destroy(this->mutex);
		this->keyid->destroy(this->keyid);
		this->lib->f->C_CloseSession(this->session);
		free(this);
	}
}

/**
 * Find the PKCS#11 library by its friendly name
 */
static pkcs11_library_t* find_lib(char *module)
{
	pkcs11_manager_t *manager;
	enumerator_t *enumerator;
	pkcs11_library_t *p11, *found = NULL;
	CK_SLOT_ID slot;

	manager = pkcs11_manager_get();
	if (!manager)
	{
		return NULL;
	}
	enumerator = manager->create_token_enumerator(manager);
	while (enumerator->enumerate(enumerator, &p11, &slot))
	{
		if (streq(module, p11->get_name(p11)))
		{
			found = p11;
			break;
		}
	}
	enumerator->destroy(enumerator);
	return found;
}

/**
 * Find the PKCS#11 lib having a keyid, and optionally a slot
 */
static pkcs11_library_t* find_lib_by_keyid(chunk_t keyid, int *slot)
{
	pkcs11_manager_t *manager;
	enumerator_t *enumerator;
	pkcs11_library_t *p11, *found = NULL;
	CK_SLOT_ID current;

	manager = pkcs11_manager_get();
	if (!manager)
	{
		return NULL;
	}
	enumerator = manager->create_token_enumerator(manager);
	while (enumerator->enumerate(enumerator, &p11, &current))
	{
		if (*slot == -1 || *slot == current)
		{
			/* we look for a public key, it is usually readable without login */
			CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
			CK_ATTRIBUTE tmpl[] = {
				{CKA_CLASS, &class, sizeof(class)},
				{CKA_ID, keyid.ptr, keyid.len},
			};
			CK_OBJECT_HANDLE object;
			CK_SESSION_HANDLE session;
			CK_RV rv;
			enumerator_t *keys;

			rv = p11->f->C_OpenSession(current, CKF_SERIAL_SESSION, NULL, NULL,
									   &session);
			if (rv != CKR_OK)
			{
				DBG1(DBG_CFG, "opening PKCS#11 session failed: %N",
					 ck_rv_names, rv);
				continue;
			}
			keys = p11->create_object_enumerator(p11, session,
												 tmpl, countof(tmpl), NULL, 0);
			if (keys->enumerate(keys, &object))
			{
				DBG1(DBG_CFG, "found key on PKCS#11 token '%s':%d",
					 p11->get_name(p11), current);
				found = p11;
				*slot = current;
			}
			keys->destroy(keys);
			p11->f->C_CloseSession(session);
			if (found)
			{
				break;
			}
		}
	}
	enumerator->destroy(enumerator);
	return found;
}

/**
 * Find the key on the token
 */
static bool find_key(private_pkcs11_private_key_t *this, chunk_t keyid)
{
	CK_OBJECT_CLASS class = CKO_PRIVATE_KEY;
	CK_ATTRIBUTE tmpl[] = {
		{CKA_CLASS, &class, sizeof(class)},
		{CKA_ID, keyid.ptr, keyid.len},
	};
	CK_OBJECT_HANDLE object;
	CK_KEY_TYPE type;
	CK_BBOOL reauth = FALSE;
	CK_ATTRIBUTE attr[] = {
		{CKA_KEY_TYPE, &type, sizeof(type)},
		{CKA_MODULUS, NULL, 0},
		{CKA_PUBLIC_EXPONENT, NULL, 0},
		{CKA_ALWAYS_AUTHENTICATE, &reauth, sizeof(reauth)},
	};
	enumerator_t *enumerator;
	chunk_t modulus, pubexp;
	int count = countof(attr);

	/* do not use CKA_ALWAYS_AUTHENTICATE if not supported */
	if (!(this->lib->get_features(this->lib) & PKCS11_ALWAYS_AUTH_KEYS))
	{
		count--;
	}
	enumerator = this->lib->create_object_enumerator(this->lib,
							this->session, tmpl, countof(tmpl), attr, count);
	if (enumerator->enumerate(enumerator, &object))
	{
		switch (type)
		{
			case CKK_RSA:
				if (attr[1].ulValueLen == -1 || attr[2].ulValueLen == -1)
				{
					DBG1(DBG_CFG, "reading modulus/exponent from PKCS#1 failed");
					break;
				}
				modulus = chunk_create(attr[1].pValue, attr[1].ulValueLen);
				pubexp = chunk_create(attr[2].pValue, attr[2].ulValueLen);
				this->pubkey = lib->creds->create(lib->creds, CRED_PUBLIC_KEY,
									KEY_RSA, BUILD_RSA_MODULUS, modulus,
									BUILD_RSA_PUB_EXP, pubexp, BUILD_END);
				if (!this->pubkey)
				{
					DBG1(DBG_CFG, "extracting public key from PKCS#11 RSA "
						 "private key failed");
				}
				this->reauth = reauth;
				this->object = object;
				break;
			default:
				DBG1(DBG_CFG, "PKCS#11 key type %d not supported", type);
				break;
		}
	}
	enumerator->destroy(enumerator);
	return this->pubkey != NULL;
}

/**
 * Find a PIN and try to log in
 */
static bool login(private_pkcs11_private_key_t *this, int slot)
{
	enumerator_t *enumerator;
	shared_key_t *shared;
	chunk_t pin;
	CK_RV rv;
	CK_SESSION_INFO info;
	bool found = FALSE, success = FALSE;

	rv = this->lib->f->C_GetSessionInfo(this->session, &info);
	if (rv != CKR_OK)
	{
		DBG1(DBG_CFG, "C_GetSessionInfo failed: %N", ck_rv_names, rv);
		return FALSE;
	}
	if (info.state != CKS_RO_PUBLIC_SESSION &&
		info.state != CKS_RW_PUBLIC_SESSION)
	{	/* already logged in with another session, skip */
		return TRUE;
	}

	enumerator = lib->credmgr->create_shared_enumerator(lib->credmgr,
												SHARED_PIN, this->keyid, NULL);
	while (enumerator->enumerate(enumerator, &shared, NULL, NULL))
	{
		found = TRUE;
		pin = shared->get_key(shared);
		rv = this->lib->f->C_Login(this->session, CKU_USER, pin.ptr, pin.len);
		if (rv == CKR_OK)
		{
			success = TRUE;
			break;
		}
		DBG1(DBG_CFG, "login to '%s':%d failed: %N",
			 this->lib->get_name(this->lib), slot, ck_rv_names, rv);
	}
	enumerator->destroy(enumerator);

	if (!found)
	{
		DBG1(DBG_CFG, "no PIN found for PKCS#11 key %Y", this->keyid);
		return FALSE;
	}
	return success;
}

/**
 * See header.
 */
pkcs11_private_key_t *pkcs11_private_key_connect(key_type_t type, va_list args)
{
	private_pkcs11_private_key_t *this;
	char *module = NULL;
	chunk_t keyid = chunk_empty;
	int slot = -1;
	CK_RV rv;

	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_PKCS11_KEYID:
				keyid = va_arg(args, chunk_t);
				continue;
			case BUILD_PKCS11_SLOT:
				slot = va_arg(args, int);
				continue;
			case BUILD_PKCS11_MODULE:
				module = va_arg(args, char*);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}
	if (!keyid.len)
	{
		return NULL;
	}

	INIT(this,
		.public = {
			.key = {
				.get_type = _get_type,
				.sign = _sign,
				.decrypt = _decrypt,
				.get_keysize = _get_keysize,
				.get_public_key = _get_public_key,
				.equals = private_key_equals,
				.belongs_to = private_key_belongs_to,
				.get_fingerprint = _get_fingerprint,
				.has_fingerprint = private_key_has_fingerprint,
				.get_encoding = _get_encoding,
				.get_ref = _get_ref,
				.destroy = _destroy,
			},
		},
		.ref = 1,
	);

	if (module && slot != -1)
	{
		this->lib = find_lib(module);
		if (!this->lib)
		{
			DBG1(DBG_CFG, "PKCS#11 module '%s' not found", module);
			free(this);
			return NULL;
		}
	}
	else
	{
		this->lib = find_lib_by_keyid(keyid, &slot);
		if (!this->lib)
		{
			DBG1(DBG_CFG, "no PKCS#11 module found having a keyid %#B", &keyid);
			free(this);
			return NULL;
		}
	}

	rv = this->lib->f->C_OpenSession(slot, CKF_SERIAL_SESSION,
									 NULL, NULL, &this->session);
	if (rv != CKR_OK)
	{
		DBG1(DBG_CFG, "opening private key session on '%s':%d failed: %N",
			 module, slot, ck_rv_names, rv);
		free(this);
		return NULL;
	}

	this->mutex = mutex_create(MUTEX_TYPE_DEFAULT);
	this->keyid = identification_create_from_encoding(ID_KEY_ID, keyid);

	if (!login(this, slot))
	{
		destroy(this);
		return NULL;
	}

	if (!find_key(this, keyid))
	{
		destroy(this);
		return NULL;
	}

	return &this->public;
}
