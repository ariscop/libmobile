// SPDX-License-Identifier: LGPL-3.0-or-later
#include "dns.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "util.h"

// Implemented RFCs:
// RFC1035 - DOMAIN NAMES - IMPLEMENTATION AND SPECIFICATION
// RFC6895 - Domain Name System (DNS) IANA Considerations
// RFC3596 - DNS Extensions to Support IP Version 6

// Not implemented but possibly relevant for the future:
// RFC6891 - Extension Mechanisms for DNS (EDNS(0))
// RFC7873 - Domain Name System (DNS) Cookies

#define DNS_HEADER_SIZE 12
#define DNS_QD_SIZE 4
#define DNS_RR_SIZE 10

enum dns_qtype {
    DNS_QTYPE_A = 1,
    DNS_QTYPE_AAAA = 28
};

static bool dns_make_name(struct mobile_adapter_dns *state, unsigned *offset, const char *name, unsigned name_len)
{
    unsigned char *plen = state->buffer + *offset;
    unsigned char *pdat = plen + 1;
    unsigned count = 0;

    for (const char *c = name; c < name + name_len; c++) {
        if (pdat - state->buffer + 1 > MOBILE_DNS_PACKET_SIZE) return false;
        if (*c == '.') {
            *plen = count;
            count = 0;
            plen = pdat++;
        } else {
            if (count++ >= 63) return false;
            *pdat++ = *c;
        }
    }
    if (pdat - state->buffer + 1 > MOBILE_DNS_PACKET_SIZE) return false;
    *plen = count;
    *pdat++ = 0;
    *offset = pdat - state->buffer;
    return true;
}

static bool dns_name_compare(struct mobile_adapter_dns *state, unsigned *offset, const char *name, unsigned name_len)
{
    if (*offset + 1 > state->buffer_len) return false;
    if (!name_len) return false;

    const char *pname = name;
    const unsigned char *pcmp = state->buffer + *offset;

    int end = -1;

    for (;;) {
        if (!*pcmp) {
            break;
        } else if ((*pcmp & 0xC0) == 0xC0) {
            // RFC1035 Section 4.1.4. Message compression
            if (pcmp - state->buffer + 2U > state->buffer_len) return false;
            if (end < 0) end = pcmp - state->buffer + 2;

            unsigned off = (pcmp[0] & 0x3F) << 8 | pcmp[1];
            if (off + 1 > state->buffer_len) return false;
            pcmp = state->buffer + off;
        } else if ((*pcmp & 0xC0) == 0x00) {
            unsigned len = *pcmp++;
            if (pcmp - state->buffer + len + 1 > state->buffer_len) return false;
            if (pname != name && *pname++ != '.') return false;
            if (pname - name + len > name_len) return false;
            while (len--) {
                if (*pcmp++ != *pname++) return false;
            }
        } else {
            return false;
        }
    }
    if ((unsigned)(pname - name) != name_len) return false;

    if (end < 0) end = pcmp - state->buffer + 1;
    *offset = end;
    return true;
}

static int dns_name_len(struct mobile_adapter_dns *state, unsigned offset)
{
    if (offset + 1 > state->buffer_len) return -1;

    const unsigned char *pcmp = state->buffer + offset;
    for (;;) {
        if (!*pcmp) {
            break;
        } else if ((*pcmp & 0xC0) == 0xC0) {
            // RFC1035 Section 4.1.4. Message compression
            if (pcmp - state->buffer + 2U > state->buffer_len) return -1;
            return pcmp - state->buffer - offset + 2U;
        } else if ((*pcmp & 0xC0) == 0x00) {
            unsigned len = *pcmp++;
            if (pcmp - state->buffer + len + 1 > state->buffer_len) return -1;
            pcmp += len;
        } else {
            return -1;
        }
    }

    return pcmp - state->buffer - offset + 1;
}

static bool dns_make_query(struct mobile_adapter_dns *state, enum dns_qtype type, const char *name, unsigned name_len)
{
    state->id += 1;
    state->type = type;

    memcpy(state->buffer, (unsigned char []){
        (state->id >> 8) & 0xFF, (state->id >> 0) & 0xFF,
        0x01, 0x00,  // Flags: Standard query, Recursion Desired
        0, 1,  // Questions: 1
        0, 0,  // Answers: 0
        0, 0,  // Authority records: 0
        0, 0,  // Additional records: 0
    }, DNS_HEADER_SIZE);

    unsigned offset = DNS_HEADER_SIZE;
    if (!dns_make_name(state, &offset, name, name_len)) return false;
    if (offset + DNS_QD_SIZE > MOBILE_DNS_PACKET_SIZE) return false;

    unsigned char *question = state->buffer + offset;
    question[0] = (state->type >> 8) & 0xFF;
    question[1] = (state->type >> 0) & 0xFF;
    question[2] = 0;
    question[3] = 1;  // QCLASS = IN

    state->buffer_len = offset + DNS_QD_SIZE;
    return true;
}

