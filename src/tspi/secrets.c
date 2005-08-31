
/*
 * Licensed Materials - Property of IBM
 *
 * trousers - An open source TCG Software Stack
 *
 * (C) Copyright International Business Machines Corp. 2004
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "trousers/tss.h"
#include "trousers/trousers.h"
#include "spi_internal_types.h"
#include "spi_utils.h"
#include "capabilities.h"
#include "tsplog.h"
#include "obj.h"

/*
 *  popup_GetSecret()
 *
 *    newPIN - non-zero to popup the dialog to enter a new PIN, zero to popup a dialog
 *      to enter an existing PIN
 *    popup_str - string to appear in the title bar of the popup dialog
 *    auth_hash - the 20+ byte buffer that receives the SHA1 hash of the auth data
 *      entered into the dialog box
 *
 */
TSS_RESULT
popup_GetSecret(UINT32 new_pin, BYTE *popup_str, void *auth_hash)
{
	char secret[UI_MAX_SECRET_STRING_LENGTH] = "\0";
	BYTE *dflt = "TSS Authentication Dialog";

	if (popup_str == NULL)
		popup_str = dflt;

	/* pin the area where the secret will be put in memory */
	if (pin_mem(&secret, UI_MAX_SECRET_STRING_LENGTH)) {
		LogError1("Failed to pin secret in memory.");
		return TSPERR(TSS_E_INTERNAL_ERROR);
	}

	if (new_pin) {
		if (DisplayNewPINWindow(secret, popup_str))
			return TSPERR(TSS_E_INTERNAL_ERROR);
	} else if (DisplayPINWindow(secret, popup_str))
		return TSPERR(TSS_E_INTERNAL_ERROR);

	/* allow a 0 length password here, as spec'd by the TSSWG */
	Trspi_Hash(TSS_HASH_SHA1, strlen(secret), secret, (char *)auth_hash);

	/* zero, then unpin the memory */
	memset(&secret, 0, UI_MAX_SECRET_STRING_LENGTH);
	unpin_mem(&secret, UI_MAX_SECRET_STRING_LENGTH);

	return TSS_SUCCESS;
}

TSS_RESULT
secret_PerformAuth_OIAP(TSS_HPOLICY hPolicy, TCPA_DIGEST *hashDigest, TPM_AUTH *auth)
{
	TSS_RESULT result;
	TCS_CONTEXT_HANDLE tcsContext;
	TSS_BOOL bExpired;
	UINT32 mode;
	TCPA_SECRET secret;

	/* This validates that the secret can be used */
	if ((result = obj_policy_has_expired(hPolicy, &bExpired)))
		return result;

	if (bExpired == TRUE)
		return TSPERR(TSS_E_INVALID_OBJ_ACCESS);

	if ((result = obj_policy_get_tcs_context(hPolicy, &tcsContext)))
		return result;

	if ((result = Init_AuthNonce(tcsContext, auth)))
		return result;

	/* added retry logic */
	if ((result = TCSP_OIAP(tcsContext, &auth->AuthHandle, &auth->NonceEven))) {
		if (result == TCPA_E_RESOURCES) {
			int retry = 0;
			do {
				/* POSIX sleep time, { secs, nanosecs } */
				struct timespec t = { 0, AUTH_RETRY_NANOSECS };

				nanosleep(&t, NULL);

				result = TCSP_OIAP(tcsContext, &auth->AuthHandle, &auth->NonceEven);
			} while (result == TCPA_E_RESOURCES && ++retry < AUTH_RETRY_COUNT);
		}

		if (result)
			return result;
	}

	if ((result = obj_policy_get_mode(hPolicy, &mode)))
		return result;

	switch (mode) {
		case TSS_SECRET_MODE_CALLBACK:
			result = obj_policy_do_hmac(hPolicy, TRUE,
					auth->fContinueAuthSession,
					FALSE,
					20,
					auth->NonceEven.nonce,
					auth->NonceOdd.nonce,
					NULL, NULL, 20,
					hashDigest->digest,
					(BYTE *)&auth->HMAC);
			break;
		case TSS_SECRET_MODE_SHA1:
		case TSS_SECRET_MODE_PLAIN:
		case TSS_SECRET_MODE_POPUP:
			if ((result = obj_policy_get_secret(hPolicy, &secret)))
				return result;

			HMAC_Auth(secret.authdata, hashDigest->digest, auth);
			break;
		default:
			result = TSPERR(TSS_E_POLICY_NO_SECRET);
			break;
	}

	if (result)
		return result;

	return obj_policy_dec_counter(hPolicy);
}

