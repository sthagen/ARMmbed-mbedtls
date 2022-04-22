/*
 *  TLS 1.3 client-side functions
 *
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS ( https://tls.mbed.org )
 */

#include "common.h"

#if defined(MBEDTLS_SSL_CLI_C) && defined(MBEDTLS_SSL_PROTO_TLS1_3)

#include <string.h>

#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"

#include "ssl_misc.h"
#include "ssl_client.h"
#include "ssl_tls13_keys.h"

/* Write extensions */

/*
 * ssl_tls13_write_supported_versions_ext():
 *
 * struct {
 *      ProtocolVersion versions<2..254>;
 * } SupportedVersions;
 */
static int ssl_tls13_write_supported_versions_ext( mbedtls_ssl_context *ssl,
                                                   unsigned char *buf,
                                                   unsigned char *end,
                                                   size_t *out_len )
{
    unsigned char *p = buf;
    unsigned char versions_len = ( ssl->handshake->min_tls_version <=
                                   MBEDTLS_SSL_VERSION_TLS1_2 ) ? 4 : 2;

    *out_len = 0;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding supported versions extension" ) );

    /* Check if we have space to write the extension:
     * - extension_type         (2 bytes)
     * - extension_data_length  (2 bytes)
     * - versions_length        (1 byte )
     * - versions               (2 or 4 bytes)
     */
    MBEDTLS_SSL_CHK_BUF_PTR( p, end, 5 + versions_len );

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS, p, 0 );
    MBEDTLS_PUT_UINT16_BE( versions_len + 1, p, 2 );
    p += 4;

    /* Length of versions */
    *p++ = versions_len;

    /* Write values of supported versions.
     * They are defined by the configuration.
     * Currently, we advertise only TLS 1.3 or both TLS 1.3 and TLS 1.2.
     */
    mbedtls_ssl_write_version( p, MBEDTLS_SSL_TRANSPORT_STREAM,
                               MBEDTLS_SSL_VERSION_TLS1_3 );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "supported version: [3:4]" ) );


    if( ssl->handshake->min_tls_version <= MBEDTLS_SSL_VERSION_TLS1_2 )
    {
        mbedtls_ssl_write_version( p + 2, MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_VERSION_TLS1_2 );
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "supported version: [3:3]" ) );
    }

    *out_len = 5 + versions_len;

    return( 0 );
}

static int ssl_tls13_parse_supported_versions_ext( mbedtls_ssl_context *ssl,
                                                   const unsigned char *buf,
                                                   const unsigned char *end )
{
    ((void) ssl);

    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, 2 );
    if( mbedtls_ssl_read_version( buf, ssl->conf->transport ) !=
          MBEDTLS_SSL_VERSION_TLS1_3 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "unexpected version" ) );

        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    if( &buf[2] != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "supported_versions ext data length incorrect" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,
                                      MBEDTLS_ERR_SSL_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    return( 0 );
}

#if defined(MBEDTLS_SSL_ALPN)
static int ssl_tls13_parse_alpn_ext( mbedtls_ssl_context *ssl,
                               const unsigned char *buf, size_t len )
{
    size_t list_len, name_len;
    const unsigned char *p = buf;
    const unsigned char *end = buf + len;

    /* If we didn't send it, the server shouldn't send it */
    if( ssl->conf->alpn_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );

    /*
     * opaque ProtocolName<1..2^8-1>;
     *
     * struct {
     *     ProtocolName protocol_name_list<2..2^16-1>
     * } ProtocolNameList;
     *
     * the "ProtocolNameList" MUST contain exactly one "ProtocolName"
     */

    /* Min length is 2 ( list_len ) + 1 ( name_len ) + 1 ( name ) */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 4 );

    list_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, list_len );

    name_len = *p++;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, list_len - 1 );

    /* Check that the server chosen protocol was in our list and save it */
    for ( const char **alpn = ssl->conf->alpn_list; *alpn != NULL; alpn++ )
    {
        if( name_len == strlen( *alpn ) &&
            memcmp( buf + 3, *alpn, name_len ) == 0 )
        {
            ssl->alpn_chosen = *alpn;
            return( 0 );
        }
    }

    return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
}
#endif /* MBEDTLS_SSL_ALPN */

static int ssl_tls13_reset_key_share( mbedtls_ssl_context *ssl )
{
    uint16_t group_id = ssl->handshake->offered_group_id;

    if( group_id == 0 )
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) )
    {
        int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
        psa_status_t status = PSA_ERROR_CORRUPTION_DETECTED;

        /* Destroy generated private key. */
        status = psa_destroy_key( ssl->handshake->ecdh_psa_privkey );
        if( status != PSA_SUCCESS )
        {
            ret = psa_ssl_status_to_mbedtls( status );
            MBEDTLS_SSL_DEBUG_RET( 1, "psa_destroy_key", ret );
            return( ret );
        }

        ssl->handshake->ecdh_psa_privkey = MBEDTLS_SVC_KEY_ID_INIT;
        return( 0 );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }

    return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
}

/*
 * Functions for writing key_share extension.
 */
#if defined(MBEDTLS_ECDH_C)
static int ssl_tls13_generate_and_write_ecdh_key_exchange(
                mbedtls_ssl_context *ssl,
                uint16_t named_group,
                unsigned char *buf,
                unsigned char *end,
                size_t *out_len )
{
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;
    psa_key_attributes_t key_attributes;
    size_t own_pubkey_len;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;
    size_t ecdh_bits = 0;

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Perform PSA-based ECDH computation." ) );

    /* Convert EC group to PSA key type. */
    if( ( handshake->ecdh_psa_type =
        mbedtls_psa_parse_tls_ecc_group( named_group, &ecdh_bits ) ) == 0 )
            return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );

    ssl->handshake->ecdh_bits = ecdh_bits;

    key_attributes = psa_key_attributes_init();
    psa_set_key_usage_flags( &key_attributes, PSA_KEY_USAGE_DERIVE );
    psa_set_key_algorithm( &key_attributes, PSA_ALG_ECDH );
    psa_set_key_type( &key_attributes, handshake->ecdh_psa_type );
    psa_set_key_bits( &key_attributes, handshake->ecdh_bits );

    /* Generate ECDH private key. */
    status = psa_generate_key( &key_attributes,
                                &handshake->ecdh_psa_privkey );
    if( status != PSA_SUCCESS )
    {
        ret = psa_ssl_status_to_mbedtls( status );
        MBEDTLS_SSL_DEBUG_RET( 1, "psa_generate_key", ret );
        return( ret );

    }

    /* Export the public part of the ECDH private key from PSA. */
    status = psa_export_public_key( handshake->ecdh_psa_privkey,
                                    buf, (size_t)( end - buf ),
                                    &own_pubkey_len );
    if( status != PSA_SUCCESS )
    {
        ret = psa_ssl_status_to_mbedtls( status );
        MBEDTLS_SSL_DEBUG_RET( 1, "psa_export_public_key", ret );
        return( ret );

    }

    *out_len = own_pubkey_len;

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C */

