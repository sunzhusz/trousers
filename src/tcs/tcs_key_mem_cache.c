
/*
 * Licensed Materials - Property of IBM
 *
 * trousers - An open source TCG Software Stack
 *
 * (C) Copyright International Business Machines Corp. 2004-2006
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "trousers/tss.h"
#include "trousers_types.h"
#include "spi_internal_types.h"
#include "tcs_internal_types.h"
#include "tcs_tsp.h"
#include "tcs_utils.h"
#include "tcs_int_literals.h"
#include "capabilities.h"
#include "tcslog.h"
#include "tcsps.h"
#include "req_mgr.h"

#include "tcs_key_ps.h"

/*
 * mem_cache_lock will be responsible for protecting the key_mem_cache_head list. This is a
 * TCSD global linked list of all keys which have been loaded into the TPM at some time.
 */
pthread_mutex_t mem_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * tcs_keyhandle_lock is only used to make TCS keyhandle generation atomic for all TCSD
 * threads.
 */
static pthread_mutex_t tcs_keyhandle_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * timestamp_lock is only used to make TCS key timestamp generation atomic for all TCSD
 * threads.
 */
static pthread_mutex_t timestamp_lock = PTHREAD_MUTEX_INITIALIZER;

TCS_KEY_HANDLE
getNextTcsKeyHandle()
{
	static TCS_KEY_HANDLE NextTcsKeyHandle = 0x22330000;
	TCS_KEY_HANDLE ret;

	pthread_mutex_lock(&tcs_keyhandle_lock);

	do {
		ret = NextTcsKeyHandle++;
	} while (NextTcsKeyHandle == SRK_TPM_HANDLE);

	pthread_mutex_unlock(&tcs_keyhandle_lock);

	return ret;
}

UINT32
getNextTimeStamp()
{
	static UINT32 time_stamp = 1;
	UINT32 ret;

	pthread_mutex_lock(&timestamp_lock);
	ret = time_stamp++;
	pthread_mutex_unlock(&timestamp_lock);

	return ret;
}

/* only called from load key paths, so no locking */
TCPA_STORE_PUBKEY *
mc_get_pub_by_slot(TCPA_KEY_HANDLE tpm_handle)
{
	struct key_mem_cache *tmp;
	TCPA_STORE_PUBKEY *ret;

	if (tpm_handle == NULL_TPM_HANDLE)
		return NULL;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x",
			   tmp->tcs_handle);
		if (tmp->tpm_handle == tpm_handle) {
			ret = &tmp->blob->pubKey;
			return ret;
		}
	}
	LogDebugFn("returning NULL TCPA_STORE_PUBKEY");
	return NULL;
}

/* only called from load key paths, so no locking */
TCPA_STORE_PUBKEY *
mc_get_pub_by_handle(TCS_KEY_HANDLE tcs_handle)
{
	struct key_mem_cache *tmp;
	TCPA_STORE_PUBKEY *ret;

	LogDebugFn("looking for 0x%x", tcs_handle);

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x",
			 tmp->tcs_handle);
		if (tmp->tcs_handle == tcs_handle) {
			ret = &tmp->blob->pubKey;
			return ret;
		}
	}

	LogDebugFn("returning NULL TCPA_STORE_PUBKEY");
	return NULL;
}

/* only called from load key paths, so no locking */
TSS_RESULT
mc_set_parent_by_handle(TCS_KEY_HANDLE tcs_handle, TCS_KEY_HANDLE p_tcs_handle)
{
	struct key_mem_cache *tmp, *parent;

	/* find parent */
	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebug("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tcs_handle == p_tcs_handle) {
			parent = tmp;
			break;
		}
	}

	/* didn't find parent */
	if (tmp == NULL)
		goto done;

	/* set parent blob in child */
	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		if (tmp->tcs_handle == tcs_handle) {
			tmp->parent = parent;
			return TSS_SUCCESS;
		}
	}
done:
	return TCSERR(TSS_E_FAIL);
}

