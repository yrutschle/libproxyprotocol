/*
 * libproxyprotocol is an ANSI C library to parse and create PROXY protocol v1 and v2 headers
 * Copyright (C) 2022  Kosmas Valianos (kosmas.valianos@gmail.com)
 *
 * The libproxyprotocol library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The libproxyprotocol library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <ws2tcpip.h>
    /* Caution: To be used only with fixed length arrays */
    #define _sprintf(buffer, format, ...) sprintf_s(buffer, sizeof(buffer), format, __VA_ARGS__)
#else
    #include <arpa/inet.h>
    /* sprintf() as snprintf does not exist in ANSI C */
    #define _sprintf(buffer, format, ...) sprintf(buffer, format, __VA_ARGS__)
#endif

#include "proxy_protocol.h"

#pragma pack(1)

/******************* PROXY Protocol Version 1 *******************/
/*
 * The maximum line lengths the receiver must support including the CRLF are :
  - TCP/IPv4 :
      "PROXY TCP4 255.255.255.255 255.255.255.255 65535 65535\r\n"
    => 5 + 1 + 4 + 1 + 15 + 1 + 15 + 1 + 5 + 1 + 5 + 2 = 56 chars

  - TCP/IPv6 :
      "PROXY TCP6 ffff:f...f:ffff ffff:f...f:ffff 65535 65535\r\n"
    => 5 + 1 + 4 + 1 + 39 + 1 + 39 + 1 + 5 + 1 + 5 + 2 = 104 chars

  - unknown connection (short form) :
      "PROXY UNKNOWN\r\n"
    => 5 + 1 + 7 + 2 = 15 chars

  - worst case (optional fields set to 0xff) :
      "PROXY UNKNOWN ffff:f...f:ffff ffff:f...f:ffff 65535 65535\r\n"
    => 5 + 1 + 7 + 1 + 39 + 1 + 39 + 1 + 5 + 1 + 5 + 2 = 107 chars

So a 108-byte buffer is always enough to store all the line and a trailing zero
for string processing.
 */

#define PP1_MAX_LENGHT 108
#define PP1_SIG        "PROXY"
#define CRLF           "\r\n"

/****************************************************************/

/******************* PROXY Protocol Version 2 *******************/

typedef struct
{
    uint8_t  sig[12];  /* hex 0D 0A 0D 0A 00 0D 0A 51 55 49 54 0A */
    uint8_t  ver_cmd;  /* protocol version and command */
    uint8_t  fam;      /* protocol family and address */
    uint16_t len;      /* number of following bytes part of the header */
} proxy_hdr_v2_t;

typedef union
{
    struct
    {        /* for TCP/UDP over IPv4, len = 12 */
        uint32_t src_addr;
        uint32_t dst_addr;
        uint16_t src_port;
        uint16_t dst_port;
    } ipv4_addr;
    struct
    {        /* for TCP/UDP over IPv6, len = 36 */
        uint8_t  src_addr[16];
        uint8_t  dst_addr[16];
        uint16_t src_port;
        uint16_t dst_port;
    } ipv6_addr;
    struct
    {        /* for AF_UNIX sockets, len = 216 */
        uint8_t src_addr[108];
        uint8_t dst_addr[108];
    } unix_addr;
} proxy_addr_t;

/* Type-Length-Value (TLV vectors) */
#define PP2_TYPE_ALPN           0x01
#define PP2_TYPE_AUTHORITY      0x02
#define PP2_TYPE_CRC32C         0x03
#define PP2_TYPE_NOOP           0x04
#define PP2_TYPE_UNIQUE_ID      0x05
#define PP2_TYPE_SSL            0x20
#define PP2_SUBTYPE_SSL_VERSION 0x21
#define PP2_SUBTYPE_SSL_CN      0x22
#define PP2_SUBTYPE_SSL_CIPHER  0x23
#define PP2_SUBTYPE_SSL_SIG_ALG 0x24
#define PP2_SUBTYPE_SSL_KEY_ALG 0x25
#define PP2_TYPE_NETNS          0x30
/* Custom TLVs */
#define PP2_TYPE_AWS            0xEA
#define PP2_TYPE_AZURE          0xEE

/* PP2_TYPE_AWS subtypes */
#define PP2_SUBTYPE_AWS_VPCE_ID 0x01

/* PP2_TYPE_AZURE subtypes */
#define PP2_SUBTYPE_AZURE_PRIVATEENDPOINT_LINKID 0x01

struct _pp2_tlv_t
{
    uint8_t type;
    uint8_t length_hi;
    uint8_t length_lo;
    uint8_t value[1];
};

/* PP2_TYPE_SSL <client> bit field  */
#define PP2_CLIENT_SSL       0x01
#define PP2_CLIENT_CERT_CONN 0x02
#define PP2_CLIENT_CERT_SESS 0x04

typedef struct
{
    uint8_t   client;
    uint32_t  verify;
    pp2_tlv_t sub_tlv[1];
} pp2_tlv_ssl_t;

typedef struct
{
    uint8_t type;
    uint8_t value[1];
} pp2_tlv_aws_t;

typedef struct
{
    uint8_t  type;
    uint32_t linkid;
} pp2_tlv_azure_t;

#define PP2_SIG "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"

/* ANSI C makes us suffer as we cannot have value[0] */
#define sizeof_pp2_tlv_t     ((uint16_t) sizeof(pp2_tlv_t) - 1)
#define sizeof_pp2_tlv_aws_t ((uint16_t) sizeof(pp2_tlv_aws_t) - 1)

/****************************************************************/

#pragma pack()

static const char *errors[] = {
    "No error",
    "Invalid PROXY protocol version given. Only 1 and 2 are valid",
    "v2 PROXY protocol header: wrong signature",
    "v2 PROXY protocol header: wrong version",
    "v2 PROXY protocol header: wrong command",
    "v2 PROXY protocol header: wrong address family",
    "v2 PROXY protocol header: wrong transport protocol",
    "v2 PROXY protocol header: length",
    "v2 PROXY protocol header: invalid IPv4 src IP",
    "v2 PROXY protocol header: invalid IPv4 dst IP",
    "v2 PROXY protocol header: invalid IPv6 src IP",
    "v2 PROXY protocol header: invalid IPv6 dst IP",
    "v2 PROXY protocol header: invalid TLV vector's length",
    "v2 PROXY protocol header: invalid PP2_TYPE_CRC32C",
    "v2 PROXY protocol header: invalid PP2_TYPE_SSL",
    "v2 PROXY protocol header: invalid PP2_TYPE_UNIQUE_ID",
    "v2 PROXY protocol header: invalid PP2_TYPE_AWS",
    "v2 PROXY protocol header: invalid PP2_TYPE_AZURE",
    "v1 PROXY protocol header: \"\\r\\n\" is missing",
    "v1 PROXY protocol header: \"PROXY\" is missing",
    "v1 PROXY protocol header: space is missing",
    "v1 PROXY protocol header: wrong transport protocol or address family",
    "v1 PROXY protocol header: invalid IPv4 src IP",
    "v1 PROXY protocol header: invalid IPv4 dst IP",
    "v1 PROXY protocol header: invalid IPv6 src IP",
    "v1 PROXY protocol header: invalid IPv6 dst IP",
    "v1 PROXY protocol header: invalid src port",
    "v1 PROXY protocol header: invalid dst port",
    "Heap memory allocation failure",
};