static int ssl_tls13_get_default_group_id( mbedtls_ssl_context *ssl,
                                           uint16_t *group_id )
{
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;


#if defined(MBEDTLS_ECDH_C)
    const uint16_t *group_list = mbedtls_ssl_get_groups( ssl );
    /* Pick first available ECDHE group compatible with TLS 1.3 */
    if( group_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_CONFIG );

    for ( ; *group_list != 0; group_list++ )
    {
        const mbedtls_ecp_curve_info *curve_info;
        curve_info = mbedtls_ecp_curve_info_from_tls_id( *group_list );
        if( curve_info != NULL &&
            mbedtls_ssl_tls13_named_group_is_ecdhe( *group_list ) )
        {
            *group_id = *group_list;
            return( 0 );
        }
    }
#else
    ((void) ssl);
    ((void) group_id);
#endif /* MBEDTLS_ECDH_C */

    /*
     * Add DHE named groups here.
     * Pick first available DHE group compatible with TLS 1.3
     */

    return( ret );
}

/*
 * ssl_tls13_write_key_share_ext
 *
 * Structure of key_share extension in ClientHello:
 *
 *  struct {
 *          NamedGroup group;
 *          opaque key_exchange<1..2^16-1>;
 *      } KeyShareEntry;
 *  struct {
 *          KeyShareEntry client_shares<0..2^16-1>;
 *      } KeyShareClientHello;
 */
static int ssl_tls13_write_key_share_ext( mbedtls_ssl_context *ssl,
                                          unsigned char *buf,
                                          unsigned char *end,
                                          size_t *out_len )
{
    unsigned char *p = buf;
    unsigned char *client_shares; /* Start of client_shares */
    size_t client_shares_len;     /* Length of client_shares */
    uint16_t group_id;
    int ret = MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE;

    *out_len = 0;

    /* Check if we have space for header and length fields:
     * - extension_type         (2 bytes)
     * - extension_data_length  (2 bytes)
     * - client_shares_length   (2 bytes)
     */
    MBEDTLS_SSL_CHK_BUF_PTR( p, end, 6 );
    p += 6;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello: adding key share extension" ) );

    /* HRR could already have requested something else. */
    group_id = ssl->handshake->offered_group_id;
    if( !mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) &&
        !mbedtls_ssl_tls13_named_group_is_dhe( group_id ) )
    {
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_get_default_group_id( ssl,
                                                              &group_id ) );
    }

    /*
     * Dispatch to type-specific key generation function.
     *
     * So far, we're only supporting ECDHE. With the introduction
     * of PQC KEMs, we'll want to have multiple branches, one per
     * type of KEM, and dispatch to the corresponding crypto. And
     * only one key share entry is allowed.
     */
    client_shares = p;
#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group_id ) )
    {
        /* Pointer to group */
        unsigned char *group = p;
        /* Length of key_exchange */
        size_t key_exchange_len = 0;

        /* Check there is space for header of KeyShareEntry
         * - group                  (2 bytes)
         * - key_exchange_length    (2 bytes)
         */
        MBEDTLS_SSL_CHK_BUF_PTR( p, end, 4 );
        p += 4;
        ret = ssl_tls13_generate_and_write_ecdh_key_exchange( ssl, group_id, p, end,
                                                              &key_exchange_len );
        p += key_exchange_len;
        if( ret != 0 )
            return( ret );

        /* Write group */
        MBEDTLS_PUT_UINT16_BE( group_id, group, 0 );
        /* Write key_exchange_length */
        MBEDTLS_PUT_UINT16_BE( key_exchange_len, group, 2 );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }
    else
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    /* Length of client_shares */
    client_shares_len = p - client_shares;
    if( client_shares_len == 0)
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "No key share defined." ) );
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
    }
    /* Write extension_type */
    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_KEY_SHARE, buf, 0 );
    /* Write extension_data_length */
    MBEDTLS_PUT_UINT16_BE( client_shares_len + 2, buf, 2 );
    /* Write client_shares_length */
    MBEDTLS_PUT_UINT16_BE( client_shares_len, buf, 4 );

    /* Update offered_group_id field */
    ssl->handshake->offered_group_id = group_id;

    /* Output the total length of key_share extension. */
    *out_len = p - buf;

    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, key_share extension", buf, *out_len );

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_KEY_SHARE;

cleanup:

    return( ret );
}

#if defined(MBEDTLS_ECDH_C)

static int ssl_tls13_read_public_ecdhe_share( mbedtls_ssl_context *ssl,
                                              const unsigned char *buf,
                                              size_t buf_len )
{
    uint8_t *p = (uint8_t*)buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Get size of the TLS opaque key_exchange field of the KeyShareEntry struct. */
    uint16_t peerkey_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check if key size is consistent with given buffer length. */
    if ( peerkey_len > ( buf_len - 2 ) )
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );

    /* Store peer's ECDH public key. */
    memcpy( handshake->ecdh_psa_peerkey, p, peerkey_len );
    handshake->ecdh_psa_peerkey_len = peerkey_len;

    return( 0 );
}
#endif /* MBEDTLS_ECDH_C */

/*
 * ssl_tls13_parse_hrr_key_share_ext()
 *      Parse key_share extension in Hello Retry Request
 *
 * struct {
 *        NamedGroup selected_group;
 * } KeyShareHelloRetryRequest;
 */
static int ssl_tls13_parse_hrr_key_share_ext( mbedtls_ssl_context *ssl,
                                              const unsigned char *buf,
                                              const unsigned char *end )
{
    const mbedtls_ecp_curve_info *curve_info = NULL;
    const unsigned char *p = buf;
    int selected_group;
    int found = 0;

    const uint16_t *group_list = mbedtls_ssl_get_groups( ssl );
    if( group_list == NULL )
        return( MBEDTLS_ERR_SSL_BAD_CONFIG );

    MBEDTLS_SSL_DEBUG_BUF( 3, "key_share extension", p, end - buf );

    /* Read selected_group */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    selected_group = MBEDTLS_GET_UINT16_BE( p, 0 );
    MBEDTLS_SSL_DEBUG_MSG( 3, ( "selected_group ( %d )", selected_group ) );

    /* Upon receipt of this extension in a HelloRetryRequest, the client
     * MUST first verify that the selected_group field corresponds to a
     * group which was provided in the "supported_groups" extension in the
     * original ClientHello.
     * The supported_group was based on the info in ssl->conf->group_list.
     *
     * If the server provided a key share that was not sent in the ClientHello
     * then the client MUST abort the handshake with an "illegal_parameter" alert.
     */
    for( ; *group_list != 0; group_list++ )
    {
        curve_info = mbedtls_ecp_curve_info_from_tls_id( *group_list );
        if( curve_info == NULL || curve_info->tls_id != selected_group )
            continue;

        /* We found a match */
        found = 1;
        break;
    }

    /* Client MUST verify that the selected_group field does not
     * correspond to a group which was provided in the "key_share"
     * extension in the original ClientHello. If the server sent an
     * HRR message with a key share already provided in the
     * ClientHello then the client MUST abort the handshake with
     * an "illegal_parameter" alert.
     */
    if( found == 0 || selected_group == ssl->handshake->offered_group_id )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid key share in HRR" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT(
                MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    /* Remember server's preference for next ClientHello */
    ssl->handshake->offered_group_id = selected_group;

    return( 0 );
}

/*
 * ssl_tls13_parse_key_share_ext()
 *      Parse key_share extension in Server Hello
 *
 * struct {
 *        KeyShareEntry server_share;
 * } KeyShareServerHello;
 * struct {
 *        NamedGroup group;
 *        opaque key_exchange<1..2^16-1>;
 * } KeyShareEntry;
 */
