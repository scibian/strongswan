/*
 * Copyright (C) 2010 Andreas Steffen
 * HSR Hochschule fuer Technik Rapperswil
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

#include "tnccs_11.h"
#include "batch/tnccs_batch.h"
#include "messages/tnccs_msg.h"
#include "messages/imc_imv_msg.h"
#include "messages/tnccs_error_msg.h"
#include "messages/tnccs_preferred_language_msg.h"
#include "messages/tnccs_reason_strings_msg.h"
#include "messages/tnccs_recommendation_msg.h"

#include <daemon.h>
#include <debug.h>
#include <threading/mutex.h>
#include <tnc/tncif.h>
#include <tnc/tncifimv.h>
#include <tnc/tnccs/tnccs.h>

typedef struct private_tnccs_11_t private_tnccs_11_t;

/**
 * Private data of a tnccs_11_t object.
 */
struct private_tnccs_11_t {

	/**
	 * Public tls_t interface.
	 */
	tls_t public;

	/**
	 * TNCC if TRUE, TNCS if FALSE
	 */
	bool is_server;

	/**
	 * Connection ID assigned to this TNCCS connection
	 */
	TNC_ConnectionID connection_id;

	/**
	 * Last TNCCS batch ID
	 */
	int batch_id;

	/**
	 * TNCCS batch being constructed
	 */
	tnccs_batch_t *batch;

	/**
	 * Mutex locking the batch in construction
	 */
	mutex_t *mutex;

	/**
	 * Flag set while processing
	 */
	bool fatal_error;

	/**
	 * Flag set by TNCCS-Recommendation message
	 */
	bool delete_state;

	/**
	 * SendMessage() by IMC/IMV only allowed if flag is set
	 */
	bool send_msg;

	/**
	 * Flag set by IMC/IMV RequestHandshakeRetry() function
	 */
	bool request_handshake_retry;

	/**
	 * Set of IMV recommendations  (TNC Server only)
	 */
	recommendations_t *recs;
};

METHOD(tnccs_t, send_msg, TNC_Result,
	private_tnccs_11_t* this, TNC_IMCID imc_id, TNC_IMVID imv_id,
							  TNC_BufferReference msg,
							  TNC_UInt32 msg_len,
							  TNC_MessageType msg_type)
{
	tnccs_msg_t *tnccs_msg;

	if (!this->send_msg)
	{
		DBG1(DBG_TNC, "%s %u not allowed to call SendMessage()",
			this->is_server ? "IMV" : "IMC",
			this->is_server ? imv_id : imc_id);
		return TNC_RESULT_ILLEGAL_OPERATION;
	}
	tnccs_msg = imc_imv_msg_create(msg_type, chunk_create(msg, msg_len));

	/* adding an IMC-IMV Message to TNCCS batch */
	this->mutex->lock(this->mutex);
	if (!this->batch)
	{
		this->batch = tnccs_batch_create(this->is_server, ++this->batch_id);
	}
	this->batch->add_msg(this->batch, tnccs_msg);
	this->mutex->unlock(this->mutex);
	return TNC_RESULT_SUCCESS;
}

/**
 * Handle a single TNCCS message according to its type
 */