const char *pp_strerror(int32_t error)
{
    if (error < -ERR_HEAP_ALLOC || error > ERR_NULL)
    {
        return NULL;
    }
    return errors[-error];
}

static uint8_t parse_port(const char *value, uint16_t *usport)
{
    uint64_t port = strtoul(value, NULL, 10);
    if (port == 0 || port > UINT16_MAX)
    {
        return 0;
    }
    *usport = (uint16_t) port;
    return 1;
}

static pp2_tlv_t *tlv_new(uint8_t type, uint16_t length, const void *value)
{
    pp2_tlv_t *tlv = malloc(sizeof_pp2_tlv_t + length);
    if (!tlv)
    {
        return NULL;
    }
    tlv->type = type;
    tlv->length_hi = length >> 8;
    tlv->length_lo = length & 0x00ff;
    memcpy(tlv->value, value, length);
    return tlv;
}

static uint8_t tlv_array_append_tlv(tlv_array_t *tlv_array, pp2_tlv_t *tlv)
{
    if (!tlv_array->tlvs)
    {
        tlv_array->len = 0;
        tlv_array->size = 10;
        tlv_array->tlvs = malloc(tlv_array->size * sizeof(pp2_tlv_t*));
        if (!tlv_array->tlvs)
        {
            return 0;
        }
    }

    if (tlv_array->size == tlv_array->len)
    {
        tlv_array->size += 5;
        pp2_tlv_t **tlvs = realloc(tlv_array->tlvs, tlv_array->size * sizeof(pp2_tlv_t*));
        if (!tlvs)
        {
            return 0;
        }
        tlv_array->tlvs = tlvs;
    }

    tlv_array->len++;
    tlv_array->tlvs[tlv_array->len - 1] = tlv;
    return 1;
}

static uint8_t tlv_array_append_tlv_new(tlv_array_t *tlv_array, uint8_t type, uint16_t length, const void *value)
{
    pp2_tlv_t *tlv = tlv_new(type, length, value);
    if (!tlv || !tlv_array_append_tlv(tlv_array, tlv))
    {
        return 0;
    }
    return 1;
}

static uint8_t tlv_array_append_tlv_new_usascii(tlv_array_t *tlv_array, uint8_t type, uint16_t length, const void *value)
{
    pp2_tlv_t *tlv = tlv_new(type, length + 1, value);
    if (!tlv || !tlv_array_append_tlv(tlv_array, tlv))
    {
        return 0;
    }
    tlv->value[length] = '\0';
    return 1;
}

uint8_t pp_info_add_alpn(pp_info_t *pp_info, uint16_t length, const uint8_t *alpn)
{
    return tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_ALPN, length, alpn);
}

uint8_t pp_info_add_authority(pp_info_t *pp_info, uint16_t length, const uint8_t *host_name)
{
    return tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_AUTHORITY, length, host_name);
}

uint8_t pp_info_add_unique_id(pp_info_t *pp_info, uint16_t length, const uint8_t *unique_id)
{
    if (length > 128)
    {
        return 0;
    }
    return tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_UNIQUE_ID, length, unique_id);
}

static void pp_info_add_subtype_ssl(uint8_t *value, uint16_t *index, uint8_t subtype_ssl, uint16_t length, const void *subtype_ssl_value)
{
    if (!length || !subtype_ssl_value)
    {
        return;
    }
    value[(*index)++] = subtype_ssl;
    value[(*index)++] = length >> 8;
    value[(*index)++] = length & 0x00ff;
    memcpy(value + *index, subtype_ssl_value, length);
    *index += length;
}

uint8_t pp_info_add_ssl(pp_info_t *pp_info, const char *version, const char *cipher, const char *sig_alg, const char *key_alg, const uint8_t *cn, uint16_t cn_value_len)
{
    const pp2_ssl_info_t *pp2_ssl_info = &pp_info->pp2_info.pp2_ssl_info;
    uint8_t client = pp2_ssl_info->ssl | pp2_ssl_info->cert_in_connection << 1 | pp2_ssl_info->cert_in_connection << 2;
    uint32_t verify = !pp2_ssl_info->cert_verified;
    size_t version_value_len = version ? strlen(version) : 0;
    size_t cipher_value_len = cipher ? strlen(cipher) : 0;
    size_t sig_alg_value_len = sig_alg ? strlen(sig_alg) : 0;
    size_t key_alg_value_len = key_alg ? strlen(key_alg) : 0;
    size_t length = sizeof(client) + sizeof(verify)
        + sizeof_pp2_tlv_t + version_value_len
        + sizeof_pp2_tlv_t + cipher_value_len
        + sizeof_pp2_tlv_t + sig_alg_value_len
        + sizeof_pp2_tlv_t + key_alg_value_len
        + sizeof_pp2_tlv_t + cn_value_len;

    if (length > UINT16_MAX)
    {
        return 0;
    }

    uint16_t index = 0;
    uint8_t *value = malloc(length);
    value[index++] = client;
    memcpy(value + index, &verify, sizeof(verify));
    index += sizeof(verify);
    pp_info_add_subtype_ssl(value, &index, PP2_SUBTYPE_SSL_VERSION, (uint16_t) version_value_len, version);
    pp_info_add_subtype_ssl(value, &index, PP2_SUBTYPE_SSL_CIPHER, (uint16_t) cipher_value_len, cipher);
    pp_info_add_subtype_ssl(value, &index, PP2_SUBTYPE_SSL_SIG_ALG, (uint16_t) sig_alg_value_len, sig_alg);
    pp_info_add_subtype_ssl(value, &index, PP2_SUBTYPE_SSL_KEY_ALG, (uint16_t) key_alg_value_len, key_alg);
    pp_info_add_subtype_ssl(value, &index, PP2_SUBTYPE_SSL_CN, cn_value_len, cn);
    uint8_t rc = tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_SSL, (uint16_t) length, value);
    free(value);
    return rc;
}

uint8_t pp_info_add_netns(pp_info_t *pp_info, const char *netns)
{
    return tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_NETNS, (uint16_t) strlen(netns), netns);
}

uint8_t pp_info_add_aws_vpce_id(pp_info_t *pp_info, const char *vpce_id)
{
    uint16_t length = sizeof_pp2_tlv_aws_t + (uint16_t) strlen(vpce_id);
    pp2_tlv_aws_t *pp2_tlv_aws = malloc(length);
    pp2_tlv_aws->type = PP2_SUBTYPE_AWS_VPCE_ID;
    memcpy(pp2_tlv_aws->value, vpce_id, strlen(vpce_id));
    uint8_t rc = tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_AWS, length, pp2_tlv_aws);
    free(pp2_tlv_aws);
    return rc;
}