TSS_RESULT
secret_PerformXOR_OSAP(TSS_HPOLICY hPolicy, TSS_HPOLICY hUsagePolicy,
		       TSS_HPOLICY hMigrationPolicy, TSS_HOBJECT hKey,
		       UINT16 osapType, UINT32 osapData,
		       TCPA_ENCAUTH * encAuthUsage, TCPA_ENCAUTH * encAuthMig,
		       BYTE *sharedSecret, TPM_AUTH * auth, TCPA_NONCE * nonceEvenOSAP)
{
	TSS_BOOL bExpired;
	TCPA_SECRET keySecret;
	TCPA_SECRET usageSecret;
	TCPA_SECRET migSecret;
	UINT32 keyMode, usageMode, migMode;

	TSS_RESULT result;
	TCS_CONTEXT_HANDLE tcsContext;

	if ((result = obj_policy_has_expired(hPolicy, &bExpired)))
		return result;

	if (bExpired == TRUE)
		return TSPERR(TSS_E_INVALID_OBJ_ACCESS);

	if ((result = obj_policy_has_expired(hUsagePolicy, &bExpired)))
		return result;

	if (bExpired == TRUE)
		return TSPERR(TSS_E_INVALID_OBJ_ACCESS);

	if ((result = obj_policy_has_expired(hMigrationPolicy, &bExpired)))
		return result;

	if (bExpired == TRUE)
		return TSPERR(TSS_E_INVALID_OBJ_ACCESS);

	if ((result = obj_policy_get_tcs_context(hPolicy, &tcsContext)))
		return result;

	if ((result = obj_policy_get_mode(hPolicy, &keyMode)))
		return result;

	if ((result = obj_policy_get_mode(hUsagePolicy, &usageMode)))
		return result;

	if ((result = obj_policy_get_mode(hMigrationPolicy, &migMode)))
		return result;

	if (keyMode == TSS_SECRET_MODE_CALLBACK ||
	    usageMode == TSS_SECRET_MODE_CALLBACK ||
	    migMode == TSS_SECRET_MODE_CALLBACK) {
		if (keyMode != TSS_SECRET_MODE_CALLBACK ||
		    usageMode != TSS_SECRET_MODE_CALLBACK ||
		    migMode != TSS_SECRET_MODE_CALLBACK)
			return TSPERR(TSS_E_BAD_PARAMETER);
	}

	if (keyMode != TSS_SECRET_MODE_CALLBACK) {
		if ((result = obj_policy_get_secret(hPolicy, &keySecret)))
			return result;

		if ((result = obj_policy_get_secret(hUsagePolicy, &usageSecret)))
			return result;

		if ((result = obj_policy_get_secret(hMigrationPolicy, &migSecret)))
			return result;

		if ((result = OSAP_Calc(tcsContext, osapType, osapData, keySecret.authdata,
				       usageSecret.authdata,
				       migSecret.authdata,
				       encAuthUsage, encAuthMig, sharedSecret, auth)))
			return result;
	} else {
		if ((result = TCSP_OSAP(tcsContext, osapType, osapData, auth->NonceOdd,
				  &auth->AuthHandle, &auth->NonceEven, nonceEvenOSAP)))
			return result;

		if ((result = obj_policy_do_xor(hPolicy, NULL_HOBJECT, hKey,
						FALSE, 20,
						auth->NonceEven.nonce, NULL,
						nonceEvenOSAP->nonce,
						auth->NonceOdd.nonce, 20,
						encAuthUsage->authdata,
						encAuthMig->authdata)))
			return result;
	}

	return TSS_SUCCESS;
}