TCPA_RESULT
ensureKeyIsLoaded(TCS_CONTEXT_HANDLE hContext, TCS_KEY_HANDLE keyHandle, TCPA_KEY_HANDLE * keySlot)
{
	TCPA_RESULT result = TSS_SUCCESS;
	TCPA_STORE_PUBKEY *myPub;

	LogDebugFn("0x%x", keyHandle);

	pthread_mutex_lock(&mem_cache_lock);

	*keySlot = mc_get_slot_by_handle(keyHandle);
	LogDebug("keySlot is %08X", *keySlot);
	if (*keySlot == NULL_TPM_HANDLE || isKeyLoaded(*keySlot) == FALSE) {
		LogDebug("calling mc_get_pub_by_handle");
		if ((myPub = mc_get_pub_by_handle(keyHandle)) == NULL) {
			LogDebug("Failed to find pub by handle");
			result = TCSERR(TCS_E_KM_LOADFAILED);
			goto done;
		}

		LogDebugFn("calling LoadKeyShim");
		if ((result = LoadKeyShim(hContext, myPub, NULL, keySlot))) {
			LogDebug("Failed shim");
			goto done;
		}

		if (*keySlot == NULL_TPM_HANDLE) {
			LogDebug("Key slot is still invalid after ensureKeyIsLoaded");
			result = TCSERR(TCS_E_KM_LOADFAILED);
			goto done;
		}
	}
	mc_update_time_stamp(*keySlot);

done:
	pthread_mutex_unlock(&mem_cache_lock);
	LogDebugFn("Exit");
	return result;
}


/* only called from load key paths, so no locking */
TSS_UUID *
mc_get_uuid_by_pub(TCPA_STORE_PUBKEY *pub)
{
	TSS_UUID *ret;
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->blob->pubKey.keyLength == pub->keyLength &&
		    !memcmp(tmp->blob->pubKey.key, pub->key, pub->keyLength)) {
			ret = &tmp->uuid;
			return ret;
		}
	}

	return NULL;
}

TSS_RESULT
mc_get_handles_by_uuid(TSS_UUID *uuid, TCS_KEY_HANDLE *tcsHandle, TCPA_KEY_HANDLE *slot)
{
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		if (!memcmp(&tmp->uuid, uuid, sizeof(TSS_UUID))) {
			*tcsHandle = tmp->tcs_handle;
			*slot = tmp->tpm_handle;
			return TSS_SUCCESS;
		}
	}

	return TCSERR(TSS_E_FAIL);
}

TCS_KEY_HANDLE
mc_get_handle_by_encdata(BYTE *encData)
{
	struct key_mem_cache *tmp;
	TCS_KEY_HANDLE ret;

	pthread_mutex_lock(&mem_cache_lock);

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->blob->encSize == 0)
			continue;
		if (!memcmp(tmp->blob->encData, encData, tmp->blob->encSize)) {
			ret = tmp->tcs_handle;
			pthread_mutex_unlock(&mem_cache_lock);
			return ret;
		}
	}
	pthread_mutex_unlock(&mem_cache_lock);
	return 0;
}

TSS_RESULT
mc_update_encdata(BYTE *encData, BYTE *newEncData)
{
	struct key_mem_cache *tmp;
	BYTE *tmp_enc_data;

	pthread_mutex_lock(&mem_cache_lock);

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->blob->encSize == 0)
			continue;
		if (!memcmp(tmp->blob->encData, encData, tmp->blob->encSize)) {
			tmp_enc_data = (BYTE *)malloc(tmp->blob->encSize);
			if (tmp_enc_data == NULL) {
				LogError("malloc of %u bytes failed.", tmp->blob->encSize);
				pthread_mutex_unlock(&mem_cache_lock);
				return TCSERR(TSS_E_OUTOFMEMORY);
			}

			memcpy(tmp_enc_data, newEncData, tmp->blob->encSize);
			free(tmp->blob->encData);
			tmp->blob->encData = tmp_enc_data;
			pthread_mutex_unlock(&mem_cache_lock);
			return TSS_SUCCESS;
		}
	}
	pthread_mutex_unlock(&mem_cache_lock);
	LogError("Couldn't find requested encdata in mem cache");
	return TCSERR(TSS_E_INTERNAL_ERROR);
}

/*
 * only called from load key paths and the init (single thread time) path,
 * so no locking
 */