static int dns_verify_response(struct mobile_adapter_dns *state, unsigned *offset, const char *name, unsigned name_len)
{
    if (state->buffer_len < DNS_HEADER_SIZE) return -1;
    if ((unsigned)(state->buffer[0] << 8 | state->buffer[1]) != state->id) {
        return -1;
    }

    // Make sure:
    // - We've got a response (bit 0) for a QUERY opcode (bits 1-4)
    // - It's not a truncated message (bit 6)
    // - The server supports recursion (bits 7 and 8)
    // - Nothing is assigned to the zero-area, as according to spec (bits 9-11)
    //   - These are used to know whether this is a verified answer or not,
    //     but if zero in the request the server should never return unverified
    //     responses.
    // - No error has happened (bits 12-15)
    unsigned flags = state->buffer[2] << 8 | state->buffer[3];
    if ((flags & 0xFBFF) != 0x8180) {
        return -2 - (flags & 0xF);
    }

    unsigned qdcount = state->buffer[4] << 8 | state->buffer[5];
    unsigned ancount = state->buffer[6] << 8 | state->buffer[7];
    //unsigned nscount = state->buffer[8] << 8 | state->buffer[9];
    //unsigned arcount = state->buffer[10] << 8 | state->buffer[11];

    if (qdcount != 1) return -18;
    if (ancount < 1) return -18;

    // Verify question section
    *offset = DNS_HEADER_SIZE;
    if (!dns_name_compare(state, offset, name, name_len)) return -19;
    if (*offset + DNS_QD_SIZE > state->buffer_len) return -19;

    unsigned char *qflags = state->buffer + *offset;
    if ((unsigned)(qflags[0] << 8 | qflags[1]) != state->type) return -19;
    if ((qflags[2] << 8 | qflags[3]) != 1) return -19;  // QCLASS = IN
    *offset += DNS_QD_SIZE;

    return ancount;
}

static int dns_get_answer(struct mobile_adapter_dns *state, unsigned *offset, const char *name, unsigned name_len)
{
    // Get the start of the RR info and make sure it all fits in the buffer
    int rname_len = dns_name_len(state, *offset);
    if (rname_len < 0) return -1;
    if (*offset + rname_len + DNS_RR_SIZE > state->buffer_len) return -1;

    // Check the response length fits in the packet
    unsigned char *info = state->buffer + *offset + rname_len;
    unsigned rdlength = info[8] << 8 | info[9];
    unsigned rdata = *offset + rname_len + DNS_RR_SIZE;
    if (*offset + rname_len + DNS_RR_SIZE + rdlength > state->buffer_len) {
        return -1;
    }

    // Make sure this is the kind of response we asked for
    if (!dns_name_compare(state, offset, name, name_len)) return -2;
    if ((unsigned)(info[0] << 8 | info[1]) != state->type) return -2;
    if ((info[2] << 8 | info[3]) != 1) return -2;  // QCLASS = IN
    if (state->type == DNS_QTYPE_A && rdlength != 4) return -2;
    if (state->type == DNS_QTYPE_AAAA && rdlength != 16) return -2;

    *offset += DNS_RR_SIZE + rdlength;
    return rdata;
}

void mobile_dns_init(struct mobile_adapter *adapter)
{
    struct mobile_adapter_dns *state = &adapter->dns;
    state->id = 0;
}

bool mobile_dns_query_send(struct mobile_adapter *adapter, unsigned conn, const struct mobile_addr *addr_send, const char *host, unsigned host_len)
{
    struct mobile_adapter_dns *s = &adapter->dns;
    void *_u = adapter->user;

    if (!dns_make_query(s, DNS_QTYPE_A, host, host_len)) return false;

    if (!mobile_board_sock_send(_u, conn, s->buffer, s->buffer_len,
            addr_send)) {
        return false;
    }

    mobile_board_time_latch(_u, MOBILE_TIMER_COMMAND);
    return true;
}

// Returns:
// -1 - error
// 0 - nothing received
// 1 - success
int mobile_dns_query_recv(struct mobile_adapter *adapter, unsigned conn, const struct mobile_addr *addr_send, const char *host, unsigned host_len, unsigned char *ip)
{
    struct mobile_adapter_dns *s = &adapter->dns;
    void *_u = adapter->user;

    if (mobile_board_time_check_ms(_u, MOBILE_TIMER_COMMAND, 3000)) return -1;

    struct mobile_addr addr_recv = {0};
    int recv = mobile_board_sock_recv(_u, conn, s->buffer,
        MOBILE_DNS_PACKET_SIZE, &addr_recv);
    if (recv <= 0) return recv;
    s->buffer_len = recv;

    // Verify sender, discard if incorrect
    if (!mobile_addr_compare(addr_send, &addr_recv)) return 0;

    unsigned offset;
    int ancount = dns_verify_response(s, &offset, host, host_len);
    if (ancount < 0) return -1;

    while (ancount--) {
        int anoffset = dns_get_answer(s, &offset, host, host_len);
        if (anoffset < -1) continue;
        if (anoffset == -1) return -1;
        memcpy(ip, s->buffer + anoffset, MOBILE_HOSTLEN_IPV4);
        break;
    }
    return 1;
}
