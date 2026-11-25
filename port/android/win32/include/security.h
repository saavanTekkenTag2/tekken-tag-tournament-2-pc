// SSPI types only (the Android build replaces the SChannel TLS client, community_tls_android.cpp).
#pragma once
#include <windows.h>
typedef LONG SECURITY_STATUS;
typedef struct _SecHandle { ULONG_PTR dwLower, dwUpper; } SecHandle, *PSecHandle, CredHandle, *PCredHandle, CtxtHandle, *PCtxtHandle;
typedef LARGE_INTEGER TimeStamp, *PTimeStamp;
typedef struct _SecBuffer { ULONG cbBuffer, BufferType; void* pvBuffer; } SecBuffer, *PSecBuffer;
typedef struct _SecBufferDesc { ULONG ulVersion, cBuffers; PSecBuffer pBuffers; } SecBufferDesc, *PSecBufferDesc;
typedef struct _SecPkgContext_StreamSizes { ULONG cbHeader, cbTrailer, cbMaximumMessage, cBuffers, cbBlockSize; } SecPkgContext_StreamSizes;
typedef struct _SecPkgContext_ConnectionInfo { DWORD dwProtocol; DWORD aiCipher, dwCipherStrength, aiHash, dwHashStrength, aiExch, dwExchStrength; } SecPkgContext_ConnectionInfo;
#define SEC_E_OK ((SECURITY_STATUS)0x00000000L)
#define SEC_I_CONTINUE_NEEDED ((SECURITY_STATUS)0x00090312L)
#define SEC_E_INCOMPLETE_MESSAGE ((SECURITY_STATUS)0x80090318L)
#define SEC_I_CONTEXT_EXPIRED ((SECURITY_STATUS)0x00090317L)
#define SEC_I_RENEGOTIATE ((SECURITY_STATUS)0x00090321L)
#define SECBUFFER_VERSION 0
#define SECBUFFER_EMPTY 0
#define SECBUFFER_DATA 1
#define SECBUFFER_TOKEN 2
#define SECBUFFER_EXTRA 5
#define SECBUFFER_STREAM_TRAILER 6
#define SECBUFFER_STREAM_HEADER 7
#define SECBUFFER_ALERT 17
#define SECPKG_CRED_OUTBOUND 0x00000002