TSS_RESULT
mc_add_entry(TCS_KEY_HANDLE tcs_handle,
	     TCPA_KEY_HANDLE tpm_handle,
	     TCPA_KEY *key_blob)
{
	struct key_mem_cache *entry, *tmp;

	/* Make sure the cache doesn't already have an entry for this key */
	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tcs_handle == tmp->tcs_handle) {
			return TSS_SUCCESS;
		}
	}

	/* Not found - we need to create a new entry */
	entry = (struct key_mem_cache *)calloc(1, sizeof(struct key_mem_cache));
	if (entry == NULL) {
		LogError("malloc of %zd bytes failed.", sizeof(struct key_mem_cache));
		return TCSERR(TSS_E_OUTOFMEMORY);
	}

	entry->tcs_handle = tcs_handle;
	if (tpm_handle != NULL_TPM_HANDLE)
		entry->time_stamp = getNextTimeStamp();

	entry->tpm_handle = tpm_handle;

	/* allocate space for the blob */
	entry->blob = calloc(1, sizeof(TCPA_KEY));
	if (entry->blob == NULL) {
		LogError("malloc of %zd bytes failed.", sizeof(TCPA_KEY));
		free(entry);
		return TCSERR(TSS_E_OUTOFMEMORY);
	}
	memcpy(entry->blob, key_blob, sizeof(TCPA_KEY));

	/* allocate space for the key parameters if necessary */
	if (key_blob->algorithmParms.parmSize) {
		BYTE *tmp_parms = (BYTE *)malloc(key_blob->algorithmParms.parmSize);
		if (tmp_parms == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->algorithmParms.parmSize);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(tmp_parms, key_blob->algorithmParms.parms, key_blob->algorithmParms.parmSize);
		entry->blob->algorithmParms.parms = tmp_parms;
	}
	entry->blob->algorithmParms.parmSize = key_blob->algorithmParms.parmSize;

	/* allocate space for the public key */
	if (key_blob->pubKey.keyLength > 0) {
		entry->blob->pubKey.key = (BYTE *)malloc(key_blob->pubKey.keyLength);
		if (entry->blob->pubKey.key == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->pubKey.keyLength);
			free(entry->blob->algorithmParms.parms);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(entry->blob->pubKey.key, key_blob->pubKey.key, key_blob->pubKey.keyLength);
	}
	entry->blob->pubKey.keyLength = key_blob->pubKey.keyLength;

	/* allocate space for the PCR info */
	if (key_blob->PCRInfoSize > 0) {
		entry->blob->PCRInfo = (BYTE *)malloc(key_blob->PCRInfoSize);
		if (entry->blob->PCRInfo == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->PCRInfoSize);
			free(entry->blob->pubKey.key);
			free(entry->blob->algorithmParms.parms);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(entry->blob->PCRInfo, key_blob->PCRInfo, key_blob->PCRInfoSize);
	}
	entry->blob->PCRInfoSize = key_blob->PCRInfoSize;

	/* allocate space for the encData if necessary */
	if (key_blob->encSize > 0) {
		entry->blob->encData = (BYTE *)malloc(key_blob->encSize);
		if (entry->blob->encData == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->encSize);
			free(entry->blob->PCRInfo);
			free(entry->blob->pubKey.key);
			free(entry->blob->algorithmParms.parms);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(entry->blob->encData, key_blob->encData, key_blob->encSize);
	}
	entry->blob->encSize = key_blob->encSize;

	/* add to the front of the list */
	entry->next = key_mem_cache_head;
	if (key_mem_cache_head) {
		/* set the reference count to 0 initially for all keys not being the SRK. Up
		 * the call chain, a reference to this mem cache entry will be set in the
		 * context object of the calling context and this reference count will be
		 * incremented there. */
		entry->ref_cnt = 0;

		key_mem_cache_head->prev = entry;
	} else {
		/* if we are the SRK, initially set the reference count to 1, so that it is
		 * always seen as loaded in the TPM. */
		entry->ref_cnt = 1;
	}
	key_mem_cache_head = entry;

	return TSS_SUCCESS;
}