TSS_RESULT
secret_PerformAuth_OSAP(TSS_HPOLICY hPolicy, TSS_HPOLICY hUsagePolicy,
			TSS_HPOLICY hMigPolicy, TSS_HOBJECT hKey,
			BYTE sharedSecret[20], TPM_AUTH * auth,
			BYTE * hashDigest, TCPA_NONCE *nonceEvenOSAP)
{
	TSS_RESULT result;
	UINT32 keyMode, usageMode, migMode;

	if ((result = obj_policy_get_mode(hPolicy, &keyMode)))
		return result;

	if ((result = obj_policy_get_mode(hUsagePolicy, &usageMode)))
		return result;

	if ((result = obj_policy_get_mode(hMigPolicy, &migMode)))
		return result;

	/* ---  If any of them is a callback */
	if (keyMode == TSS_SECRET_MODE_CALLBACK ||
	    usageMode == TSS_SECRET_MODE_CALLBACK ||
	    migMode == TSS_SECRET_MODE_CALLBACK) {
		/* ---  And they're not all callback */
		if (keyMode != TSS_SECRET_MODE_CALLBACK ||
		    usageMode != TSS_SECRET_MODE_CALLBACK ||
		    migMode != TSS_SECRET_MODE_CALLBACK)
			return TSPERR(TSS_E_BAD_PARAMETER);
	}

	if (keyMode == TSS_SECRET_MODE_CALLBACK) {
		if ((result = obj_policy_do_hmac(hPolicy,
						TRUE,
						auth->fContinueAuthSession,
						TRUE,
						20,
						auth->NonceEven.nonce,
						NULL,
						nonceEvenOSAP->nonce,
						auth->NonceOdd.nonce, 20,
						hashDigest,
						(BYTE *)&auth->HMAC)))
			return result;
	} else {
		HMAC_Auth(sharedSecret, hashDigest, auth);
	}

	if ((result = obj_policy_dec_counter(hPolicy)))
		return result;

	if ((result = obj_policy_dec_counter(hUsagePolicy)))
		return result;

	if ((result = obj_policy_dec_counter(hMigPolicy)))
		return result;

	return TSS_SUCCESS;
}

TSS_RESULT
secret_ValidateAuth_OSAP(TSS_HPOLICY hPolicy, TSS_HPOLICY hUsagePolicy,
			 TSS_HPOLICY hMigPolicy, BYTE sharedSecret[20],
			 TPM_AUTH * auth, BYTE * hashDigest, TCPA_NONCE *nonceEvenOSAP)
{
	TSS_RESULT result;
	UINT32 keyMode, usageMode, migMode;

	if ((result = obj_policy_get_mode(hPolicy, &keyMode)))
		return result;

	if ((result = obj_policy_get_mode(hUsagePolicy, &usageMode)))
		return result;

	if ((result = obj_policy_get_mode(hMigPolicy, &migMode)))
		return result;

	/* ---  If any of them is a callback */
	if (keyMode == TSS_SECRET_MODE_CALLBACK ||
	    usageMode == TSS_SECRET_MODE_CALLBACK ||
	    migMode == TSS_SECRET_MODE_CALLBACK) {
		/* ---  And they're not all callback */
		if (keyMode != TSS_SECRET_MODE_CALLBACK ||
		    usageMode != TSS_SECRET_MODE_CALLBACK ||
		    migMode != TSS_SECRET_MODE_CALLBACK)
			return TSPERR(TSS_E_BAD_PARAMETER);
	}

	if (keyMode != TSS_SECRET_MODE_CALLBACK) {
		if (validateReturnAuth(sharedSecret, hashDigest, auth))
			return TSPERR(TSS_E_TSP_AUTHFAIL);
	} else {
		if ((result = obj_policy_do_hmac(hPolicy,
						0,
						auth->fContinueAuthSession,
						TRUE,
						20,
						auth->NonceEven.nonce,
						NULL,
						nonceEvenOSAP->nonce,
						auth->NonceOdd.nonce, 20,
						hashDigest,
						(BYTE *)&auth->HMAC)))
			return result;
	}

	return TSS_SUCCESS;
}