static void handle_message(private_tnccs_11_t *this, tnccs_msg_t *msg)
{
	switch (msg->get_type(msg))
	{
		case IMC_IMV_MSG:
		{
			imc_imv_msg_t *imc_imv_msg;
			TNC_MessageType msg_type;
			chunk_t msg_body;

			imc_imv_msg = (imc_imv_msg_t*)msg;
			msg_type = imc_imv_msg->get_msg_type(imc_imv_msg);
			msg_body = imc_imv_msg->get_msg_body(imc_imv_msg);

			DBG2(DBG_TNC, "handling IMC_IMV message type 0x%08x", msg_type);

			this->send_msg = TRUE;
			if (this->is_server)
			{
				charon->imvs->receive_message(charon->imvs,
				this->connection_id, msg_body.ptr, msg_body.len, msg_type);
			}
			else
			{
				charon->imcs->receive_message(charon->imcs,
				this->connection_id, msg_body.ptr, msg_body.len,msg_type);
			}
			this->send_msg = FALSE;
			break;
		}
		case TNCCS_MSG_RECOMMENDATION:
		{
			tnccs_recommendation_msg_t *rec_msg;
			TNC_IMV_Action_Recommendation rec;
			TNC_ConnectionState state = TNC_CONNECTION_STATE_ACCESS_NONE;

			rec_msg = (tnccs_recommendation_msg_t*)msg;
			rec = rec_msg->get_recommendation(rec_msg);
			if (this->is_server)
			{
				DBG1(DBG_TNC, "ignoring NCCS-Recommendation message from "
							  " TNC client");
				break;
			}
			DBG1(DBG_TNC, "TNC recommendation is '%N'",
				 TNC_IMV_Action_Recommendation_names, rec);
			switch (rec)
			{
				case TNC_IMV_ACTION_RECOMMENDATION_ALLOW:
					state = TNC_CONNECTION_STATE_ACCESS_ALLOWED;
					break;
				case TNC_IMV_ACTION_RECOMMENDATION_ISOLATE:
					state = TNC_CONNECTION_STATE_ACCESS_ISOLATED;
					break;
				case TNC_IMV_ACTION_RECOMMENDATION_NO_ACCESS:
				default:
					state = TNC_CONNECTION_STATE_ACCESS_NONE;
			}
			charon->imcs->notify_connection_change(charon->imcs,
												   this->connection_id, state);
			this->delete_state = TRUE;
			break;
		}
		case TNCCS_MSG_ERROR:
		{
			tnccs_error_msg_t *err_msg;
			tnccs_error_type_t error_type;
			char *error_msg;

			err_msg = (tnccs_error_msg_t*)msg;
			error_msg = err_msg->get_message(err_msg, &error_type);
			DBG1(DBG_TNC, "received '%N' TNCCS-Error: %s",
				 tnccs_error_type_names, error_type, error_msg);

			/* we assume that all errors are fatal */
			this->fatal_error = TRUE;
			break;
		}
		case TNCCS_MSG_PREFERRED_LANGUAGE:
		{
			tnccs_preferred_language_msg_t *lang_msg;
			char *lang;

			lang_msg = (tnccs_preferred_language_msg_t*)msg;
			lang = lang_msg->get_preferred_language(lang_msg);

			DBG2(DBG_TNC, "setting preferred language to '%s'", lang);
			this->recs->set_preferred_language(this->recs,
									chunk_create(lang, strlen(lang)));
			break;
		}
		case TNCCS_MSG_REASON_STRINGS:
		{
			tnccs_reason_strings_msg_t *reason_msg;
			chunk_t reason_string, reason_lang;

			reason_msg = (tnccs_reason_strings_msg_t*)msg;
			reason_string = reason_msg->get_reason(reason_msg, &reason_lang);
			DBG2(DBG_TNC, "reason string is '%.*s",   reason_string.len,
													  reason_string.ptr);
			DBG2(DBG_TNC, "reason language is '%.*s", reason_lang.len,
													  reason_lang.ptr);
			break;
		}
		default:
			break;
	}
}