/* caller must lock the mem cache before calling! */
TSS_RESULT
mc_remove_entry(TCS_KEY_HANDLE tcs_handle)
{
	struct key_mem_cache *cur;

	for (cur = key_mem_cache_head; cur; cur = cur->next) {
		if (cur->tcs_handle == tcs_handle) {
			destroy_key_refs(cur->blob);
			free(cur->blob);

			if (cur->prev != NULL)
				cur->prev->next = cur->next;
			if (cur->next != NULL)
				cur->next->prev = cur->prev;

			if (cur == key_mem_cache_head)
				key_mem_cache_head = cur->next;
			free(cur);

			return TSS_SUCCESS;
		}
	}

	return TCSERR(TSS_E_FAIL);
}

/*
 * custom add mem cache entry function called only at take ownership time, since
 * that's the only non init-time instance where we need to create a mem cache
 * entry from outside a load key path
 */
TSS_RESULT
mc_add_entry_srk(TCS_KEY_HANDLE tcs_handle,
		 TCPA_KEY_HANDLE tpm_handle,
		 TCPA_KEY *key_blob)
{
	struct key_mem_cache *entry, *tmp;

	/* Make sure the cache doesn't already have an entry for this key */
	pthread_mutex_lock(&mem_cache_lock);
	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		if (tcs_handle == tmp->tcs_handle) {
			mc_remove_entry(tcs_handle);
		}
	}
	pthread_mutex_unlock(&mem_cache_lock);

	/* Not found - we need to create a new entry */
	entry = (struct key_mem_cache *)calloc(1, sizeof(struct key_mem_cache));
	if (entry == NULL) {
		LogError("malloc of %zd bytes failed.", sizeof(struct key_mem_cache));
		return TCSERR(TSS_E_OUTOFMEMORY);
	}

	entry->tcs_handle = tcs_handle;
	if (tpm_handle != NULL_TPM_HANDLE)
		entry->time_stamp = getNextTimeStamp();

	entry->tpm_handle = tpm_handle;

	/* allocate space for the blob */
	entry->blob = malloc(sizeof(TCPA_KEY));
	if (entry->blob == NULL) {
		LogError("malloc of %zd bytes failed.", sizeof(TCPA_KEY));
		free(entry);
		return TCSERR(TSS_E_OUTOFMEMORY);
	}
	memcpy(entry->blob, key_blob, sizeof(TCPA_KEY));

	/* allocate space for the key parameters if necessary */
	if (key_blob->algorithmParms.parmSize) {
		BYTE *tmp_parms = (BYTE *)malloc(key_blob->algorithmParms.parmSize);
		if (tmp_parms == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->algorithmParms.parmSize);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(tmp_parms, key_blob->algorithmParms.parms, key_blob->algorithmParms.parmSize);
		entry->blob->algorithmParms.parms = tmp_parms;
	}

	/* allocate space for the public key */
	entry->blob->pubKey.key = (BYTE *)malloc(key_blob->pubKey.keyLength);
	if (entry->blob->pubKey.key == NULL) {
		LogError("malloc of %u bytes failed.", key_blob->pubKey.keyLength);
		free(entry->blob);
		free(entry);
		return TCSERR(TSS_E_OUTOFMEMORY);
	}
	memcpy(entry->blob->pubKey.key, key_blob->pubKey.key, key_blob->pubKey.keyLength);

	/* allocate space for the encData if necessary */
	if (key_blob->encSize != 0) {
		entry->blob->encData = (BYTE *)malloc(key_blob->encSize);
		if (entry->blob->encData == NULL) {
			LogError("malloc of %u bytes failed.", key_blob->encSize);
			free(entry->blob->pubKey.key);
			free(entry->blob);
			free(entry);
			return TCSERR(TSS_E_OUTOFMEMORY);
		}
		memcpy(entry->blob->encData, key_blob->encData, key_blob->encSize);
	}
	entry->blob->encSize = key_blob->encSize;

	memcpy(&entry->uuid, &SRK_UUID, sizeof(TSS_UUID));

	pthread_mutex_lock(&mem_cache_lock);

	entry->next = key_mem_cache_head;
	if (key_mem_cache_head)
		key_mem_cache_head->prev = entry;

	entry->ref_cnt = 1;
	key_mem_cache_head = entry;
	pthread_mutex_unlock(&mem_cache_lock);

	return TSS_SUCCESS;
}