static int ssl_tls13_parse_key_share_ext( mbedtls_ssl_context *ssl,
                                          const unsigned char *buf,
                                          const unsigned char *end )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    uint16_t group, offered_group;

    /* ...
     * NamedGroup group; (2 bytes)
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    group = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check that the chosen group matches the one we offered. */
    offered_group = ssl->handshake->offered_group_id;
    if( offered_group != group )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
            ( "Invalid server key share, our group %u, their group %u",
              (unsigned) offered_group, (unsigned) group ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE,
                                      MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
        return( MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }

#if defined(MBEDTLS_ECDH_C)
    if( mbedtls_ssl_tls13_named_group_is_ecdhe( group ) )
    {
        const mbedtls_ecp_curve_info *curve_info =
            mbedtls_ecp_curve_info_from_tls_id( group );
        if( curve_info == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Invalid TLS curve group id" ) );
            return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );
        }

        MBEDTLS_SSL_DEBUG_MSG( 2, ( "ECDH curve: %s", curve_info->name ) );

        ret = ssl_tls13_read_public_ecdhe_share( ssl, p, end - p );
        if( ret != 0 )
            return( ret );
    }
    else
#endif /* MBEDTLS_ECDH_C */
    if( 0 /* other KEMs? */ )
    {
        /* Do something */
    }
    else
        return( MBEDTLS_ERR_SSL_INTERNAL_ERROR );

    ssl->handshake->extensions_present |= MBEDTLS_SSL_EXT_KEY_SHARE;
    return( ret );
}

/*
 * ssl_tls13_parse_cookie_ext()
 *      Parse cookie extension in Hello Retry Request
 *
 * struct {
 *        opaque cookie<1..2^16-1>;
 * } Cookie;
 *
 * When sending a HelloRetryRequest, the server MAY provide a "cookie"
 * extension to the client (this is an exception to the usual rule that
 * the only extensions that may be sent are those that appear in the
 * ClientHello).  When sending the new ClientHello, the client MUST copy
 * the contents of the extension received in the HelloRetryRequest into
 * a "cookie" extension in the new ClientHello.  Clients MUST NOT use
 * cookies in their initial ClientHello in subsequent connections.
 */
static int ssl_tls13_parse_cookie_ext( mbedtls_ssl_context *ssl,
                                       const unsigned char *buf,
                                       const unsigned char *end )
{
    uint16_t cookie_len;
    const unsigned char *p = buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Retrieve length field of cookie */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    cookie_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, cookie_len );
    MBEDTLS_SSL_DEBUG_BUF( 3, "cookie extension", p, cookie_len );

    mbedtls_free( handshake->cookie );
    handshake->hrr_cookie_len = 0;
    handshake->cookie = mbedtls_calloc( 1, cookie_len );
    if( handshake->cookie == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
                ( "alloc failed ( %ud bytes )",
                  cookie_len ) );
        return( MBEDTLS_ERR_SSL_ALLOC_FAILED );
    }

    memcpy( handshake->cookie, p, cookie_len );
    handshake->hrr_cookie_len = cookie_len;

    return( 0 );
}

static int ssl_tls13_write_cookie_ext( mbedtls_ssl_context *ssl,
                                       unsigned char *buf,
                                       unsigned char *end,
                                       size_t *out_len )
{
    unsigned char *p = buf;
    *out_len = 0;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    if( handshake->cookie == NULL )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "no cookie to send; skip extension" ) );
        return( 0 );
    }

    MBEDTLS_SSL_DEBUG_BUF( 3, "client hello, cookie",
                           handshake->cookie,
                           handshake->hrr_cookie_len );

    MBEDTLS_SSL_CHK_BUF_PTR( p, end, handshake->hrr_cookie_len + 6 );

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "client hello, adding cookie extension" ) );

    MBEDTLS_PUT_UINT16_BE( MBEDTLS_TLS_EXT_COOKIE, p, 0 );
    MBEDTLS_PUT_UINT16_BE( handshake->hrr_cookie_len + 2, p, 2 );
    MBEDTLS_PUT_UINT16_BE( handshake->hrr_cookie_len, p, 4 );
    p += 6;

    /* Cookie */
    memcpy( p, handshake->cookie, handshake->hrr_cookie_len );

    *out_len = handshake->hrr_cookie_len + 6;

    return( 0 );
}

int mbedtls_ssl_tls13_write_client_hello_exts( mbedtls_ssl_context *ssl,
                                               unsigned char *buf,
                                               unsigned char *end,
                                               size_t *out_len )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *p = buf;
    size_t ext_len;

    *out_len = 0;

    /* Write supported_versions extension
     *
     * Supported Versions Extension is mandatory with TLS 1.3.
     */
    ret = ssl_tls13_write_supported_versions_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;

    /* Echo the cookie if the server provided one in its preceding
     * HelloRetryRequest message.
     */
    ret = ssl_tls13_write_cookie_ext( ssl, p, end, &ext_len );
    if( ret != 0 )
        return( ret );
    p += ext_len;

    if( mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
    {
        ret = ssl_tls13_write_key_share_ext( ssl, p, end, &ext_len );
        if( ret != 0 )
            return( ret );
        p += ext_len;
    }

    *out_len = p - buf;

    return( 0 );
}

/*
 * Functions for parsing and processing Server Hello
 */

/**
 * \brief Detect if the ServerHello contains a supported_versions extension
 *        or not.
 *
 * \param[in] ssl  SSL context
 * \param[in] buf  Buffer containing the ServerHello message
 * \param[in] end  End of the buffer containing the ServerHello message
 *
 * \return 0 if the ServerHello does not contain a supported_versions extension
 * \return 1 if the ServerHello contains a supported_versions extension
 * \return A negative value if an error occurred while parsing the ServerHello.
 */
static int ssl_tls13_is_supported_versions_ext_present(
    mbedtls_ssl_context *ssl,
    const unsigned char *buf,
    const unsigned char *end )
{
    const unsigned char *p = buf;
    size_t legacy_session_id_echo_len;
    size_t extensions_len;
    const unsigned char *extensions_end;

    /*
     * Check there is enough data to access the legacy_session_id_echo vector
     * length:
     * - legacy_version                 2 bytes
     * - random                         MBEDTLS_SERVER_HELLO_RANDOM_LEN bytes
     * - legacy_session_id_echo length  1 byte
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 3 );
    p += MBEDTLS_SERVER_HELLO_RANDOM_LEN + 2;
    legacy_session_id_echo_len = *p;

    /*
     * Jump to the extensions, jumping over:
     * - legacy_session_id_echo     (legacy_session_id_echo_len + 1) bytes
     * - cipher_suite               2 bytes
     * - legacy_compression_method  1 byte
     */
     p += legacy_session_id_echo_len + 4;

    /* Case of no extension */
    if( p == end )
        return( 0 );

    /* ...
     * Extension extensions<6..2^16-1>;
     * ...
     * struct {
     *      ExtensionType extension_type; (2 bytes)
     *      opaque extension_data<0..2^16-1>;
     * } Extension;
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check extensions do not go beyond the buffer of data. */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        if( extension_type == MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS )
            return( 1 );

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );
        p += extension_data_len;
    }

    return( 0 );
}

/* Returns a negative value on failure, and otherwise
 * - 1 if the last eight bytes of the ServerHello random bytes indicate that
 *     the server is TLS 1.3 capable but negotiating TLS 1.2 or below.
 * - 0 otherwise
 */
