#pragma once
#include <security.h>
#define UNISP_NAME_W L"Microsoft Unified Security Protocol Provider"
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#define SCH_USE_STRONG_CRYPTO 0x00400000
#define SCH_CRED_NO_DEFAULT_CREDS 0x00000010
#define SCH_CRED_MANUAL_CRED_VALIDATION 0x00000008
#define SCH_CREDENTIALS_VERSION 0x00000005
#define SCHANNEL_CRED_VERSION 0x00000004
typedef struct _TLS_PARAMETERS { DWORD cAlpnIds; void* rgstrAlpnIds; DWORD grbitDisabledProtocols; DWORD cDisabledCrypto; void* pDisabledCrypto; DWORD dwFlags; } TLS_PARAMETERS;
typedef struct _SCH_CREDENTIALS { DWORD dwVersion, dwCredFormat, cCreds; void* paCred; void* hRootStore; DWORD cMappers; void* aphMappers; DWORD dwSessionLifespan, dwFlags, cTlsParameters; TLS_PARAMETERS* pTlsParameters; } SCH_CREDENTIALS;
typedef struct _SCHANNEL_CRED { DWORD dwVersion, cCreds; void* paCred; void* hRootStore; DWORD cMappers; void* aphMappers; DWORD cSupportedAlgs; void* palgSupportedAlgs; DWORD grbitEnabledProtocols, dwMinimumCipherStrength, dwMaximumCipherStrength, dwSessionLifespan, dwFlags, dwCredFormat; } SCHANNEL_CRED;
typedef const struct _CERT_CONTEXT* PCCERT_CONTEXT;
#define X509_ASN_ENCODING 0x00000001
#define X509_PUBLIC_KEY_INFO ((LPCSTR)8)