/* only called from evict key paths, so no locking */
TSS_RESULT
mc_set_slot_by_slot(TCPA_KEY_HANDLE old_handle, TCPA_KEY_HANDLE new_handle)
{
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		if (tmp->tpm_handle == old_handle) {
			LogDebugFn("Set TCS key 0x%x, old TPM handle: 0x%x "
				   "new TPM handle: 0x%x", tmp->tcs_handle,
				   old_handle, new_handle);
			if (new_handle == NULL_TPM_HANDLE)
				tmp->time_stamp = 0;
			else
				tmp->time_stamp = getNextTimeStamp();
			tmp->tpm_handle = new_handle;
			return TSS_SUCCESS;
		}
	}

	return TCSERR(TSS_E_FAIL);
}

/* only called from load key paths, so no locking */
TSS_RESULT
mc_set_slot_by_handle(TCS_KEY_HANDLE tcs_handle, TCPA_KEY_HANDLE tpm_handle)
{
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebug("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tcs_handle == tcs_handle) {
			if (tpm_handle == NULL_TPM_HANDLE)
				tmp->time_stamp = 0;
			else
				tmp->time_stamp = getNextTimeStamp();
			tmp->tpm_handle = tpm_handle;
			return TSS_SUCCESS;
		}
	}

	return TCSERR(TSS_E_FAIL);
}

/* the beginnings of a key manager start here ;-) */

TSS_RESULT
key_mgr_evict(TCS_CONTEXT_HANDLE hContext, TCS_KEY_HANDLE hKey)
{
	TSS_RESULT result;

	pthread_mutex_lock(&mem_cache_lock);

	result = TCSP_EvictKey_Internal(hContext, hKey);

	pthread_mutex_unlock(&mem_cache_lock);

	return result;
}

TSS_RESULT
key_mgr_load_by_blob(TCS_CONTEXT_HANDLE hContext, TCS_KEY_HANDLE hUnwrappingKey,
		     UINT32 cWrappedKeyBlob, BYTE *rgbWrappedKeyBlob,
		     TPM_AUTH *pAuth, TCS_KEY_HANDLE *phKeyTCSI, TCS_KEY_HANDLE *phKeyHMAC)
{
	TSS_RESULT result;

	pthread_mutex_lock(&mem_cache_lock);

	result = TCSP_LoadKeyByBlob_Internal(hContext, hUnwrappingKey, cWrappedKeyBlob,
					     rgbWrappedKeyBlob, pAuth, phKeyTCSI, phKeyHMAC);

	pthread_mutex_unlock(&mem_cache_lock);

	return result;
}

/* create a reference to one key. This is called from the key_mgr_load_*
 * functions only, so no locking is done.
 */
TSS_RESULT
key_mgr_inc_ref_count(TCS_KEY_HANDLE key_handle)
{
	struct key_mem_cache *cur;

	for (cur = key_mem_cache_head; cur; cur = cur->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", cur->tcs_handle);
		if (cur->tcs_handle == key_handle) {
			cur->ref_cnt++;
			return TSS_SUCCESS;
		}
	}

	return TCSERR(TSS_E_FAIL);
}

/* de-reference one key.  This is called by the context routines, so
 * locking is necessary.
 */
TSS_RESULT
key_mgr_dec_ref_count(TCS_KEY_HANDLE key_handle)
{
	struct key_mem_cache *cur;

	pthread_mutex_lock(&mem_cache_lock);

	for (cur = key_mem_cache_head; cur; cur = cur->next) {
		if (cur->tcs_handle == key_handle) {
			cur->ref_cnt--;
			LogDebugFn("decrementing ref cnt for key 0x%x",
				   key_handle);
			pthread_mutex_unlock(&mem_cache_lock);
			return TSS_SUCCESS;
		}
	}

	pthread_mutex_unlock(&mem_cache_lock);
	return TCSERR(TSS_E_FAIL);
}