static int ssl_tls13_is_downgrade_negotiation( mbedtls_ssl_context *ssl,
                                               const unsigned char *buf,
                                               const unsigned char *end )
{
    /* First seven bytes of the magic downgrade strings, see RFC 8446 4.1.3 */
    static const unsigned char magic_downgrade_string[] =
        { 0x44, 0x4F, 0x57, 0x4E, 0x47, 0x52, 0x44 };
    const unsigned char *last_eight_bytes_of_random;
    unsigned char last_byte_of_random;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 2 );
    last_eight_bytes_of_random = buf + 2 + MBEDTLS_SERVER_HELLO_RANDOM_LEN - 8;

    if( memcmp( last_eight_bytes_of_random,
                magic_downgrade_string,
                sizeof( magic_downgrade_string ) ) == 0 )
    {
        last_byte_of_random = last_eight_bytes_of_random[7];
        return( last_byte_of_random == 0 ||
                last_byte_of_random == 1    );
    }

    return( 0 );
}

/* Returns a negative value on failure, and otherwise
 * - SSL_SERVER_HELLO_COORDINATE_HELLO or
 * - SSL_SERVER_HELLO_COORDINATE_HRR
 * to indicate which message is expected and to be parsed next.
 */
#define SSL_SERVER_HELLO_COORDINATE_HELLO 0
#define SSL_SERVER_HELLO_COORDINATE_HRR 1
static int ssl_server_hello_is_hrr( mbedtls_ssl_context *ssl,
                                    const unsigned char *buf,
                                    const unsigned char *end )
{
    static const unsigned char magic_hrr_string[MBEDTLS_SERVER_HELLO_RANDOM_LEN] =
        { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
          0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
          0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
          0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33 ,0x9C };

    /* Check whether this message is a HelloRetryRequest ( HRR ) message.
     *
     * Server Hello and HRR are only distinguished by Random set to the
     * special value of the SHA-256 of "HelloRetryRequest".
     *
     * struct {
     *    ProtocolVersion legacy_version = 0x0303;
     *    Random random;
     *    opaque legacy_session_id_echo<0..32>;
     *    CipherSuite cipher_suite;
     *    uint8 legacy_compression_method = 0;
     *    Extension extensions<6..2^16-1>;
     * } ServerHello;
     *
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( buf, end, 2 + sizeof( magic_hrr_string ) );

    if( memcmp( buf + 2, magic_hrr_string, sizeof( magic_hrr_string ) ) == 0 )
    {
        return( SSL_SERVER_HELLO_COORDINATE_HRR );
    }

    return( SSL_SERVER_HELLO_COORDINATE_HELLO );
}

/* Fetch and preprocess
 * Returns a negative value on failure, and otherwise
 * - SSL_SERVER_HELLO_COORDINATE_HELLO or
 * - SSL_SERVER_HELLO_COORDINATE_HRR or
 * - SSL_SERVER_HELLO_COORDINATE_TLS1_2
 */
#define SSL_SERVER_HELLO_COORDINATE_TLS1_2 2
static int ssl_tls13_server_hello_coordinate( mbedtls_ssl_context *ssl,
                                              unsigned char **buf,
                                              size_t *buf_len )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *end;

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                             MBEDTLS_SSL_HS_SERVER_HELLO,
                                             buf, buf_len ) );
    end = *buf + *buf_len;

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_is_supported_versions_ext_present(
                                  ssl, *buf, end ) );
    if( ret == 0 )
    {
        MBEDTLS_SSL_PROC_CHK_NEG(
            ssl_tls13_is_downgrade_negotiation( ssl, *buf, end ) );

        /* If the server is negotiating TLS 1.2 or below and:
         * . we did not propose TLS 1.2 or
         * . the server responded it is TLS 1.3 capable but negotiating a lower
         *   version of the protocol and thus we are under downgrade attack
         * abort the handshake with an "illegal parameter" alert.
         */
        if( ssl->handshake->min_tls_version > MBEDTLS_SSL_VERSION_TLS1_2 || ret )
        {
            MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                          MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        }

        ssl->keep_current_message = 1;
        ssl->tls_version = MBEDTLS_SSL_VERSION_TLS1_2;
        mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                            *buf, *buf_len );

        if( mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
        {
            ret = ssl_tls13_reset_key_share( ssl );
            if( ret != 0 )
                return( ret );
        }

        return( SSL_SERVER_HELLO_COORDINATE_TLS1_2 );
    }

    ret = ssl_server_hello_is_hrr( ssl, *buf, end );
    switch( ret )
    {
        case SSL_SERVER_HELLO_COORDINATE_HELLO:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received ServerHello message" ) );
            break;
        case SSL_SERVER_HELLO_COORDINATE_HRR:
            MBEDTLS_SSL_DEBUG_MSG( 2, ( "received HelloRetryRequest message" ) );
             /* If a client receives a second
              * HelloRetryRequest in the same connection (i.e., where the ClientHello
              * was itself in response to a HelloRetryRequest), it MUST abort the
              * handshake with an "unexpected_message" alert.
              */
            if( ssl->handshake->hello_retry_request_count > 0 )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1, ( "Multiple HRRs received" ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNEXPECTED_MESSAGE,
                                    MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
                return( MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE );
            }
            /*
             * Clients must abort the handshake with an "illegal_parameter"
             * alert if the HelloRetryRequest would not result in any change
             * in the ClientHello.
             * In a PSK only key exchange that what we expect.
             */
            if( ! mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
            {
                MBEDTLS_SSL_DEBUG_MSG( 1,
                            ( "Unexpected HRR in pure PSK key exchange." ) );
                MBEDTLS_SSL_PEND_FATAL_ALERT(
                            MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                            MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);
                return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
            }

            ssl->handshake->hello_retry_request_count++;

            break;
    }

cleanup:

    return( ret );
}

static int ssl_tls13_check_server_hello_session_id_echo( mbedtls_ssl_context *ssl,
                                                         const unsigned char **buf,
                                                         const unsigned char *end )
{
    const unsigned char *p = *buf;
    size_t legacy_session_id_echo_len;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    legacy_session_id_echo_len = *p++ ;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, legacy_session_id_echo_len );

    /* legacy_session_id_echo */
    if( ssl->session_negotiate->id_len != legacy_session_id_echo_len ||
        memcmp( ssl->session_negotiate->id, p , legacy_session_id_echo_len ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_BUF( 3, "Expected Session ID",
                               ssl->session_negotiate->id,
                               ssl->session_negotiate->id_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Received Session ID", p,
                               legacy_session_id_echo_len );

        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER);

        return( MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
    }

    p += legacy_session_id_echo_len;
    *buf = p;

    MBEDTLS_SSL_DEBUG_BUF( 3, "Session ID", ssl->session_negotiate->id,
                            ssl->session_negotiate->id_len );
    return( 0 );
}

static int ssl_tls13_cipher_suite_is_offered( mbedtls_ssl_context *ssl,
                                              int cipher_suite )
{
    const int *ciphersuite_list = ssl->conf->ciphersuite_list;

    /* Check whether we have offered this ciphersuite */
    for ( size_t i = 0; ciphersuite_list[i] != 0; i++ )
    {
        if( ciphersuite_list[i] == cipher_suite )
        {
            return( 1 );
        }
    }
    return( 0 );
}

/* Parse ServerHello message and configure context
 *
 * struct {
 *    ProtocolVersion legacy_version = 0x0303; // TLS 1.2
 *    Random random;
 *    opaque legacy_session_id_echo<0..32>;
 *    CipherSuite cipher_suite;
 *    uint8 legacy_compression_method = 0;
 *    Extension extensions<6..2^16-1>;
 * } ServerHello;
 */