TSS_RESULT
secret_TakeOwnership(TSS_HKEY hEndorsementPubKey,
		     TSS_HTPM hTPM,
		     TSS_HKEY hKeySRK,
		     TPM_AUTH * auth,
		     UINT32 * encOwnerAuthLength,
		     BYTE * encOwnerAuth, UINT32 * encSRKAuthLength, BYTE * encSRKAuth)
{
	TSS_RESULT result;
	UINT32 endorsementKeySize;
	BYTE *endorsementKey;
	TCPA_KEY dummyKey;
	UINT16 offset;
	TCPA_SECRET ownerSecret;
	TCPA_SECRET srkSecret;
	BYTE hashblob[1024];
	TCPA_DIGEST digest;
	TSS_HPOLICY hSrkPolicy;
	TSS_HPOLICY hOwnerPolicy;
	UINT32 srkKeyBlobLength;
	BYTE *srkKeyBlob;
	TCS_CONTEXT_HANDLE tcsContext;
	TSS_HCONTEXT tspContext;
	UINT32 ownerMode, srkMode;

	if ((result = obj_tpm_get_tcs_context(hTPM, &tcsContext)))
		return result;

	if ((result = obj_tpm_get_tsp_context(hTPM, &tspContext)))
		return result;

	/*************************************************
	 *	First, get the policy objects and check them for how
	 *		to handle the secrets.  If they cannot be found
	 *		or there is an error, then we must fail
	 **************************************************/

	/* First get the Owner Policy */
	if ((result = Tspi_GetPolicyObject(hTPM, TSS_POLICY_USAGE, &hOwnerPolicy)))
		return result;

	/* Now get the SRK Policy */

	if ((result = Tspi_GetPolicyObject(hKeySRK, TSS_POLICY_USAGE, &hSrkPolicy)))
		return result;

	if ((result = obj_policy_get_mode(hOwnerPolicy, &ownerMode)))
		return result;

	if ((result = obj_policy_get_mode(hSrkPolicy, &srkMode)))
		return result;

	/* If the policy callback's aren't the same, that's an error if one is callback */
	if (srkMode == TSS_SECRET_MODE_CALLBACK ||
	    ownerMode == TSS_SECRET_MODE_CALLBACK) {
		if (srkMode != TSS_SECRET_MODE_CALLBACK ||
		    ownerMode != TSS_SECRET_MODE_CALLBACK) {
			LogError1("Policy callback modes for SRK policy and "
					"Owner policy differ");
			return TSPERR(TSS_E_BAD_PARAMETER);
		}
	}

	if (ownerMode != TSS_SECRET_MODE_CALLBACK) {
		/* First, get the Endorsement Public Key for Encrypting */
		if ((result = Tspi_GetAttribData(hEndorsementPubKey,
					    TSS_TSPATTRIB_KEY_BLOB,
					    TSS_TSPATTRIB_KEYBLOB_BLOB,
					    &endorsementKeySize, &endorsementKey)))
			return result;

		/* now stick it in a Key Structure */
		offset = 0;
		Trspi_UnloadBlob_KEY(tspContext, &offset, endorsementKey, &dummyKey);

		if ((result = obj_policy_get_secret(hOwnerPolicy, &ownerSecret)))
			return result;

		if ((result = obj_policy_get_secret(hSrkPolicy, &srkSecret)))
			return result;

		/* Encrypt the Owner Authorization */
		Trspi_RSA_Encrypt(ownerSecret.authdata,
				       20,
				       encOwnerAuth,
				       encOwnerAuthLength,
				       dummyKey.pubKey.key,
				       dummyKey.pubKey.keyLength);

		/* Encrypt the SRK Authorization */
		Trspi_RSA_Encrypt(srkSecret.authdata,
				       20,
				       encSRKAuth,
				       encSRKAuthLength,
				       dummyKey.pubKey.key,
				       dummyKey.pubKey.keyLength);
	} else {
		*encOwnerAuthLength = 256;
		*encSRKAuthLength = 256;
		if ((result = obj_policy_do_takeowner(hOwnerPolicy, hTPM,
						hEndorsementPubKey, *encOwnerAuthLength,
						encOwnerAuth)))
			return result;
	}

	if ((result = Tspi_GetAttribData(hKeySRK,
				    TSS_TSPATTRIB_KEY_BLOB,
				    TSS_TSPATTRIB_KEYBLOB_BLOB,
				    &srkKeyBlobLength,
				    &srkKeyBlob)))
		return result;

	/* Authorizatin Digest Calculation */
	/* Hash first the following: */
	offset = 0;
	Trspi_LoadBlob_UINT32(&offset, TPM_ORD_TakeOwnership, hashblob);
	Trspi_LoadBlob_UINT16(&offset, TCPA_PID_OWNER, hashblob);
	Trspi_LoadBlob_UINT32(&offset, *encOwnerAuthLength, hashblob);
	Trspi_LoadBlob(&offset, *encOwnerAuthLength, hashblob, encOwnerAuth);
	Trspi_LoadBlob_UINT32(&offset, *encSRKAuthLength, hashblob);
	Trspi_LoadBlob(&offset, *encSRKAuthLength, hashblob, encSRKAuth);
	Trspi_LoadBlob(&offset, srkKeyBlobLength, hashblob, srkKeyBlob);

	Trspi_Hash(TSS_HASH_SHA1, offset, hashblob, digest.digest);

	/* HMAC for the final digest */

	if ((result = secret_PerformAuth_OIAP(hOwnerPolicy, &digest, auth)))
		return result;

	return TSS_SUCCESS;
}