uint8_t pp_info_add_azure_linkid(pp_info_t *pp_info, uint32_t linkid)
{
    uint16_t length = (uint16_t) sizeof(pp2_tlv_azure_t);
    pp2_tlv_azure_t *pp2_tlv_azure = malloc(length);
    pp2_tlv_azure->type = PP2_SUBTYPE_AZURE_PRIVATEENDPOINT_LINKID;
    pp2_tlv_azure->linkid = linkid;
    uint8_t rc = tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, PP2_TYPE_AZURE, length, pp2_tlv_azure);
    free(pp2_tlv_azure);
    return rc;
}

static void tlv_array_clear(tlv_array_t *tlv_array)
{
    uint32_t i;
    for (i = 0; i < tlv_array->len; i++)
    {
        free(tlv_array->tlvs[i]);
        tlv_array->tlvs[i] = NULL;
    }
    tlv_array->len = 0;
    tlv_array->size = 0;
    free(tlv_array->tlvs);
    tlv_array->tlvs = NULL;
}

static const uint8_t *pp_info_get_tlv_value(const pp_info_t *pp_info, uint8_t type, uint8_t subtype, uint16_t *length)
{
    *length = 0;
    if (!pp_info->pp2_info.tlv_array.tlvs || !pp_info->pp2_info.tlv_array.len)
    {
        return NULL;
    }

    uint32_t i;
    for (i = 0; i < pp_info->pp2_info.tlv_array.len; i++)
    {
        pp2_tlv_t *tlv = pp_info->pp2_info.tlv_array.tlvs[i];
        if (tlv->type == type)
        {
            *length = tlv->length_hi << 8 | tlv->length_lo;
            if (subtype > 0)
            {
                if (tlv->value[0] == subtype)
                {
                    (*length)--;
                    return &tlv->value[1];
                }
                return NULL;
            }
            return tlv->value;
        }
    }
    return NULL;
}

const uint8_t *pp_info_get_alpn(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_ALPN, 0, length);
}

const uint8_t *pp_info_get_authority(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_AUTHORITY, 0, length);
}

const uint8_t *pp_info_get_crc32c(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_CRC32C, 0, length);
}

const uint8_t *pp_info_get_unique_id(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_UNIQUE_ID, 0, length);
}

const uint8_t *pp_info_get_ssl_version(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_SUBTYPE_SSL_VERSION, 0, length);
}

const uint8_t *pp_info_get_ssl_cn(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_SUBTYPE_SSL_CN, 0, length);
}

const uint8_t *pp_info_get_ssl_cipher(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_SUBTYPE_SSL_CIPHER, 0, length);
}

const uint8_t *pp_info_get_ssl_sig_alg(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_SUBTYPE_SSL_SIG_ALG, 0, length);
}

const uint8_t *pp_info_get_ssl_key_alg(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_SUBTYPE_SSL_KEY_ALG, 0, length);
}

const uint8_t *pp_info_get_netns(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_NETNS, 0, length);
}

const uint8_t *pp_info_get_aws_vpce_id(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_AWS, PP2_SUBTYPE_AWS_VPCE_ID, length);
}

const uint8_t *pp_info_get_azure_linkid(const pp_info_t *pp_info, uint16_t *length)
{
    return pp_info_get_tlv_value(pp_info, PP2_TYPE_AZURE, PP2_SUBTYPE_AZURE_PRIVATEENDPOINT_LINKID, length);
}

void pp_info_clear(pp_info_t *pp_info)
{
    tlv_array_clear(&pp_info->pp2_info.tlv_array);
    memset(pp_info, 0, sizeof(*pp_info));
}

/*****************************************************************/
/*                                                               */
/* CRC LOOKUP TABLE                                              */
/* ================                                              */
/* The following CRC lookup table was generated automagically    */
/* by the Rocksoft^tm Model CRC Algorithm Table Generation       */
/* Program V1.0 using the following model parameters:            */
/*                                                               */
/*    Width   : 4 bytes.                                         */
/*    Poly    : 0x1EDC6F41L                                      */
/*    Reverse : TRUE.                                            */
/*                                                               */
/* For more information on the Rocksoft^tm Model CRC Algorithm,  */
/* see the document titled "A Painless Guide to CRC Error        */
/* Detection Algorithms" by Ross Williams                        */
/* (ross@guest.adelaide.edu.au.). This document is likely to be  */
/* in the FTP archive "ftp.adelaide.edu.au/pub/rocksoft".        */
/*                                                               */
/*****************************************************************/