static int ssl_tls13_parse_server_hello( mbedtls_ssl_context *ssl,
                                         const unsigned char *buf,
                                         const unsigned char *end,
                                         int is_hrr )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;
    size_t extensions_len;
    const unsigned char *extensions_end;
    uint16_t cipher_suite;
    const mbedtls_ssl_ciphersuite_t *ciphersuite_info;
    int fatal_alert = 0;

    /*
     * Check there is space for minimal fields
     *
     * - legacy_version             ( 2 bytes)
     * - random                     (MBEDTLS_SERVER_HELLO_RANDOM_LEN bytes)
     * - legacy_session_id_echo     ( 1 byte ), minimum size
     * - cipher_suite               ( 2 bytes)
     * - legacy_compression_method  ( 1 byte )
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, MBEDTLS_SERVER_HELLO_RANDOM_LEN + 6 );

    MBEDTLS_SSL_DEBUG_BUF( 4, "server hello", p, end - p );
    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, version", p, 2 );

    /* ...
     * ProtocolVersion legacy_version = 0x0303; // TLS 1.2
     * ...
     * with ProtocolVersion defined as:
     * uint16 ProtocolVersion;
     */
    if( mbedtls_ssl_read_version( p, ssl->conf->transport ) !=
          MBEDTLS_SSL_VERSION_TLS1_2 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "Unsupported version of TLS." ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION,
                                      MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION );
        ret = MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION;
        goto cleanup;
    }
    p += 2;

    /* ...
     * Random random;
     * ...
     * with Random defined as:
     * opaque Random[MBEDTLS_SERVER_HELLO_RANDOM_LEN];
     */
    if( !is_hrr )
    {
        memcpy( &handshake->randbytes[MBEDTLS_CLIENT_HELLO_RANDOM_LEN], p,
                MBEDTLS_SERVER_HELLO_RANDOM_LEN );
        MBEDTLS_SSL_DEBUG_BUF( 3, "server hello, random bytes",
                               p, MBEDTLS_SERVER_HELLO_RANDOM_LEN );
    }
    p += MBEDTLS_SERVER_HELLO_RANDOM_LEN;

    /* ...
     * opaque legacy_session_id_echo<0..32>;
     * ...
     */
    if( ssl_tls13_check_server_hello_session_id_echo( ssl, &p, end ) != 0 )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
        goto cleanup;
    }

    /* ...
     * CipherSuite cipher_suite;
     * ...
     * with CipherSuite defined as:
     * uint8 CipherSuite[2];
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    cipher_suite = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;


    ciphersuite_info = mbedtls_ssl_ciphersuite_from_id( cipher_suite );
    /*
     * Check whether this ciphersuite is valid and offered.
     */
    if( ( mbedtls_ssl_validate_ciphersuite( ssl, ciphersuite_info,
                                            ssl->tls_version,
                                            ssl->tls_version ) != 0 ) ||
        !ssl_tls13_cipher_suite_is_offered( ssl, cipher_suite ) )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
    }
    /*
     * If we received an HRR before and that the proposed selected
     * ciphersuite in this server hello is not the same as the one
     * proposed in the HRR, we abort the handshake and send an
     * "illegal_parameter" alert.
     */
    else if( ( !is_hrr ) && ( handshake->hello_retry_request_count > 0 ) &&
             ( cipher_suite != ssl->session_negotiate->ciphersuite ) )
    {
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
    }

    if( fatal_alert == MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid ciphersuite(%04x) parameter",
                                    cipher_suite ) );
        goto cleanup;
    }

    /* Configure ciphersuites */
    mbedtls_ssl_optimize_checksum( ssl, ciphersuite_info );

    handshake->ciphersuite_info = ciphersuite_info;
    ssl->session_negotiate->ciphersuite = cipher_suite;

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "server hello, chosen ciphersuite: ( %04x ) - %s",
                                 cipher_suite, ciphersuite_info->name ) );

#if defined(MBEDTLS_HAVE_TIME)
    ssl->session_negotiate->start = time( NULL );
#endif /* MBEDTLS_HAVE_TIME */

    /* ...
     * uint8 legacy_compression_method = 0;
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    if( p[0] != 0 )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "bad legacy compression method" ) );
        fatal_alert = MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER;
        goto cleanup;
    }
    p++;

    /* ...
     * Extension extensions<6..2^16-1>;
     * ...
     * struct {
     *      ExtensionType extension_type; (2 bytes)
     *      opaque extension_data<0..2^16-1>;
     * } Extension;
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    /* Check extensions do not go beyond the buffer of data. */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    MBEDTLS_SSL_DEBUG_BUF( 3, "server hello extensions", p, extensions_len );

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;
        const unsigned char *extension_data_end;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );
        extension_data_end = p + extension_data_len;

        switch( extension_type )
        {
            case MBEDTLS_TLS_EXT_COOKIE:

                if( !is_hrr )
                {
                    fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                    goto cleanup;
                }

                ret = ssl_tls13_parse_cookie_ext( ssl,
                                                  p, extension_data_end );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1,
                                           "ssl_tls13_parse_cookie_ext",
                                           ret );
                    goto cleanup;
                }
                break;

            case MBEDTLS_TLS_EXT_SUPPORTED_VERSIONS:
                ret = ssl_tls13_parse_supported_versions_ext( ssl,
                                                              p,
                                                              extension_data_end );
                if( ret != 0 )
                    goto cleanup;
                break;

            case MBEDTLS_TLS_EXT_PRE_SHARED_KEY:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found pre_shared_key extension." ) );
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "pre_shared_key:Not supported yet" ) );

                fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                goto cleanup;

            case MBEDTLS_TLS_EXT_KEY_SHARE:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found key_shares extension" ) );
                if( ! mbedtls_ssl_conf_tls13_some_ephemeral_enabled( ssl ) )
                {
                    fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                    goto cleanup;
                }

                if( is_hrr )
                    ret = ssl_tls13_parse_hrr_key_share_ext( ssl,
                                            p, extension_data_end );
                else
                    ret = ssl_tls13_parse_key_share_ext( ssl,
                                            p, extension_data_end );
                if( ret != 0 )
                {
                    MBEDTLS_SSL_DEBUG_RET( 1,
                                           "ssl_tls13_parse_key_share_ext",
                                           ret );
                    goto cleanup;
                }
                break;

            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3,
                    ( "unknown extension found: %u ( ignoring )",
                      extension_type ) );

                fatal_alert = MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT;
                goto cleanup;
        }

        p += extension_data_len;
    }

cleanup:

    if( fatal_alert == MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT,
                                      MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
        ret = MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION;
    }
    else if ( fatal_alert == MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER )
    {
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_ILLEGAL_PARAMETER,
                                      MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER );
        ret = MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER;
    }
    return( ret );
}

