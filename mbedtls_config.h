#pragma once

// Based on the official pico-examples mbedtls_config_examples_common.h
// (pico_w/wifi/mbedtls_config_examples_common.h), used verbatim - its
// per-example wrapper file added nothing beyond #include-ing this content,
// so there's nothing to merge here beyond dropping the wrapper indirection.

// Workaround for some mbedtls source files using INT_MAX without including limits.h
#include <limits.h>

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// lwIP's bundled mqtt.c always creates its own TLS connection internally
// (mqtt_client_connect() -> altcp_tls_new()) with no hook for setting the expected
// hostname (mbedtls_ssl_set_hostname()) before the handshake starts - the mqtt_client_t/
// altcp_pcb internals are fully opaque to callers. Without a hostname set,
// MBEDTLS_SSL_VERIFY_REQUIRED refuses to proceed at all (MBEDTLS_ERR_SSL_CERTIFICATE_
// VERIFICATION_WITHOUT_HOSTNAME) as a deliberate safeguard against verifying "signed by a
// trusted CA" while forgetting to verify "is this actually the server I meant to talk to".
// This define explicitly accepts that gap: the full certificate chain up to
// AmazonRootCA1 is still verified (a random attacker can't just present a self-signed
// cert), but the certificate's CN/SAN is not checked against the endpoint hostname.
// Practical residual risk: an attacker would need both a DNS-hijack/rogue-AP AND another
// valid Amazon-signed IoT certificate from a different AWS account.
#define MBEDTLS_SSL_CLI_ALLOW_WEAK_CERTIFICATE_VERIFICATION_WITHOUT_HOSTNAME

#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048
// Was 4096 (chosen to silence altcp_tls's "RX decrypion buffer bigger than TCP_WND"
// warning) - almost certainly too small for AWS IoT's server certificate chain (leaf +
// Amazon intermediate CA) once combined with handshake framing overhead, which is the
// likely cause of mbedtls_ssl_handshake failing with -28928 while parsing state
// SERVER_CERTIFICATE. 8192 stays within TCP_WND (11680 in lwipopts.h, avoiding the
// warning) while giving the cert chain comfortable room.
#define MBEDTLS_SSL_IN_CONTENT_LEN     8192

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_ECP_DP_SECP192R1_ENABLED
#define MBEDTLS_ECP_DP_SECP224R1_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_SECP192K1_ENABLED
#define MBEDTLS_ECP_DP_SECP224K1_ENABLED
#define MBEDTLS_ECP_DP_SECP256K1_ENABLED
#define MBEDTLS_ECP_DP_BP256R1_ENABLED
#define MBEDTLS_ECP_DP_BP384R1_ENABLED
#define MBEDTLS_ECP_DP_BP512R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED // plain RSA key exchange
// ECDHE authenticated by an RSA cert (as opposed to ECDHE_ECDSA below, which needs an
// ECDSA cert) - the standard modern ciphersuite family for RSA-keyed TLS 1.2 clients like
// ours, and almost certainly what AWS IoT's server picks. Without this, our ciphersuite
// lookup table has no entry for the ID the server selects, and the handshake fails with
// MBEDTLS_ERR_SSL_BAD_INPUT_DATA as soon as we try to parse its ServerHello.
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_AES_FEWER_TABLES

/* TLS 1.2 */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_GCM_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_WRITE_C

// Needed to parse our PEM-format certificate/key/root-CA files
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

// Significantly speeds up mbedtls due to NIST curve optimizations.
#define MBEDTLS_ECP_NIST_OPTIM