METHOD(tls_t, process, status_t,
	private_tnccs_11_t *this, void *buf, size_t buflen)
{
	chunk_t data;
	tnccs_batch_t *batch;
	tnccs_msg_t *msg;
	enumerator_t *enumerator;
	status_t status;

	if (this->is_server && !this->connection_id)
	{
		this->connection_id = charon->tnccs->create_connection(charon->tnccs,
								(tnccs_t*)this,	_send_msg,
								&this->request_handshake_retry, &this->recs);
		if (!this->connection_id)
		{
			return FAILED;
		}
		charon->imvs->notify_connection_change(charon->imvs,
							this->connection_id, TNC_CONNECTION_STATE_CREATE);
		charon->imvs->notify_connection_change(charon->imvs,
							this->connection_id, TNC_CONNECTION_STATE_HANDSHAKE);
	}

	data = chunk_create(buf, buflen);
	DBG1(DBG_TNC, "received TNCCS Batch (%u bytes) for Connection ID %u",
				   data.len, this->connection_id);
	DBG3(DBG_TNC, "%.*s", data.len, data.ptr);
	batch = tnccs_batch_create_from_data(this->is_server, ++this->batch_id, data);
	status = batch->process(batch);

	if (status == FAILED)
	{
		this->fatal_error = TRUE;
		this->mutex->lock(this->mutex);
		if (this->batch)
		{
			DBG1(DBG_TNC, "cancelling TNCCS batch");
			this->batch->destroy(this->batch);
			this->batch_id--;
		 }
		this->batch = tnccs_batch_create(this->is_server, ++this->batch_id);

		/* add error messages to outbound batch */
		enumerator = batch->create_error_enumerator(batch);
		while (enumerator->enumerate(enumerator, &msg))
		{
			this->batch->add_msg(this->batch, msg->get_ref(msg));
		}
		enumerator->destroy(enumerator);
		this->mutex->unlock(this->mutex);
	}
	else
	{
		enumerator = batch->create_msg_enumerator(batch);
		while (enumerator->enumerate(enumerator, &msg))
		{
			handle_message(this, msg);
		}
		enumerator->destroy(enumerator);

		/* received any TNCCS-Error messages */
		if (this->fatal_error)
		{
			DBG1(DBG_TNC, "a fatal TNCCS-Error occurred, terminating connection");
			batch->destroy(batch);
			return FAILED;
		}

		this->send_msg = TRUE;
		if (this->is_server)
		{
			charon->imvs->batch_ending(charon->imvs, this->connection_id);
		}
		else
		{
			charon->imcs->batch_ending(charon->imcs, this->connection_id);
		}
		this->send_msg = FALSE;
	}
	batch->destroy(batch);

	return NEED_MORE;
}

/**
 *  Add a recommendation message if a final recommendation is available
 */
static void check_and_build_recommendation(private_tnccs_11_t *this)
{
	TNC_IMV_Action_Recommendation rec;
	TNC_IMV_Evaluation_Result eval;
	TNC_IMVID id;
	chunk_t reason, language;
	enumerator_t *enumerator;
	tnccs_msg_t *msg;

	if (!this->recs->have_recommendation(this->recs, &rec, &eval))
	{
		charon->imvs->solicit_recommendation(charon->imvs, this->connection_id);
	}
	if (this->recs->have_recommendation(this->recs, &rec, &eval))
	{
		if (!this->batch)
		{
			this->batch = tnccs_batch_create(this->is_server, ++this->batch_id);
		}

		msg = tnccs_recommendation_msg_create(rec);
		this->batch->add_msg(this->batch, msg);

		/* currently we just send the first Reason String */
		enumerator = this->recs->create_reason_enumerator(this->recs);
		if (enumerator->enumerate(enumerator, &id, &reason, &language))
		{
			msg = tnccs_reason_strings_msg_create(reason, language);
			this->batch->add_msg(this->batch, msg);
		}
		enumerator->destroy(enumerator);

		/* we have reache the final state */
		this->delete_state = TRUE;
	}
}