static int ssl_tls13_postprocess_server_hello( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ssl_key_set traffic_keys;
    mbedtls_ssl_transform *transform_handshake = NULL;
    mbedtls_ssl_handshake_params *handshake = ssl->handshake;

    /* Determine the key exchange mode:
     * 1) If both the pre_shared_key and key_share extensions were received
     *    then the key exchange mode is PSK with EPHEMERAL.
     * 2) If only the pre_shared_key extension was received then the key
     *    exchange mode is PSK-only.
     * 3) If only the key_share extension was received then the key
     *    exchange mode is EPHEMERAL-only.
     */
    switch( handshake->extensions_present &
            ( MBEDTLS_SSL_EXT_PRE_SHARED_KEY | MBEDTLS_SSL_EXT_KEY_SHARE ) )
    {
        /* Only the pre_shared_key extension was received */
        case MBEDTLS_SSL_EXT_PRE_SHARED_KEY:
            handshake->tls13_kex_modes = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK;
            break;

        /* Only the key_share extension was received */
        case MBEDTLS_SSL_EXT_KEY_SHARE:
            handshake->tls13_kex_modes = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL;
            break;

        /* Both the pre_shared_key and key_share extensions were received */
        case ( MBEDTLS_SSL_EXT_PRE_SHARED_KEY | MBEDTLS_SSL_EXT_KEY_SHARE ):
            handshake->tls13_kex_modes = MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL;
            break;

        /* Neither pre_shared_key nor key_share extension was received */
        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "Unknown key exchange." ) );
            ret = MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
            goto cleanup;
    }

    /* Start the TLS 1.3 key schedule: Set the PSK and derive early secret.
     *
     * TODO: We don't have to do this in case we offered 0-RTT and the
     *       server accepted it. In this case, we could skip generating
     *       the early secret. */
    ret = mbedtls_ssl_tls13_key_schedule_stage_early( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_key_schedule_stage_early_data",
                               ret );
        goto cleanup;
    }

    /* Compute handshake secret */
    ret = mbedtls_ssl_tls13_key_schedule_stage_handshake( ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_derive_master_secret", ret );
        goto cleanup;
    }

    /* Next evolution in key schedule: Establish handshake secret and
     * key material. */
    ret = mbedtls_ssl_tls13_generate_handshake_keys( ssl, &traffic_keys );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_generate_handshake_keys",
                               ret );
        goto cleanup;
    }

    transform_handshake = mbedtls_calloc( 1, sizeof( mbedtls_ssl_transform ) );
    if( transform_handshake == NULL )
    {
        ret = MBEDTLS_ERR_SSL_ALLOC_FAILED;
        goto cleanup;
    }

    ret = mbedtls_ssl_tls13_populate_transform( transform_handshake,
                              ssl->conf->endpoint,
                              ssl->session_negotiate->ciphersuite,
                              &traffic_keys,
                              ssl );
    if( ret != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_tls13_populate_transform", ret );
        goto cleanup;
    }

    handshake->transform_handshake = transform_handshake;
    mbedtls_ssl_set_inbound_transform( ssl, transform_handshake );

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to handshake keys for inbound traffic" ) );
    ssl->session_in = ssl->session_negotiate;

    /*
     * State machine update
     */
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_ENCRYPTED_EXTENSIONS );

cleanup:

    mbedtls_platform_zeroize( &traffic_keys, sizeof( traffic_keys ) );
    if( ret != 0 )
    {
        mbedtls_free( transform_handshake );

        MBEDTLS_SSL_PEND_FATAL_ALERT(
            MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE,
            MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE );
    }
    return( ret );
}

static int ssl_tls13_postprocess_hrr( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    /* If not offering early data, the client sends a dummy CCS record
     * immediately before its second flight. This may either be before
     * its second ClientHello or before its encrypted handshake flight.
     */
    mbedtls_ssl_handshake_set_state( ssl,
            MBEDTLS_SSL_CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO );
#else
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    mbedtls_ssl_session_reset_msg_layer( ssl, 0 );

    /*
     * We are going to re-generate a shared secret corresponding to the group
     * selected by the server, which is different from the group for which we
     * generated a shared secret in the first client hello.
     * Thus, reset the shared secret.
     */
    ret = ssl_tls13_reset_key_share( ssl );
    if( ret != 0 )
        return( ret );

    return( 0 );
}

/*
 * Wait and parse ServerHello handshake message.
 * Handler for MBEDTLS_SSL_SERVER_HELLO
 */
static int ssl_tls13_process_server_hello( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char *buf = NULL;
    size_t buf_len = 0;
    int is_hrr = 0;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> %s", __func__ ) );

    /* Coordination step
     * - Fetch record
     * - Make sure it's either a ServerHello or a HRR.
     * - Switch processing routine in case of HRR
     */
    ssl->handshake->extensions_present = MBEDTLS_SSL_EXT_NONE;

    ret = ssl_tls13_server_hello_coordinate( ssl, &buf, &buf_len );
    if( ret < 0 )
        goto cleanup;
    else
        is_hrr = ( ret == SSL_SERVER_HELLO_COORDINATE_HRR );

    if( ret == SSL_SERVER_HELLO_COORDINATE_TLS1_2 )
    {
        ret = 0;
        goto cleanup;
    }

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_parse_server_hello( ssl, buf,
                                                        buf + buf_len,
                                                        is_hrr ) );
    if( is_hrr )
        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_reset_transcript_for_hrr( ssl ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_SERVER_HELLO,
                                        buf, buf_len );

    if( is_hrr )
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_hrr( ssl ) );
    else
        MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_server_hello( ssl ) );

cleanup:
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= %s ( %s )", __func__,
                                is_hrr?"HelloRetryRequest":"ServerHello" ) );
    return( ret );
}

/*
 *
 * EncryptedExtensions message
 *
 * The EncryptedExtensions message contains any extensions which
 * should be protected, i.e., any which are not needed to establish
 * the cryptographic context.
 */

/*
 * Overview
 */

/* Main entry point; orchestrates the other functions */
static int ssl_tls13_process_encrypted_extensions( mbedtls_ssl_context *ssl );

static int ssl_tls13_parse_encrypted_extensions( mbedtls_ssl_context *ssl,
                                                 const unsigned char *buf,
                                                 const unsigned char *end );
static int ssl_tls13_postprocess_encrypted_extensions( mbedtls_ssl_context *ssl );

/*
 * Handler for  MBEDTLS_SSL_ENCRYPTED_EXTENSIONS
 */
static int ssl_tls13_process_encrypted_extensions( mbedtls_ssl_context *ssl )
{
    int ret;
    unsigned char *buf;
    size_t buf_len;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse encrypted extensions" ) );

    MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                             MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS,
                                             &buf, &buf_len ) );

    /* Process the message contents */
    MBEDTLS_SSL_PROC_CHK(
        ssl_tls13_parse_encrypted_extensions( ssl, buf, buf + buf_len ) );

    mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_ENCRYPTED_EXTENSIONS,
                                        buf, buf_len );

    MBEDTLS_SSL_PROC_CHK( ssl_tls13_postprocess_encrypted_extensions( ssl ) );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse encrypted extensions" ) );
    return( ret );

}

/* Parse EncryptedExtensions message
 * struct {
 *     Extension extensions<0..2^16-1>;
 * } EncryptedExtensions;
 */