static uint32_t crctable[256] = {
 0x00000000L, 0xF26B8303L, 0xE13B70F7L, 0x1350F3F4L,
 0xC79A971FL, 0x35F1141CL, 0x26A1E7E8L, 0xD4CA64EBL,
 0x8AD958CFL, 0x78B2DBCCL, 0x6BE22838L, 0x9989AB3BL,
 0x4D43CFD0L, 0xBF284CD3L, 0xAC78BF27L, 0x5E133C24L,
 0x105EC76FL, 0xE235446CL, 0xF165B798L, 0x030E349BL,
 0xD7C45070L, 0x25AFD373L, 0x36FF2087L, 0xC494A384L,
 0x9A879FA0L, 0x68EC1CA3L, 0x7BBCEF57L, 0x89D76C54L,
 0x5D1D08BFL, 0xAF768BBCL, 0xBC267848L, 0x4E4DFB4BL,
 0x20BD8EDEL, 0xD2D60DDDL, 0xC186FE29L, 0x33ED7D2AL,
 0xE72719C1L, 0x154C9AC2L, 0x061C6936L, 0xF477EA35L,
 0xAA64D611L, 0x580F5512L, 0x4B5FA6E6L, 0xB93425E5L,
 0x6DFE410EL, 0x9F95C20DL, 0x8CC531F9L, 0x7EAEB2FAL,
 0x30E349B1L, 0xC288CAB2L, 0xD1D83946L, 0x23B3BA45L,
 0xF779DEAEL, 0x05125DADL, 0x1642AE59L, 0xE4292D5AL,
 0xBA3A117EL, 0x4851927DL, 0x5B016189L, 0xA96AE28AL,
 0x7DA08661L, 0x8FCB0562L, 0x9C9BF696L, 0x6EF07595L,
 0x417B1DBCL, 0xB3109EBFL, 0xA0406D4BL, 0x522BEE48L,
 0x86E18AA3L, 0x748A09A0L, 0x67DAFA54L, 0x95B17957L,
 0xCBA24573L, 0x39C9C670L, 0x2A993584L, 0xD8F2B687L,
 0x0C38D26CL, 0xFE53516FL, 0xED03A29BL, 0x1F682198L,
 0x5125DAD3L, 0xA34E59D0L, 0xB01EAA24L, 0x42752927L,
 0x96BF4DCCL, 0x64D4CECFL, 0x77843D3BL, 0x85EFBE38L,
 0xDBFC821CL, 0x2997011FL, 0x3AC7F2EBL, 0xC8AC71E8L,
 0x1C661503L, 0xEE0D9600L, 0xFD5D65F4L, 0x0F36E6F7L,
 0x61C69362L, 0x93AD1061L, 0x80FDE395L, 0x72966096L,
 0xA65C047DL, 0x5437877EL, 0x4767748AL, 0xB50CF789L,
 0xEB1FCBADL, 0x197448AEL, 0x0A24BB5AL, 0xF84F3859L,
 0x2C855CB2L, 0xDEEEDFB1L, 0xCDBE2C45L, 0x3FD5AF46L,
 0x7198540DL, 0x83F3D70EL, 0x90A324FAL, 0x62C8A7F9L,
 0xB602C312L, 0x44694011L, 0x5739B3E5L, 0xA55230E6L,
 0xFB410CC2L, 0x092A8FC1L, 0x1A7A7C35L, 0xE811FF36L,
 0x3CDB9BDDL, 0xCEB018DEL, 0xDDE0EB2AL, 0x2F8B6829L,
 0x82F63B78L, 0x709DB87BL, 0x63CD4B8FL, 0x91A6C88CL,
 0x456CAC67L, 0xB7072F64L, 0xA457DC90L, 0x563C5F93L,
 0x082F63B7L, 0xFA44E0B4L, 0xE9141340L, 0x1B7F9043L,
 0xCFB5F4A8L, 0x3DDE77ABL, 0x2E8E845FL, 0xDCE5075CL,
 0x92A8FC17L, 0x60C37F14L, 0x73938CE0L, 0x81F80FE3L,
 0x55326B08L, 0xA759E80BL, 0xB4091BFFL, 0x466298FCL,
 0x1871A4D8L, 0xEA1A27DBL, 0xF94AD42FL, 0x0B21572CL,
 0xDFEB33C7L, 0x2D80B0C4L, 0x3ED04330L, 0xCCBBC033L,
 0xA24BB5A6L, 0x502036A5L, 0x4370C551L, 0xB11B4652L,
 0x65D122B9L, 0x97BAA1BAL, 0x84EA524EL, 0x7681D14DL,
 0x2892ED69L, 0xDAF96E6AL, 0xC9A99D9EL, 0x3BC21E9DL,
 0xEF087A76L, 0x1D63F975L, 0x0E330A81L, 0xFC588982L,
 0xB21572C9L, 0x407EF1CAL, 0x532E023EL, 0xA145813DL,
 0x758FE5D6L, 0x87E466D5L, 0x94B49521L, 0x66DF1622L,
 0x38CC2A06L, 0xCAA7A905L, 0xD9F75AF1L, 0x2B9CD9F2L,
 0xFF56BD19L, 0x0D3D3E1AL, 0x1E6DCDEEL, 0xEC064EEDL,
 0xC38D26C4L, 0x31E6A5C7L, 0x22B65633L, 0xD0DDD530L,
 0x0417B1DBL, 0xF67C32D8L, 0xE52CC12CL, 0x1747422FL,
 0x49547E0BL, 0xBB3FFD08L, 0xA86F0EFCL, 0x5A048DFFL,
 0x8ECEE914L, 0x7CA56A17L, 0x6FF599E3L, 0x9D9E1AE0L,
 0xD3D3E1ABL, 0x21B862A8L, 0x32E8915CL, 0xC083125FL,
 0x144976B4L, 0xE622F5B7L, 0xF5720643L, 0x07198540L,
 0x590AB964L, 0xAB613A67L, 0xB831C993L, 0x4A5A4A90L,
 0x9E902E7BL, 0x6CFBAD78L, 0x7FAB5E8CL, 0x8DC0DD8FL,
 0xE330A81AL, 0x115B2B19L, 0x020BD8EDL, 0xF0605BEEL,
 0x24AA3F05L, 0xD6C1BC06L, 0xC5914FF2L, 0x37FACCF1L,
 0x69E9F0D5L, 0x9B8273D6L, 0x88D28022L, 0x7AB90321L,
 0xAE7367CAL, 0x5C18E4C9L, 0x4F48173DL, 0xBD23943EL,
 0xF36E6F75L, 0x0105EC76L, 0x12551F82L, 0xE03E9C81L,
 0x34F4F86AL, 0xC69F7B69L, 0xD5CF889DL, 0x27A40B9EL,
 0x79B737BAL, 0x8BDCB4B9L, 0x988C474DL, 0x6AE7C44EL,
 0xBE2DA0A5L, 0x4C4623A6L, 0x5F16D052L, 0xAD7D5351L
};

static uint32_t crc32c(const uint8_t* buf, uint32_t len)
{
    uint32_t crc = 0xffffffff;
    while (len-- > 0)
    {
        crc = (crc >> 8) ^ crctable[(crc ^ (*buf++)) & 0xFF];
    }
    return crc ^ 0xffffffff;
}