/* run through the global list and free any keys with reference counts of 0 */
void
key_mgr_ref_count()
{
	struct key_mem_cache *tmp, *cur;

	pthread_mutex_lock(&mem_cache_lock);

	for (cur = key_mem_cache_head; cur;) {
		if (cur->ref_cnt == 0) {
			LogDebugFn("Key 0x%x being freed", cur->tcs_handle);
			destroy_key_refs(cur->blob);
			free(cur->blob);
			if (cur->prev != NULL)
				cur->prev->next = cur->next;
			if (cur->next != NULL)
				cur->next->prev = cur->prev;

			tmp = cur;
			if (cur == key_mem_cache_head)
				key_mem_cache_head = cur->next;
			cur = cur->next;
			free(tmp);
		} else {
			cur = cur->next;
		}
	}

	pthread_mutex_unlock(&mem_cache_lock);
}

/* only called from load key paths, so no locking */
TCPA_KEY_HANDLE
mc_get_slot_by_handle(TCS_KEY_HANDLE tcs_handle)
{
	struct key_mem_cache *tmp;
	TCS_KEY_HANDLE ret;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tcs_handle == tcs_handle) {
			ret = tmp->tpm_handle;
			return ret;
		}
	}

	LogDebugFn("returning NULL_TPM_HANDLE");
	return NULL_TPM_HANDLE;
}

/* called from functions outside the load key path */
TCPA_KEY_HANDLE
mc_get_slot_by_handle_lock(TCS_KEY_HANDLE tcs_handle)
{
	struct key_mem_cache *tmp;
	TCS_KEY_HANDLE ret;

	pthread_mutex_lock(&mem_cache_lock);

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tcs_handle == tcs_handle) {
			ret = tmp->tpm_handle;
			pthread_mutex_unlock(&mem_cache_lock);
			return ret;
		}
	}

	pthread_mutex_unlock(&mem_cache_lock);
	LogDebugFn("returning NULL_TPM_HANDLE");
	return NULL_TPM_HANDLE;
}

/* only called from load key paths, so no locking */
TCPA_KEY_HANDLE
mc_get_slot_by_pub(TCPA_STORE_PUBKEY *pub)
{
	struct key_mem_cache *tmp;
	TCPA_KEY_HANDLE ret;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (!memcmp(tmp->blob->pubKey.key, pub->key, pub->keyLength)) {
			ret = tmp->tpm_handle;
			return ret;
		}
	}

	LogDebugFn("returning NULL_TPM_HANDLE");
	return NULL_TPM_HANDLE;
}

/* Check the mem cache for a key with public key pub. If a parent TCS key handle
 * is passed in, make sure the parent of the key find matches it, else return
 * key not found */
/* only called from load key paths, so no locking */
TCS_KEY_HANDLE
mc_get_handle_by_pub(TCPA_STORE_PUBKEY *pub, TCS_KEY_HANDLE parent)
{
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (pub->keyLength == tmp->blob->pubKey.keyLength &&
		    !memcmp(tmp->blob->pubKey.key, pub->key, pub->keyLength)) {
			if (parent) {
				if (!tmp->parent)
					continue;
				if (parent == tmp->parent->tcs_handle)
					return tmp->tcs_handle;
			} else
				return tmp->tcs_handle;
		}
	}

	LogDebugFn("returning NULL_TPM_HANDLE");
	return NULL_TPM_HANDLE;
}

/* only called from load key paths, so no locking */
TCPA_STORE_PUBKEY *
mc_get_parent_pub_by_pub(TCPA_STORE_PUBKEY *pub)
{
	struct key_mem_cache *tmp;
	TCPA_STORE_PUBKEY *ret = NULL;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tcs_handle == TPM_KEYHND_SRK) {
			LogDebugFn("skipping the SRK");
			continue;
		}
		if (!memcmp(tmp->blob->pubKey.key, pub->key, pub->keyLength)) {
			if (tmp->parent && tmp->parent->blob) {
				ret = &tmp->parent->blob->pubKey;
				LogDebugFn("Success");
			} else {
				LogError("parent pointer not set in key mem cache object w/ TCS "
					 "handle: 0x%x", tmp->tcs_handle);
			}
			return ret;
		}
	}

	LogDebugFn("returning NULL TCPA_STORE_PUBKEY");
	return NULL;
}