static int ssl_tls13_parse_encrypted_extensions( mbedtls_ssl_context *ssl,
                                                 const unsigned char *buf,
                                                 const unsigned char *end )
{
    int ret = 0;
    size_t extensions_len;
    const unsigned char *p = buf;
    const unsigned char *extensions_end;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_DEBUG_BUF( 3, "encrypted extensions", p, extensions_len );
    extensions_end = p + extensions_len;
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        /*
         * struct {
         *     ExtensionType extension_type; (2 bytes)
         *     opaque extension_data<0..2^16-1>;
         * } Extension;
         */
        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );

        /* The client MUST check EncryptedExtensions for the
         * presence of any forbidden extensions and if any are found MUST abort
         * the handshake with an "unsupported_extension" alert.
         */
        switch( extension_type )
        {

            case MBEDTLS_TLS_EXT_SUPPORTED_GROUPS:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found extensions supported groups" ) );
                break;

#if defined(MBEDTLS_SSL_ALPN)
            case MBEDTLS_TLS_EXT_ALPN:
                MBEDTLS_SSL_DEBUG_MSG( 3, ( "found alpn extension" ) );

                if( ( ret = ssl_tls13_parse_alpn_ext( ssl, p, (size_t)extension_data_len ) ) != 0 )
                {
                    return( ret );
                }

                break;
#endif /* MBEDTLS_SSL_ALPN */
            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3, ( "unsupported extension found: %u ", extension_type) );
                MBEDTLS_SSL_PEND_FATAL_ALERT(
                    MBEDTLS_SSL_ALERT_MSG_UNSUPPORTED_EXT,
                    MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
                return ( MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION );
        }

        p += extension_data_len;
    }

    /* Check that we consumed all the message. */
    if( p != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "EncryptedExtension lengths misaligned" ) );
        MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,
                                      MBEDTLS_ERR_SSL_DECODE_ERROR );
        return( MBEDTLS_ERR_SSL_DECODE_ERROR );
    }

    return( ret );
}

static int ssl_tls13_postprocess_encrypted_extensions( mbedtls_ssl_context *ssl )
{
#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
    if( mbedtls_ssl_tls13_some_psk_enabled( ssl ) )
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
    else
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CERTIFICATE_REQUEST );
#else
    ((void) ssl);
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
#endif
    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
/*
 *
 * STATE HANDLING: CertificateRequest
 *
 */
#define SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST 0
#define SSL_CERTIFICATE_REQUEST_SKIP           1
/* Coordination:
 * Deals with the ambiguity of not knowing if a CertificateRequest
 * will be sent. Returns a negative code on failure, or
 * - SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST
 * - SSL_CERTIFICATE_REQUEST_SKIP
 * indicating if a Certificate Request is expected or not.
 */
static int ssl_tls13_certificate_request_coordinate( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    if( mbedtls_ssl_tls13_some_psk_enabled( ssl ) )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3, ( "<= skip parse certificate request" ) );
        return( SSL_CERTIFICATE_REQUEST_SKIP );
    }

    if( ( ret = mbedtls_ssl_read_record( ssl, 0 ) ) != 0 )
    {
        MBEDTLS_SSL_DEBUG_RET( 1, "mbedtls_ssl_read_record", ret );
        return( ret );
    }
    ssl->keep_current_message = 1;

    if( ( ssl->in_msgtype == MBEDTLS_SSL_MSG_HANDSHAKE ) &&
        ( ssl->in_msg[0] == MBEDTLS_SSL_HS_CERTIFICATE_REQUEST ) )
    {
        return( SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST );
    }

    return( SSL_CERTIFICATE_REQUEST_SKIP );
}

/*
 * ssl_tls13_parse_certificate_request()
 *     Parse certificate request
 * struct {
 *   opaque certificate_request_context<0..2^8-1>;
 *   Extension extensions<2..2^16-1>;
 * } CertificateRequest;
 */
static int ssl_tls13_parse_certificate_request( mbedtls_ssl_context *ssl,
                                                const unsigned char *buf,
                                                const unsigned char *end )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    const unsigned char *p = buf;
    size_t certificate_request_context_len = 0;
    size_t extensions_len = 0;
    const unsigned char *extensions_end;
    unsigned char sig_alg_ext_found = 0;

    /* ...
     * opaque certificate_request_context<0..2^8-1>
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 1 );
    certificate_request_context_len = (size_t) p[0];
    p += 1;

    if( certificate_request_context_len > 0 )
    {
        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, certificate_request_context_len );
        MBEDTLS_SSL_DEBUG_BUF( 3, "Certificate Request Context",
                               p, certificate_request_context_len );

        mbedtls_ssl_handshake_params *handshake = ssl->handshake;
        handshake->certificate_request_context =
                mbedtls_calloc( 1, certificate_request_context_len );
        if( handshake->certificate_request_context == NULL )
        {
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "buffer too small" ) );
            return ( MBEDTLS_ERR_SSL_ALLOC_FAILED );
        }
        memcpy( handshake->certificate_request_context, p,
                certificate_request_context_len );
        p += certificate_request_context_len;
    }

    /* ...
     * Extension extensions<2..2^16-1>;
     * ...
     */
    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, 2 );
    extensions_len = MBEDTLS_GET_UINT16_BE( p, 0 );
    p += 2;

    MBEDTLS_SSL_CHK_BUF_READ_PTR( p, end, extensions_len );
    extensions_end = p + extensions_len;

    while( p < extensions_end )
    {
        unsigned int extension_type;
        size_t extension_data_len;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, 4 );
        extension_type = MBEDTLS_GET_UINT16_BE( p, 0 );
        extension_data_len = MBEDTLS_GET_UINT16_BE( p, 2 );
        p += 4;

        MBEDTLS_SSL_CHK_BUF_READ_PTR( p, extensions_end, extension_data_len );

        switch( extension_type )
        {
            case MBEDTLS_TLS_EXT_SIG_ALG:
                MBEDTLS_SSL_DEBUG_MSG( 3,
                        ( "found signature algorithms extension" ) );
                ret = mbedtls_ssl_tls13_parse_sig_alg_ext( ssl, p,
                              p + extension_data_len );
                if( ret != 0 )
                    return( ret );
                if( ! sig_alg_ext_found )
                    sig_alg_ext_found = 1;
                else
                {
                    MBEDTLS_SSL_DEBUG_MSG( 3,
                        ( "Duplicate signature algorithms extensions found" ) );
                    goto decode_error;
                }
                break;

            default:
                MBEDTLS_SSL_DEBUG_MSG(
                    3,
                    ( "unknown extension found: %u ( ignoring )",
                    extension_type ) );
                break;
        }
        p += extension_data_len;
    }
    /* Check that we consumed all the message. */
    if( p != end )
    {
        MBEDTLS_SSL_DEBUG_MSG( 1,
            ( "CertificateRequest misaligned" ) );
        goto decode_error;
    }
    /* Check that we found signature algorithms extension */
    if( ! sig_alg_ext_found )
    {
        MBEDTLS_SSL_DEBUG_MSG( 3,
            ( "no signature algorithms extension found" ) );
        goto decode_error;
    }

    ssl->handshake->client_auth = 1;
    return( 0 );

decode_error:
    MBEDTLS_SSL_PEND_FATAL_ALERT( MBEDTLS_SSL_ALERT_MSG_DECODE_ERROR,
                                  MBEDTLS_ERR_SSL_DECODE_ERROR );
    return( MBEDTLS_ERR_SSL_DECODE_ERROR );
}

/*
 * Handler for  MBEDTLS_SSL_CERTIFICATE_REQUEST
 */
