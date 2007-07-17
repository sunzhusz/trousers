
/*
 * Licensed Materials - Property of IBM
 *
 * trousers - An open source TCG Software Stack
 *
 * (C) Copyright International Business Machines Corp. 2004, 2005, 2007
 *
 */

#ifndef _TROUSERS_TYPES_H_
#define _TROUSERS_TYPES_H_

#define TCPA_NONCE_SIZE		sizeof(TCPA_NONCE)
#define TCPA_DIGEST_SIZE	sizeof(TCPA_DIGEST)
#define TCPA_ENCAUTH_SIZE	sizeof(TCPA_ENCAUTH)
#define TCPA_DIRVALUE_SIZE	sizeof(TCPA_DIRVALUE)
#define TCPA_AUTHDATA_SIZE	sizeof(TCPA_AUTHDATA)
#define TPM_NONCE_SIZE		TCPA_NONCE_SIZE
#define TPM_DIGEST_SIZE		TCPA_DIGEST_SIZE
#define TPM_ENCAUTH_SIZE	TCPA_ENCAUTH_SIZE
#define TPM_DIRVALUE_SIZE	TCPA_DIRVALUE_SIZE
#define TPM_AUTHDATA_SIZE	TCPA_AUTHDATA_SIZE

#define TSS_FLAG_MIGRATABLE	(migratable)
#define TSS_FLAG_VOLATILE	(volatileKey)
#define TSS_FLAG_REDIRECTION	(redirection)

/* return codes */
#define TCPA_E_INAPPROPRIATE_ENC	TCPA_E_NEED_SELFTEST

#define TSS_ERROR_LAYER(x)	(x & 0x3000)
#define TSS_ERROR_CODE(x)	(x & TSS_MAX_ERROR)

#define TSPERR(x)		(x | TSS_LAYER_TSP)
#define TCSERR(x)		(x | TSS_LAYER_TCS)
#define TDDLERR(x)		(x | TSS_LAYER_TDDL)

extern TSS_UUID	NULL_UUID;
extern TSS_UUID	SRK_UUID;

#define NULL_HOBJECT	0
#define NULL_HCONTEXT	NULL_HOBJECT
#define NULL_HPCRS	NULL_HOBJECT
#define NULL_HENCDATA	NULL_HOBJECT
#define NULL_HKEY	NULL_HOBJECT
#define NULL_HTPM	NULL_HOBJECT
#define NULL_HHASH	NULL_HOBJECT
#define NULL_HPOLICY	NULL_HOBJECT

#define TSS_OBJECT_TYPE_CONTEXT		(0x0e)
#define TSS_OBJECT_TYPE_TPM		(0x0f)

#define TSS_PS_TYPE_NO			(0)

/* Derived Types */
#define TSS_MIGRATION_SCHEME	TSS_MIGRATE_SCHEME

// The TPM's non-volatile flags (TPM_PERMANENT_FLAGS)
#define TSS_TPM_PF_DISABLE_BIT			    (1 << (TPM_PF_DISABLE - 1))
#define TSS_TPM_PF_OWNERSHIP_BIT		    (1 << (TPM_PF_OWNERSHIP - 1))
#define TSS_TPM_PF_DEACTIVATED_BIT		    (1 << (TPM_PF_DEACTIVATED - 1))
#define TSS_TPM_PF_READPUBEK_BIT		    (1 << (TPM_PF_READPUBEK - 1))
#define TSS_TPM_PF_DISABLEOWNERCLEAR_BIT	    (1 << (TPM_PF_DISABLEOWNERCLEAR - 1))
#define TSS_TPM_PF_ALLOWMAINTENANCE_BIT		    (1 << (TPM_PF_ALLOWMAINTENANCE - 1))
#define TSS_TPM_PF_PHYSICALPRESENCELIFETIMELOCK_BIT (1 << (TPM_PF_PHYSICALPRESENCELIFETIMELOCK - 1))
#define TSS_TPM_PF_PHYSICALPRESENCEHWENABLE_BIT	    (1 << (TPM_PF_PHYSICALPRESENCEHWENABLE - 1))
#define TSS_TPM_PF_PHYSICALPRESENCECMDENABLE_BIT    (1 << (TPM_PF_PHYSICALPRESENCECMDENABLE - 1))
#define TSS_TPM_PF_CEKPUSED_BIT			    (1 << (TPM_PF_CEKPUSED - 1))
#define TSS_TPM_PF_TPMPOST_BIT			    (1 << (TPM_PF_TPMPOST - 1))
#define TSS_TPM_PF_TPMPOSTLOCK_BIT		    (1 << (TPM_PF_TPMPOSTLOCK - 1))
#define TSS_TPM_PF_FIPS_BIT			    (1 << (TPM_PF_FIPS - 1))
#define TSS_TPM_PF_OPERATOR_BIT			    (1 << (TPM_PF_OPERATOR - 1))
#define TSS_TPM_PF_ENABLEREVOKEEK_BIT		    (1 << (TPM_PF_ENABLEREVOKEEK - 1))
#define TSS_TPM_PF_NV_LOCKED_BIT		    (1 << (TPM_PF_NV_LOCKED - 1))
#define TSS_TPM_PF_READSRKPUB_BIT		    (1 << (TPM_PF_READSRKPUB - 1))
#define TSS_TPM_PF_RESETESTABLISHMENTBIT_BIT	    (1 << (TPM_PF_RESETESTABLISHMENTBIT - 1))
#define TSS_TPM_PF_MAINTENANCEDONE_BIT		    (1 << (TPM_PF_MAINTENANCEDONE - 1))

// The TPM's volatile flags (TPM_STCLEAR_FLAGS)
#define TSS_TPM_SF_DEACTIVATED_BIT	    (1 << (TPM_SF_DEACTIVATED - 1))
#define TSS_TPM_SF_DISABLEFORCECLEAR_BIT    (1 << (TPM_SF_DISABLEFORCECLEAR - 1))
#define TSS_TPM_SF_PHYSICALPRESENCE_BIT     (1 << (TPM_SF_PHYSICALPRESENCE - 1))
#define TSS_TPM_SF_PHYSICALPRESENCELOCK_BIT (1 << (TPM_SF_PHYSICALPRESENCELOCK - 1))
#define TSS_TPM_SF_GLOBALLOCK_BIT	    (1 << (TPM_SF_GLOBALLOCK - 1))

#endif