/* only called from load key paths, so no locking */
TSS_RESULT
mc_get_blob_by_pub(TCPA_STORE_PUBKEY *pub, TCPA_KEY **ret_key)
{
	struct key_mem_cache *tmp;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (!memcmp(tmp->blob->pubKey.key, pub->key, pub->keyLength)) {
			*ret_key = tmp->blob;
			return TSS_SUCCESS;
		}
	}

	LogDebugFn("returning TSS_E_FAIL");
	return TCSERR(TSS_E_FAIL);
}

/* only called from load key paths, so no locking */
TCS_KEY_HANDLE
mc_get_handle_by_slot(TCPA_KEY_HANDLE tpm_handle)
{
	struct key_mem_cache *tmp;
	TCS_KEY_HANDLE ret;

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tpm_handle == tpm_handle) {
			ret = tmp->tcs_handle;
			return ret;
		}
	}

	return NULL_TCS_HANDLE;
}

/* only called from load key paths, so no locking */
TSS_RESULT
mc_update_time_stamp(TCPA_KEY_HANDLE tpm_handle)
{
	struct key_mem_cache *tmp;
	TSS_RESULT ret = TCSERR(TSS_E_FAIL);

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		LogDebugFn("TCSD mem_cached handle: 0x%x", tmp->tcs_handle);
		if (tmp->tpm_handle == tpm_handle) {
			tmp->time_stamp = getNextTimeStamp();
			ret = TSS_SUCCESS;
			break;
		}
	}

	return ret;
}

/* Right now this evicts the LRU key assuming it's not the parent */
TSS_RESULT
evictFirstKey(TCS_KEY_HANDLE parent_tcs_handle)
{
	struct key_mem_cache *tmp;
	TCS_KEY_HANDLE tpm_handle_to_evict = NULL_TPM_HANDLE;
	UINT32 smallestTimeStamp = ~(0U);	/* largest */
	TSS_RESULT result;
	UINT32 count;

	/* First, see if there are any known keys worth evicting */
	if ((result = clearUnknownKeys(InternalContext, &count)))
		return result;

	if (count > 0) {
		LogDebugFn("Evicted %u unknown keys", count);
		return TSS_SUCCESS;
	}

	for (tmp = key_mem_cache_head; tmp; tmp = tmp->next) {
		if (tmp->tpm_handle != NULL_TPM_HANDLE &&	/* not already evicted */
		    tmp->tpm_handle != SRK_TPM_HANDLE &&	/* not the srk */
		    tmp->tcs_handle != parent_tcs_handle &&	/* not my parent */
		    tmp->time_stamp < smallestTimeStamp) {	/* is the smallest time
								   stamp so far */
			tpm_handle_to_evict = tmp->tpm_handle;
			smallestTimeStamp = tmp->time_stamp;
		}
	}

	if (tpm_handle_to_evict != NULL_TCS_HANDLE) {
		if ((result = internal_EvictByKeySlot(tpm_handle_to_evict)))
			return result;

		LogDebugFn("Evicted key w/ TPM handle 0x%x", tpm_handle_to_evict);
		result = mc_set_slot_by_slot(tpm_handle_to_evict, NULL_TPM_HANDLE);
	} else
		return TSS_SUCCESS;

	return result;
}

TSS_BOOL
isKeyLoaded(TCPA_KEY_HANDLE keySlot)
{
	UINT64 offset;
	UINT32 i;
	TCPA_KEY_HANDLE_LIST keyList;
	UINT32 respSize;
	BYTE *resp;
	TSS_RESULT result;

	if (keySlot == SRK_TPM_HANDLE) {
		return TRUE;
	}

	if ((result = TCSP_GetCapability_Internal(InternalContext, TCPA_CAP_KEY_HANDLE, 0, NULL,
						  &respSize, &resp)))
		goto not_loaded;

	offset = 0;
	UnloadBlob_KEY_HANDLE_LIST(&offset, resp, &keyList);
	free(resp);
	for (i = 0; i < keyList.loaded; i++) {
		LogDebugFn("loaded TPM key handle: 0x%x", keyList.handle[i]);
		if (keyList.handle[i] == keySlot) {
			free(keyList.handle);
			return TRUE;
		}
	}

	free(keyList.handle);

not_loaded:
	LogDebugFn("Key is not loaded, changing slot");
	mc_set_slot_by_slot(keySlot, NULL_TPM_HANDLE);
	return FALSE;
}