uint8_t *pp2_create_hdr(const pp_info_t *pp_info, uint16_t *pp2_hdr_len, int32_t *error)
{
    proxy_hdr_v2_t proxy_hdr_v2 = { .sig = PP2_SIG, .ver_cmd = '\x21' };
    uint16_t proxy_addr_len;
    proxy_addr_t proxy_addr;
    if (pp_info->address_family == ADDR_FAMILY_UNSPEC)
    {
        proxy_addr_len = 0;
        proxy_hdr_v2.ver_cmd = '\x20';
        if (!pp_info->pp2_info.local)
        {
            *error = -ERR_PP2_CMD;
            return NULL;
        }
    }
    else if (pp_info->address_family == ADDR_FAMILY_INET)
    {
        proxy_addr_len = 12;
        if (inet_pton(AF_INET, pp_info->src_addr, &proxy_addr.ipv4_addr.src_addr) != 1)
        {
            *error = -ERR_PP2_IPV4_SRC_IP;
            return NULL;
        }
        if (inet_pton(AF_INET, pp_info->dst_addr, &proxy_addr.ipv4_addr.dst_addr) != 1)
        {
            *error = -ERR_PP2_IPV4_DST_IP;
            return NULL;
        }
        proxy_addr.ipv4_addr.src_port = htons(pp_info->src_port);
        proxy_addr.ipv4_addr.dst_port = htons(pp_info->dst_port);
    }
    else if (pp_info->address_family == ADDR_FAMILY_INET6)
    {
        proxy_addr_len = 36;
        if (inet_pton(AF_INET6, pp_info->src_addr, &proxy_addr.ipv6_addr.src_addr) != 1)
        {
            *error = -ERR_PP2_IPV6_SRC_IP;
            return NULL;
        }
        if (inet_pton(AF_INET6, pp_info->dst_addr, &proxy_addr.ipv6_addr.dst_addr) != 1)
        {
            *error = -ERR_PP2_IPV6_DST_IP;
            return NULL;
        }
        proxy_addr.ipv6_addr.src_port = htons(pp_info->src_port);
        proxy_addr.ipv6_addr.dst_port = htons(pp_info->dst_port);
    }
    else if (pp_info->address_family == ADDR_FAMILY_UNIX)
    {
        proxy_addr_len = 216;
        memcpy(proxy_addr.unix_addr.src_addr, pp_info->src_addr, sizeof(pp_info->src_addr));
        memcpy(proxy_addr.unix_addr.dst_addr, pp_info->dst_addr, sizeof(pp_info->dst_addr));
    }
    else
    {
        *error = -ERR_PP2_ADDR_FAMILY;
        return NULL;
    }

    if (pp_info->transport_protocol > TRANSPORT_PROTOCOL_DGRAM)
    {
        *error = -ERR_PP2_TRANSPORT_PROTOCOL;
        return NULL;
    }

    proxy_hdr_v2.fam = pp_info->address_family << 4 | pp_info->transport_protocol;

    /* Calculate the total length */
    uint16_t len = proxy_addr_len;
    const tlv_array_t *tlv_array = &pp_info->pp2_info.tlv_array;
    uint16_t padding_bytes = 0;
    uint32_t i;
    for (i = 0; i < tlv_array->len; i++)
    {
        uint16_t tlv_len = sizeof_pp2_tlv_t + (tlv_array->tlvs[i]->length_hi << 8 | tlv_array->tlvs[i]->length_lo);
        len += tlv_len;
    }
    if (pp_info->pp2_info.crc32c)
    {
        len += sizeof_pp2_tlv_t + sizeof(uint32_t);
    }
    *pp2_hdr_len = sizeof(proxy_hdr_v2_t) + len;
    if (pp_info->pp2_info.alignment_power > 1)
    {
        uint16_t alignment = 1 << pp_info->pp2_info.alignment_power;
        if (*pp2_hdr_len % alignment)
        {
            uint16_t pp2_hdr_len_padded = (*pp2_hdr_len / alignment + 1) * alignment;
            /* The NOOP TLV needs to be at least 3 bytes because a TLV can not be smaller than that */
            if (pp2_hdr_len_padded - *pp2_hdr_len < sizeof_pp2_tlv_t)
            {
                pp2_hdr_len_padded += alignment;
            }
            padding_bytes = pp2_hdr_len_padded - sizeof(proxy_hdr_v2_t) - len - sizeof_pp2_tlv_t;

            *pp2_hdr_len = pp2_hdr_len_padded;
            len = pp2_hdr_len_padded - sizeof(proxy_hdr_v2_t);
        }
    }
    proxy_hdr_v2.len = htons(len);

    /* Create the PROXY protocol header */
    uint8_t *pp2_hdr = malloc(*pp2_hdr_len);
    if (!pp2_hdr)
    {
        *error = -ERR_HEAP_ALLOC;
        return NULL;
    }
    uint16_t index = 0;
    memcpy(pp2_hdr, &proxy_hdr_v2, sizeof(proxy_hdr_v2_t));
    index += sizeof(proxy_hdr_v2_t);
    memcpy(pp2_hdr + index, &proxy_addr, proxy_addr_len);
    index += proxy_addr_len;

    /* Append the TLVs */
    for (i = 0; i < tlv_array->len; i++)
    {
        uint16_t tlv_len = sizeof_pp2_tlv_t + (tlv_array->tlvs[i]->length_hi << 8 | tlv_array->tlvs[i]->length_lo);
        memcpy(pp2_hdr + index, tlv_array->tlvs[i], tlv_len);
        index += tlv_len;
    }
    if (pp_info->pp2_info.alignment_power > 1)
    {
        pp2_tlv_t tlv = {
            .type = PP2_TYPE_NOOP,
            .length_hi = padding_bytes >> 8,
            .length_lo = padding_bytes & 0x00ff
        };
        memcpy(pp2_hdr + index, &tlv, sizeof_pp2_tlv_t);
        index += sizeof_pp2_tlv_t;
        memset(pp2_hdr + index, 0, padding_bytes);
        index += padding_bytes;
    }
    if (pp_info->pp2_info.crc32c)
    {
        pp2_tlv_t tlv = { .type = PP2_TYPE_CRC32C, .length_lo = sizeof(uint32_t) };
        memcpy(pp2_hdr + index, &tlv, sizeof_pp2_tlv_t);
        index += sizeof_pp2_tlv_t;
        memset(pp2_hdr + index, 0, sizeof(uint32_t));
        uint32_t crc32c_calculated = crc32c(pp2_hdr, *pp2_hdr_len);
        memcpy(pp2_hdr + index, &crc32c_calculated, sizeof(uint32_t));
    }

    *error = ERR_NULL;
    return pp2_hdr;
}

uint8_t *pp2_create_healthcheck_hdr(uint16_t *pp2_hdr_len, int32_t *error)
{
    pp_info_t pp_info = {
        .address_family = ADDR_FAMILY_UNSPEC,
        .transport_protocol = TRANSPORT_PROTOCOL_UNSPEC,
        .pp2_info.local = 1
    };
    return pp2_create_hdr(&pp_info, pp2_hdr_len, error);
}

static uint8_t *pp1_create_hdr(const pp_info_t *pp_info, uint16_t *pp1_hdr_len, int32_t *error)
{
    if (pp_info->transport_protocol != TRANSPORT_PROTOCOL_UNSPEC && pp_info->transport_protocol != TRANSPORT_PROTOCOL_STREAM)
    {
        *error = -ERR_PP1_TRANSPORT_FAMILY;
        return NULL;
    }

    char block[PP1_MAX_LENGHT];
    if (pp_info->address_family == ADDR_FAMILY_UNSPEC)
    {
        static const char str[] = "PROXY UNKNOWN"CRLF;
        *pp1_hdr_len = sizeof(str) - 1;
        memcpy(block, str, *pp1_hdr_len);
    }
    else if (pp_info->address_family == ADDR_FAMILY_INET || pp_info->address_family == ADDR_FAMILY_INET6)
    {
        const char *fam = pp_info->address_family == ADDR_FAMILY_INET ? "TCP4" : "TCP6";
        if (pp_info->address_family == ADDR_FAMILY_INET)
        {
            struct in_addr in;
            if (inet_pton(AF_INET, pp_info->src_addr, &in) != 1)
            {
                *error = -ERR_PP1_IPV4_SRC_IP;
                return NULL;
            }
            if (inet_pton(AF_INET, pp_info->dst_addr, &in) != 1)
            {
                *error = -ERR_PP1_IPV4_DST_IP;
                return NULL;
            }
        }
        else if (pp_info->address_family == ADDR_FAMILY_INET6)
        {
            struct in6_addr in6;
            if (inet_pton(AF_INET6, pp_info->src_addr, &in6) != 1)
            {
                *error = -ERR_PP1_IPV6_SRC_IP;
                return NULL;
            }
            if (inet_pton(AF_INET6, pp_info->dst_addr, &in6) != 1)
            {
                *error = -ERR_PP1_IPV6_DST_IP;
                return NULL;
            }
        }
        char src_addr[39+1];
        char dst_addr[39+1];
        memcpy(src_addr, pp_info->src_addr, sizeof(src_addr));
        memcpy(dst_addr, pp_info->dst_addr, sizeof(dst_addr));
        *pp1_hdr_len = _sprintf(block, "PROXY %s %s %s %hu %hu"CRLF, fam, src_addr, dst_addr, pp_info->src_port, pp_info->dst_port);
    }
    else
    {
        *error = -ERR_PP1_TRANSPORT_FAMILY;
        return NULL;
    }
    
    /* Create the PROXY protocol header */
    uint8_t *pp1_hdr = malloc(*pp1_hdr_len);
    if (!pp1_hdr)
    {
        *error = -ERR_HEAP_ALLOC;
        return NULL;
    }
    memcpy(pp1_hdr, block, *pp1_hdr_len);
    *error = ERR_NULL;
    return pp1_hdr;
}