METHOD(tls_t, build, status_t,
	private_tnccs_11_t *this, void *buf, size_t *buflen, size_t *msglen)
{
	status_t status;

	/* Initialize the connection */
	if (!this->is_server && !this->connection_id)
	{
		tnccs_msg_t *msg;
		char *pref_lang;

		this->connection_id = charon->tnccs->create_connection(charon->tnccs,
										(tnccs_t*)this, _send_msg,
										&this->request_handshake_retry, NULL);
		if (!this->connection_id)
		{
			return FAILED;
		}

		/* Create TNCCS-PreferredLanguage message */
		pref_lang = charon->imcs->get_preferred_language(charon->imcs);
		msg = tnccs_preferred_language_msg_create(pref_lang);
		this->mutex->lock(this->mutex);
		this->batch = tnccs_batch_create(this->is_server, ++this->batch_id);
		this->batch->add_msg(this->batch, msg);
		this->mutex->unlock(this->mutex);

		charon->imcs->notify_connection_change(charon->imcs,
							this->connection_id, TNC_CONNECTION_STATE_CREATE);
		charon->imcs->notify_connection_change(charon->imcs,
							this->connection_id, TNC_CONNECTION_STATE_HANDSHAKE);
		this->send_msg = TRUE;
		charon->imcs->begin_handshake(charon->imcs, this->connection_id);
		this->send_msg = FALSE;
	}

	/* Do not allow any asynchronous IMCs or IMVs to add additional messages */
	this->mutex->lock(this->mutex);

	if (this->recs && !this->delete_state &&
	   (!this->batch || this->fatal_error))
	{
		check_and_build_recommendation(this);
	}

	if (this->batch)
	{
		chunk_t data;

		this->batch->build(this->batch);
		data = this->batch->get_encoding(this->batch);
		DBG1(DBG_TNC, "sending TNCCS Batch (%d bytes) for Connection ID %u",
					   data.len, this->connection_id);
		DBG3(DBG_TNC, "%.*s", data.len, data.ptr);
		*msglen = data.len;

		if (data.len > *buflen)
		{
			DBG1(DBG_TNC, "fragmentation of TNCCS batch not supported yet");
		}
		else
		{
			*buflen = data.len;
		}
		memcpy(buf, data.ptr, *buflen);
		this->batch->destroy(this->batch);
		this->batch = NULL;
		status = ALREADY_DONE;
	}
	else
	{
		DBG1(DBG_TNC, "no TNCCS Batch to send");
		status = INVALID_STATE;
	}
	this->mutex->unlock(this->mutex);

	return status;
}

METHOD(tls_t, is_server, bool,
	private_tnccs_11_t *this)
{
	return this->is_server;
}

METHOD(tls_t, get_purpose, tls_purpose_t,
	private_tnccs_11_t *this)
{
	return TLS_PURPOSE_EAP_TNC;
}

METHOD(tls_t, is_complete, bool,
	private_tnccs_11_t *this)
{
	TNC_IMV_Action_Recommendation rec;
	TNC_IMV_Evaluation_Result eval;

	if (this->recs && this->recs->have_recommendation(this->recs, &rec, &eval))
	{
		return charon->imvs->enforce_recommendation(charon->imvs, rec, eval);
	}
	else
	{
		return FALSE;
	}
}

METHOD(tls_t, get_eap_msk, chunk_t,
	private_tnccs_11_t *this)
{
	return chunk_empty;
}

METHOD(tls_t, destroy, void,
	private_tnccs_11_t *this)
{
	charon->tnccs->remove_connection(charon->tnccs, this->connection_id,
													this->is_server);
	this->mutex->destroy(this->mutex);
	DESTROY_IF(this->batch);
	free(this);
}

/**
 * See header
 */
tls_t *tnccs_11_create(bool is_server)
{
	private_tnccs_11_t *this;

	INIT(this,
		.public = {
			.process = _process,
			.build = _build,
			.is_server = _is_server,
			.get_purpose = _get_purpose,
			.is_complete = _is_complete,
			.get_eap_msk = _get_eap_msk,
			.destroy = _destroy,
		},
		.is_server = is_server,
		.mutex = mutex_create(MUTEX_TYPE_DEFAULT),
	);

	return &this->public;
}