/* all calls to LoadKeyShim are inside locks */
TSS_RESULT
LoadKeyShim(TCS_CONTEXT_HANDLE hContext, TCPA_STORE_PUBKEY *pubKey,
	    TSS_UUID *parentUuid, TCPA_KEY_HANDLE *slotOut)
{

	TCPA_STORE_PUBKEY *parentPub;
	UINT32 result;
	TCPA_KEY_HANDLE keySlot;
	TCPA_KEY_HANDLE parentSlot;
	TCS_KEY_HANDLE tcsKeyHandle;
	TCPA_KEY *myKey;
	UINT64 offset;
	TCS_KEY_HANDLE parentHandle;
	BYTE keyBlob[1024];

	LogDebugFn("calling mc_get_slot_by_pub");

	/* If I'm loaded, then no point being here.  Get the slot and return */
	keySlot = mc_get_slot_by_pub(pubKey);
	if (keySlot != NULL_TPM_HANDLE && isKeyLoaded(keySlot)) {
		*slotOut = keySlot;
		return TSS_SUCCESS;
	}

	/*
	 * Before proceeding, the parent must be loaded.
	 * If the parent is registered, then it can be loaded by UUID.
	 * If not, then the shim will be called to load it's parent and then try
	 * to load it based on the persistent store.
	 */

	LogDebugFn("calling mc_get_parent_pub_by_pub");
	/* Check if the Key is in the memory cache */
	if ((parentPub = mc_get_parent_pub_by_pub(pubKey)) == NULL) {
#if 0
		LogDebugFn("parentPub is NULL");
		/* If parentUUID is not handed in, then this key was never loaded and isn't reg'd */
		if (parentUuid == NULL)
			return TCSERR(TCS_E_KM_LOADFAILED);

		LogDebugFn("calling TCSP_LoadKeyByUUID_Internal");
		/* This will try to load my parent by UUID */
		if ((result = TCSP_LoadKeyByUUID_Internal(hContext, parentUuid, NULL, &parentSlot)))
			return result;
#else
		return TCSERR(TCS_E_KM_LOADFAILED);
#endif
	} else {
		LogDebugFn("calling LoadKeyShim");
		if ((result = LoadKeyShim(hContext, parentPub, NULL, &parentSlot)))
			return result;
	}

	/*
	 * Now that the parent is loaded, I can load myself.
	 * If I'm registered, that's by UUID.  If I'm not,
	 * that's by blob.  If there is no persistent storage data, then I cannot be
	 * loaded by blob. The user must have some point loaded this key manually.
	 */

	/* check the mem cache */
	if (mc_get_blob_by_pub(pubKey, &myKey) == TSS_SUCCESS) {
		parentHandle = mc_get_handle_by_slot(parentSlot);
		if (parentHandle == 0)
			return TCSERR(TCS_E_KM_LOADFAILED);

		offset = 0;
		LoadBlob_KEY(&offset, keyBlob, myKey);
		if ((result = TCSP_LoadKeyByBlob_Internal(hContext, parentHandle, offset, keyBlob,
							  NULL, &tcsKeyHandle, slotOut)))
			return result;

		return ctx_mark_key_loaded(hContext, tcsKeyHandle);
#if TSS_BUILD_PS
	} else {
		TSS_UUID *uuid;

		/* check registered */
		if (ps_is_pub_registered(pubKey) == FALSE)
			return TCSERR(TCS_E_KM_LOADFAILED);
		//uuid = mc_get_uuid_by_pub(pubKey); // XXX pub is not in MC
		if ((result = ps_get_uuid_by_pub(pubKey, &uuid)))
			return result;

		if ((result = TCSP_LoadKeyByUUID_Internal(hContext, uuid, NULL, &tcsKeyHandle))) {
			free(uuid);
			return result;
		}
		free(uuid);
		*slotOut = mc_get_slot_by_handle(tcsKeyHandle);

		return ctx_mark_key_loaded(hContext, tcsKeyHandle);
#endif
	}

	return TCSERR(TCS_E_KM_LOADFAILED);
}