static int ssl_tls13_process_certificate_request( mbedtls_ssl_context *ssl )
{
    int ret = MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "=> parse certificate request" ) );

    MBEDTLS_SSL_PROC_CHK_NEG( ssl_tls13_certificate_request_coordinate( ssl ) );

    if( ret == SSL_CERTIFICATE_REQUEST_EXPECT_REQUEST )
    {
        unsigned char *buf;
        size_t buf_len;

        MBEDTLS_SSL_PROC_CHK( mbedtls_ssl_tls13_fetch_handshake_msg( ssl,
                                            MBEDTLS_SSL_HS_CERTIFICATE_REQUEST,
                                            &buf, &buf_len ) );

        MBEDTLS_SSL_PROC_CHK( ssl_tls13_parse_certificate_request( ssl,
                                              buf, buf + buf_len ) );

        mbedtls_ssl_add_hs_msg_to_checksum( ssl, MBEDTLS_SSL_HS_CERTIFICATE_REQUEST,
                                            buf, buf_len );
    }
    else if( ret == SSL_CERTIFICATE_REQUEST_SKIP )
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= skip parse certificate request" ) );
        ret = 0;
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 1, ( "should never happen" ) );
        ret = MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        goto cleanup;
    }

    MBEDTLS_SSL_DEBUG_MSG( 3, ( "got %s certificate request",
                                ssl->handshake->client_auth ? "a" : "no" ) );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_CERTIFICATE );

cleanup:

    MBEDTLS_SSL_DEBUG_MSG( 2, ( "<= parse certificate request" ) );
    return( ret );
}

/*
 * Handler for MBEDTLS_SSL_SERVER_CERTIFICATE
 */
static int ssl_tls13_process_server_certificate( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_certificate( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CERTIFICATE_VERIFY );
    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_CERTIFICATE_VERIFY
 */
static int ssl_tls13_process_certificate_verify( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_certificate_verify( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_SERVER_FINISHED );
    return( 0 );
}
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

/*
 * Handler for MBEDTLS_SSL_SERVER_FINISHED
 */
static int ssl_tls13_process_server_finished( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_process_finished_message( ssl );
    if( ret != 0 )
        return( ret );

#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
    mbedtls_ssl_handshake_set_state(
        ssl,
        MBEDTLS_SSL_CLIENT_CCS_AFTER_SERVER_FINISHED );
#else
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE );
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_CLIENT_CERTIFICATE
 */
static int ssl_tls13_write_client_certificate( mbedtls_ssl_context *ssl )
{
    int non_empty_certificate_msg = 0;

    MBEDTLS_SSL_DEBUG_MSG( 1,
                  ( "Switch to handshake traffic keys for outbound traffic" ) );
    mbedtls_ssl_set_outbound_transform( ssl, ssl->handshake->transform_handshake );

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
    if( ssl->handshake->client_auth )
    {
        int ret = mbedtls_ssl_tls13_write_certificate( ssl );
        if( ret != 0 )
            return( ret );

        if( mbedtls_ssl_own_cert( ssl ) != NULL )
            non_empty_certificate_msg = 1;
    }
    else
    {
        MBEDTLS_SSL_DEBUG_MSG( 2, ( "No certificate message to send." ) );
    }
#endif

   if( non_empty_certificate_msg )
   {
        mbedtls_ssl_handshake_set_state( ssl,
                                         MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY );
   }
   else
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_FINISHED );

    return( 0 );
}

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
/*
 * Handler for MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY
 */
static int ssl_tls13_write_client_certificate_verify( mbedtls_ssl_context *ssl )
{
    int ret = mbedtls_ssl_tls13_write_certificate_verify( ssl );

    if( ret == 0 )
        mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_FINISHED );

    return( ret );
}
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

/*
 * Handler for MBEDTLS_SSL_CLIENT_FINISHED
 */
static int ssl_tls13_write_client_finished( mbedtls_ssl_context *ssl )
{
    int ret;

    ret = mbedtls_ssl_tls13_write_finished_message( ssl );
    if( ret != 0 )
        return( ret );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_FLUSH_BUFFERS );
    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_FLUSH_BUFFERS
 */
static int ssl_tls13_flush_buffers( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 2, ( "handshake: done" ) );
    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_WRAPUP );
    return( 0 );
}

/*
 * Handler for MBEDTLS_SSL_HANDSHAKE_WRAPUP
 */
static int ssl_tls13_handshake_wrapup( mbedtls_ssl_context *ssl )
{
    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for inbound traffic" ) );
    mbedtls_ssl_set_inbound_transform ( ssl, ssl->transform_application );

    MBEDTLS_SSL_DEBUG_MSG( 1, ( "Switch to application keys for outbound traffic" ) );
    mbedtls_ssl_set_outbound_transform( ssl, ssl->transform_application );

    mbedtls_ssl_tls13_handshake_wrapup( ssl );

    mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_HANDSHAKE_OVER );
    return( 0 );
}

int mbedtls_ssl_tls13_handshake_client_step( mbedtls_ssl_context *ssl )
{
    int ret = 0;

    switch( ssl->state )
    {
        /*
         * ssl->state is initialized as HELLO_REQUEST. It is the same
         * as CLIENT_HELLO state.
         */
        case MBEDTLS_SSL_HELLO_REQUEST:
        case MBEDTLS_SSL_CLIENT_HELLO:
            ret = mbedtls_ssl_write_client_hello( ssl );
            break;

        case MBEDTLS_SSL_SERVER_HELLO:
            ret = ssl_tls13_process_server_hello( ssl );
            break;

        case MBEDTLS_SSL_ENCRYPTED_EXTENSIONS:
            ret = ssl_tls13_process_encrypted_extensions( ssl );
            break;

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
        case MBEDTLS_SSL_CERTIFICATE_REQUEST:
            ret = ssl_tls13_process_certificate_request( ssl );
            break;

        case MBEDTLS_SSL_SERVER_CERTIFICATE:
            ret = ssl_tls13_process_server_certificate( ssl );
            break;

        case MBEDTLS_SSL_CERTIFICATE_VERIFY:
            ret = ssl_tls13_process_certificate_verify( ssl );
            break;
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

        case MBEDTLS_SSL_SERVER_FINISHED:
            ret = ssl_tls13_process_server_finished( ssl );
            break;

        case MBEDTLS_SSL_CLIENT_CERTIFICATE:
            ret = ssl_tls13_write_client_certificate( ssl );
            break;

#if defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
        case MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY:
            ret = ssl_tls13_write_client_certificate_verify( ssl );
            break;
#endif /* MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED */

        case MBEDTLS_SSL_CLIENT_FINISHED:
            ret = ssl_tls13_write_client_finished( ssl );
            break;

        case MBEDTLS_SSL_FLUSH_BUFFERS:
            ret = ssl_tls13_flush_buffers( ssl );
            break;

        case MBEDTLS_SSL_HANDSHAKE_WRAPUP:
            ret = ssl_tls13_handshake_wrapup( ssl );
            break;

        /*
         * Injection of dummy-CCS's for middlebox compatibility
         */
#if defined(MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
        case MBEDTLS_SSL_CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret == 0 )
                mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_HELLO );
            break;

        case MBEDTLS_SSL_CLIENT_CCS_AFTER_SERVER_FINISHED:
            ret = mbedtls_ssl_tls13_write_change_cipher_spec( ssl );
            if( ret == 0 )
                mbedtls_ssl_handshake_set_state( ssl, MBEDTLS_SSL_CLIENT_CERTIFICATE );
            break;
#endif /* MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE */

        default:
            MBEDTLS_SSL_DEBUG_MSG( 1, ( "invalid state %d", ssl->state ) );
            return( MBEDTLS_ERR_SSL_BAD_INPUT_DATA );
    }

    return( ret );
}

#endif /* MBEDTLS_SSL_CLI_C && MBEDTLS_SSL_PROTO_TLS1_3 */