uint8_t *pp_create_hdr(uint8_t version, const pp_info_t *pp_info, uint16_t *pp_hdr_len, int32_t *error)
{
    if (version == 2)
    {
        return pp2_create_hdr(pp_info, pp_hdr_len, error);
    }
    else if (version == 1)
    {
        return pp1_create_hdr(pp_info, pp_hdr_len, error);
    }
    else
    {
        *error = -ERR_PP_VERSION;
        return NULL;
    }
}

/* Verifies and parses a version 2 PROXY protocol header */
static int32_t pp2_parse_hdr(uint8_t *buffer, uint32_t buffer_length, pp_info_t *pp_info)
{
    const uint8_t *pp2_hdr = buffer;
    const proxy_hdr_v2_t *proxy_hdr_v2 = (proxy_hdr_v2_t*) buffer;

    /* The next byte (the 13th one) is the protocol version and command */
    /* The highest four bits contains the version. Only \x2 is accepted */
    uint8_t version = proxy_hdr_v2->ver_cmd >> 4;
    if (version != 0x2)
    {
        return -ERR_PP2_VERSION;
    }
    /* The lowest four bits represents the command
     * \x0 : LOCAL
     * \x1 : PROXY
     */
    uint8_t cmd = proxy_hdr_v2->ver_cmd & 0x0f;
    if (cmd == 0x0)
    {
        pp_info->pp2_info.local = 1;
    }
    else if (cmd == 0x1)
    {
        pp_info->pp2_info.local = 0;
    }
    else
    {
        return -ERR_PP2_CMD;
    }

    /* The 14th byte contains the transport protocol and address family */
    /* The highest 4 bits contain the address family */
    uint8_t fam;
    pp_info->address_family = proxy_hdr_v2->fam >> 4;
    if (pp_info->address_family == ADDR_FAMILY_UNSPEC)
    {
        fam = AF_UNSPEC;
    }
    else if (pp_info->address_family == ADDR_FAMILY_INET)
    {
        fam = AF_INET;
    }
    else if (pp_info->address_family == ADDR_FAMILY_INET6)
    {
        fam = AF_INET6;
    }
    else if (pp_info->address_family == ADDR_FAMILY_UNIX)
    {
        fam = AF_UNIX;
    }
    else
    {
        return -ERR_PP2_ADDR_FAMILY;
    }
    /* the lowest 4 bits contain the protocol */
    pp_info->transport_protocol = proxy_hdr_v2->fam & 0x0f;
    if (pp_info->transport_protocol > TRANSPORT_PROTOCOL_DGRAM)
    {
        return -ERR_PP2_TRANSPORT_PROTOCOL;
    }


    /* The 15th and 16th bytes is the address length in bytes in network byte order */
    uint16_t len = ntohs(proxy_hdr_v2->len);
    if (buffer_length < sizeof(proxy_hdr_v2_t) + len)
    {
        return -ERR_PP2_LENGTH;
    }

    /*
     * Starting from the 17th byte, addresses are presented in network byte order
     * The address order is always the same :
     * - source layer 3 address in network byte order
     * - destination layer 3 address in network byte order
     * - source layer 4 address if any, in network byte order (port)
     * - destination layer 4 address if any, in network byte order (port)
     */
    buffer += sizeof(proxy_hdr_v2_t);
    proxy_addr_t *addr = (proxy_addr_t*) buffer;
    uint16_t tlv_vectors_len = 0;
    if (fam == AF_UNSPEC)
    {
        tlv_vectors_len = len;
    }
    else if (fam == AF_INET && len >= sizeof(addr->ipv4_addr))
    {
        if (!inet_ntop(fam, &addr->ipv4_addr.src_addr, pp_info->src_addr, sizeof(pp_info->src_addr)))
        {
            return -ERR_PP2_IPV4_SRC_IP;
        }
        if (!inet_ntop(fam, &addr->ipv4_addr.dst_addr, pp_info->dst_addr, sizeof(pp_info->dst_addr)))
        {
            return -ERR_PP2_IPV4_DST_IP;
        }

        pp_info->src_port = ntohs(addr->ipv4_addr.src_port);
        pp_info->dst_port = ntohs(addr->ipv4_addr.dst_port);

        buffer += sizeof(addr->ipv4_addr);
        tlv_vectors_len = len - sizeof(addr->ipv4_addr);
    }
    else if (fam == AF_INET6 && len >= sizeof(addr->ipv6_addr))
    {
        if (!inet_ntop(fam, &addr->ipv6_addr.src_addr, pp_info->src_addr, sizeof(pp_info->src_addr)))
        {
            return -ERR_PP2_IPV6_SRC_IP;
        }
        if (!inet_ntop(fam, &addr->ipv6_addr.dst_addr, pp_info->dst_addr, sizeof(pp_info->dst_addr)))
        {
            return -ERR_PP2_IPV6_DST_IP;
        }

        pp_info->src_port = ntohs(addr->ipv6_addr.src_port);
        pp_info->dst_port = ntohs(addr->ipv6_addr.dst_port);

        buffer += sizeof(addr->ipv6_addr);
        tlv_vectors_len = len - sizeof(addr->ipv6_addr);
    }
    else if (fam == AF_UNIX && len >= sizeof(addr->unix_addr))
    {
        memcpy(pp_info->src_addr, addr->unix_addr.src_addr, sizeof(addr->unix_addr.src_addr));
        memcpy(pp_info->dst_addr, addr->unix_addr.dst_addr, sizeof(addr->unix_addr.dst_addr));
    }
    else
    {
        return -ERR_PP2_LENGTH;
    }

    /* TLVs */
    /* Any TLV vector must be at least 3 bytes */
    while (tlv_vectors_len > sizeof_pp2_tlv_t)
    {
        pp2_tlv_t *pp2_tlv = (pp2_tlv_t*) buffer;
        uint16_t pp2_tlv_len = pp2_tlv->length_hi << 8 | pp2_tlv->length_lo;
        uint16_t pp2_tlv_offset = sizeof_pp2_tlv_t + pp2_tlv_len;
        if (pp2_tlv_offset > tlv_vectors_len)
        {
            return -ERR_PP2_TLV_LENGTH;
        }

        switch (pp2_tlv->type)
        {
        case PP2_TYPE_ALPN:      /* Byte sequence */
        case PP2_TYPE_AUTHORITY: /* UTF8 */
            if (!tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, pp2_tlv->type, pp2_tlv_len, pp2_tlv->value))
            {
                return -ERR_HEAP_ALLOC;
            }
            break;
        case PP2_TYPE_CRC32C: /* 32-bit number */
        {
            if (pp2_tlv_len != sizeof(uint32_t))
            {
                return -ERR_PP2_TYPE_CRC32C;
            }

            /* Received CRC32c checksum */
            uint32_t crc32c_chksum;
            memcpy(&crc32c_chksum, pp2_tlv->value, pp2_tlv_len);

            /* Calculate the CRC32c checksum value of the whole PROXY header */
            memset(pp2_tlv->value, 0, pp2_tlv_len);
            uint32_t crc32c_calculated = crc32c(pp2_hdr, sizeof(proxy_hdr_v2_t) + len);

            /* Verify that the calculated CRC32c checksum is the same as the received CRC32c checksum*/
            if (memcmp(&crc32c_chksum, &crc32c_calculated, 4))
            {
                return -ERR_PP2_TYPE_CRC32C;
            }

            if (!tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, pp2_tlv->type, pp2_tlv_len, &crc32c_chksum))
            {
                return -ERR_HEAP_ALLOC;
            }
            pp_info->pp2_info.crc32c = 1;
            break;
        }
        case PP2_TYPE_NOOP:
            break;
        case PP2_TYPE_UNIQUE_ID: /* Byte sequence */
            if (pp2_tlv_len > 128)
            {
                return -ERR_PP2_TYPE_UNIQUE_ID;
            }
            if (!tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, pp2_tlv->type, pp2_tlv_len, pp2_tlv->value))
            {
                return -ERR_HEAP_ALLOC;
            }
            break;
        case PP2_TYPE_SSL:
        {
            pp2_tlv_ssl_t *pp2_tlv_ssl = (pp2_tlv_ssl_t*) pp2_tlv->value;

            /* Set the pp2_ssl_info */
            pp_info->pp2_info.pp2_ssl_info.ssl = !!(pp2_tlv_ssl->client & PP2_CLIENT_SSL);
            pp_info->pp2_info.pp2_ssl_info.cert_in_connection = !!(pp2_tlv_ssl->client & PP2_CLIENT_CERT_CONN);
            pp_info->pp2_info.pp2_ssl_info.cert_in_session = !!(pp2_tlv_ssl->client & PP2_CLIENT_CERT_SESS);
            pp_info->pp2_info.pp2_ssl_info.cert_verified = !pp2_tlv_ssl->verify;

            uint16_t pp2_tlvs_ssl_len = pp2_tlv_len - sizeof(pp2_tlv_ssl->client) - sizeof(pp2_tlv_ssl->verify);
            uint8_t tlv_ssl_version_found = 0;
            uint16_t pp2_sub_tlv_offset = 0;
            while (pp2_sub_tlv_offset < pp2_tlvs_ssl_len)
            {
                pp2_tlv_t *pp2_sub_tlv_ssl = (pp2_tlv_t*) ((uint8_t*) pp2_tlv_ssl->sub_tlv + pp2_sub_tlv_offset);
                uint16_t pp2_sub_tlv_ssl_len = pp2_sub_tlv_ssl->length_hi << 8 | pp2_sub_tlv_ssl->length_lo;
                switch (pp2_sub_tlv_ssl->type)
                {
                case PP2_SUBTYPE_SSL_VERSION: /* US-ASCII */
                    tlv_ssl_version_found = 1;
                case PP2_SUBTYPE_SSL_CIPHER:  /* US-ASCII */
                case PP2_SUBTYPE_SSL_SIG_ALG: /* US-ASCII */
                case PP2_SUBTYPE_SSL_KEY_ALG: /* US-ASCII */
                    if (!tlv_array_append_tlv_new_usascii(&pp_info->pp2_info.tlv_array, pp2_sub_tlv_ssl->type, pp2_sub_tlv_ssl_len, pp2_sub_tlv_ssl->value))
                    {
                        return -ERR_HEAP_ALLOC;
                    }
                    break;
                case PP2_SUBTYPE_SSL_CN: /* UTF8 */
                    if (!tlv_array_append_tlv_new(&pp_info->pp2_info.tlv_array, pp2_sub_tlv_ssl->type, pp2_sub_tlv_ssl_len, pp2_sub_tlv_ssl->value))
                    {
                        return -ERR_HEAP_ALLOC;
                    }
                    break;
                default:
                    return -ERR_PP2_TYPE_SSL;
                }

                pp2_sub_tlv_offset += sizeof_pp2_tlv_t + pp2_sub_tlv_ssl_len;
            }
            if (pp2_sub_tlv_offset > pp2_tlvs_ssl_len || (pp_info->pp2_info.pp2_ssl_info.ssl && !tlv_ssl_version_found))
            {
                return -ERR_PP2_TYPE_SSL;
            }
            break;
        }
        case PP2_TYPE_NETNS: /* US-ASCII */
            if (!tlv_array_append_tlv_new_usascii(&pp_info->pp2_info.tlv_array, pp2_tlv->type, pp2_tlv_len, pp2_tlv->value))
            {
                return -ERR_HEAP_ALLOC;
            }
            break;
        case PP2_TYPE_AWS:
        {
            if (pp2_tlv_len < sizeof(pp2_tlv_aws_t))
            {
                return -ERR_PP2_TYPE_AWS;
            }
            pp2_tlv_aws_t *pp2_tlv_aws = (pp2_tlv_aws_t*) pp2_tlv->value;
            /* Connection is done through Private Link/Interface VPC endpoint */
            if (pp2_tlv_aws->type == PP2_SUBTYPE_AWS_VPCE_ID) /* US-ASCII */
            {
                /* Example: \x1vpce-08d2bf15fac5001c9 */
                if (!tlv_array_append_tlv_new_usascii(&pp_info->pp2_info.tlv_array, pp2_tlv->type, pp2_tlv_len, pp2_tlv->value))
                {
                    return -ERR_HEAP_ALLOC;
                }
            }
            break;
        }
        case PP2_TYPE_AZURE:
        {
            if (pp2_tlv_len < sizeof(pp2_tlv_azure_t))
            {
                return -ERR_PP2_TYPE_AZURE;
            }
            pp2_tlv_azure_t *pp2_tlv_azure = (pp2_tlv_azure_t*) pp2_tlv->value;
            /* Connection is done through Private Link service */
            if (pp2_tlv_azure->type == PP2_SUBTYPE_AZURE_PRIVATEENDPOINT_LINKID) /* 32-bit number */
            {
                pp2_tlv_t *tlv = tlv_new(pp2_tlv->type, pp2_tlv_len, pp2_tlv->value);
                if (!tlv || !tlv_array_append_tlv(&pp_info->pp2_info.tlv_array, tlv))
                {
                    return -ERR_HEAP_ALLOC;
                }
            }
            break;
        }
        default:
            break;
        }
        buffer += pp2_tlv_offset;
        tlv_vectors_len -= pp2_tlv_offset;
    }

    return sizeof(proxy_hdr_v2_t) + len;
}

static int32_t pp1_parse_hdr(const uint8_t *buffer, uint32_t buffer_length, pp_info_t *pp_info)
{
    char block[PP1_MAX_LENGHT] = { 0 };
    char *ptr = block;
    int32_t pp1_hdr_len = 0;
    memcpy(block, buffer, buffer_length < PP1_MAX_LENGHT ? buffer_length : PP1_MAX_LENGHT);

    char *block_end = strstr(block, CRLF);
    if (!block_end)
    {
        return -ERR_PP1_CRLF;
    }
    block_end += strlen(CRLF);
    pp1_hdr_len = block_end - block;

    /* PROXY */
    if (memcmp(block, "PROXY", 5))
    {
        return -ERR_PP1_PROXY;
    }
    ptr += 5;

    /* Exactly one space */
    if (*ptr != '\x20')
    {
        return -ERR_PP1_SPACE;
    }
    ptr++;

    /* String indicating the proxied INET protocol and family */
    char *inet_family = strchr(ptr, ' ');
    if (!inet_family)
    {
        /* Unknown connection (short form) */
        if (pp1_hdr_len == 15 || !memcmp(ptr, "UNKNOWN", 7))
        {
            pp_info->address_family = ADDR_FAMILY_UNSPEC;
            pp_info->transport_protocol = TRANSPORT_PROTOCOL_UNSPEC;
            return pp1_hdr_len;
        }
        return -ERR_PP1_TRANSPORT_FAMILY;
    }
    uint8_t sa_family = AF_UNSPEC;
    if (!memcmp(ptr, "TCP4", 4))
    {
        sa_family = AF_INET;
        pp_info->address_family = ADDR_FAMILY_INET;
        pp_info->transport_protocol = TRANSPORT_PROTOCOL_STREAM;
        ptr += 4;
    }
    else if (!memcmp(ptr, "TCP6", 4))
    {
        sa_family = AF_INET6;
        pp_info->address_family = ADDR_FAMILY_INET6;
        pp_info->transport_protocol = TRANSPORT_PROTOCOL_STREAM;
        ptr += 4;
    }
    else if (!memcmp(ptr, "UNKNOWN", 7))
    {
        /* The receiver must ignore anything presented before the CRLF is found */
        pp_info->address_family = ADDR_FAMILY_UNSPEC;
        pp_info->transport_protocol = TRANSPORT_PROTOCOL_UNSPEC;
        return pp1_hdr_len;
    }
    else
    {
        return -ERR_PP1_TRANSPORT_FAMILY;;
    }

    /* Exactly one space */
    if (*ptr != '\x20')
    {
        return -ERR_PP1_SPACE;
    }
    ptr++;

    /* Source address */
    char *src_address_end = strchr(ptr, ' ');
    if (!src_address_end)
    {
        return sa_family == AF_INET ? ERR_PP1_IPV4_SRC_IP : ERR_PP1_IPV6_SRC_IP;
    }
    uint16_t src_address_length = src_address_end - ptr;
    memcpy(pp_info->src_addr, ptr, src_address_length);
    struct in6_addr src_sin_addr;
    if (inet_pton(sa_family, pp_info->src_addr, &src_sin_addr) != 1)
    {
        return sa_family == AF_INET ? ERR_PP1_IPV4_SRC_IP : ERR_PP1_IPV6_SRC_IP;
    }
    ptr += src_address_length;

    /* Exactly one space */
    if (*ptr != '\x20')
    {
        return -ERR_PP1_SPACE;
    }
    ptr++;

    /* Destination address */
    char *dst_address_end = strchr(ptr, ' ');
    if (!dst_address_end)
    {
        return sa_family == AF_INET ? ERR_PP1_IPV4_DST_IP : ERR_PP1_IPV6_DST_IP;
    }
    uint16_t dst_address_length = dst_address_end - ptr;
    memcpy(pp_info->dst_addr, ptr, dst_address_length);
    struct in6_addr dst_sin_addr;
    if (inet_pton(sa_family, pp_info->dst_addr, &dst_sin_addr) != 1)
    {
        return sa_family == AF_INET ? ERR_PP1_IPV4_DST_IP : ERR_PP1_IPV6_DST_IP;
    }
    ptr += dst_address_length;

    /* Exactly one space */
    if (*ptr != '\x20')
    {
        return -ERR_PP1_SPACE;
    }
    ptr++;

    /* TCP source port represented as a decimal integer in the range [0..65535] inclusive */
    char *src_port_end = strchr(ptr, ' ');
    if (!src_port_end)
    {
        return -ERR_PP1_SRC_PORT;
    }
    char src_port_str[6] = { 0 };
    uint16_t src_port_length = src_port_end - ptr;
    memcpy(src_port_str, ptr, src_port_length);
    if (!parse_port(src_port_str, &pp_info->src_port))
    {
        return -ERR_PP1_SRC_PORT;
    }
    ptr += src_port_length;

    /* Exactly one space */
    if (*ptr != '\x20')
    {
        return -ERR_PP1_SPACE;
    }
    ptr++;

    /* TCP destination port represented as a decimal integer in the range [0..65535] inclusive */
    char *dst_port_end = strchr(ptr, '\r');
    if (!dst_port_end)
    {
        return -ERR_PP1_DST_PORT;
    }
    char dst_port_str[6] = { 0 };
    uint16_t dst_port_length = dst_port_end - ptr;
    memcpy(dst_port_str, ptr, dst_port_length);
    if (!parse_port(dst_port_str, &pp_info->dst_port))
    {
        return -ERR_PP1_DST_PORT;
    }
    ptr += dst_port_length;

    /* The CRLF sequence */
    if (*ptr != '\r' || *(ptr+1) != '\n')
    {
        return -ERR_PP1_CRLF;
    }

    return pp1_hdr_len;
}

int32_t pp_parse_hdr(uint8_t *buffer, uint32_t buffer_length, pp_info_t *pp_info)
{
    memset(pp_info, 0, sizeof(*pp_info));
    if (buffer_length >= 16 && !memcmp(buffer, PP2_SIG, 12))
    {
        return pp2_parse_hdr(buffer, buffer_length, pp_info);
    }
    else if (buffer_length >= 8 && !memcmp(buffer, PP1_SIG, 5))
    {
        return pp1_parse_hdr(buffer, buffer_length, pp_info);;
    }
    else
    {
        return 0;
    }
}
