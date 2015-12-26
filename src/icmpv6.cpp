/*
 * Copyright (c) 2014, Matias Fontanini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
 
#ifdef TINS_DEBUG
#include <cassert>
#endif
#include <cstring>
#include "icmpv6.h"
#include "ipv6.h"
#include "rawpdu.h"
#include "utils.h"
#include "constants.h"
#include "exceptions.h"
#include "memory_helpers.h"

using Tins::Memory::InputMemoryStream;
using Tins::Memory::OutputMemoryStream;

namespace Tins {

ICMPv6::ICMPv6(Types tp)
: _options_size(), reach_time(0), retrans_timer(0)
{
    std::memset(&_header, 0, sizeof(_header));
    type(tp);
}

ICMPv6::ICMPv6(const uint8_t *buffer, uint32_t total_sz) 
: _options_size(), reach_time(0), retrans_timer(0)
{
    InputMemoryStream stream(buffer, total_sz);
    stream.read(_header);
    if (has_target_addr()) {
        _target_address = stream.read<ipaddress_type>();
    }
    if (has_dest_addr()) {
        _dest_address = stream.read<ipaddress_type>();
    }
    if (type() == ROUTER_ADVERT) {
        reach_time = stream.read<uint32_t>();
        retrans_timer = stream.read<uint32_t>();
    }
    // Retrieve options
    if (has_options()) {
        parse_options(stream);
    }
    // Attempt to parse ICMP extensions
    try_parse_extensions(stream);
    if (stream) {
        inner_pdu(new RawPDU(stream.pointer(), stream.size()));
    }
}

void ICMPv6::parse_options(InputMemoryStream& stream) {
    while (stream) {
        const uint8_t opt_type = stream.read<uint8_t>();
        const uint32_t opt_size = static_cast<uint32_t>(stream.read<uint8_t>()) * 8;
        if (opt_size < sizeof(uint8_t) << 1) {
            throw malformed_packet();
        }
        // size(option) = option_size - identifier_size - length_identifier_size
        const uint32_t payload_size = opt_size - (sizeof(uint8_t) << 1);
        if (!stream.can_read(payload_size)) { 
            throw malformed_packet();
        }
        add_option(
            option(
                opt_type, 
                payload_size, 
                stream.pointer()
            )
        );
        stream.skip(payload_size);
    }
}

void ICMPv6::type(Types new_type) {
    _header.type = new_type;
}

void ICMPv6::code(uint8_t new_code) {
    _header.code = new_code;
}

void ICMPv6::checksum(uint16_t new_cksum) {
    _header.cksum = Endian::host_to_be(new_cksum);
}

void ICMPv6::identifier(uint16_t new_identifier) {
    _header.u_echo.identifier = Endian::host_to_be(new_identifier);
}

void ICMPv6::sequence(uint16_t new_sequence) {
    _header.u_echo.sequence = Endian::host_to_be(new_sequence);
}

void ICMPv6::override(small_uint<1> new_override) {
    _header.u_nd_advt.override = new_override;
}

void ICMPv6::solicited(small_uint<1> new_solicited) {
    _header.u_nd_advt.solicited = new_solicited;
}

void ICMPv6::router(small_uint<1> new_router) {
    _header.u_nd_advt.router = new_router;
}

void ICMPv6::hop_limit(uint8_t new_hop_limit) {
    _header.u_nd_ra.hop_limit = new_hop_limit;
}

void ICMPv6::router_pref(small_uint<2> new_router_pref) {
    _header.u_nd_ra.router_pref = new_router_pref;
}

void ICMPv6::home_agent(small_uint<1> new_home_agent) {
    _header.u_nd_ra.home_agent = new_home_agent;
}

void ICMPv6::other(small_uint<1> new_other) {
    _header.u_nd_ra.other = new_other;
}

void ICMPv6::managed(small_uint<1> new_managed) {
    _header.u_nd_ra.managed = new_managed;
}

void ICMPv6::router_lifetime(uint16_t new_router_lifetime) {
    _header.u_nd_ra.router_lifetime = Endian::host_to_be(new_router_lifetime);
}

void ICMPv6::reachable_time(uint32_t new_reachable_time) {
    reach_time = Endian::host_to_be(new_reachable_time);
}

void ICMPv6::retransmit_timer(uint32_t new_retrans_timer) {
    retrans_timer = Endian::host_to_be(new_retrans_timer);
}

void ICMPv6::target_addr(const ipaddress_type &new_target_addr) {
    _target_address = new_target_addr;
}

void ICMPv6::dest_addr(const ipaddress_type &new_dest_addr) {
    _dest_address = new_dest_addr;
}

uint32_t ICMPv6::header_size() const {
    uint32_t extra = 0;
    if(type() == ROUTER_ADVERT)
        extra = sizeof(uint32_t) * 2;
    return sizeof(_header) + _options_size + extra + 
        (has_target_addr() ? ipaddress_type::address_size : 0) +
        (has_dest_addr() ? ipaddress_type::address_size : 0);
}

uint32_t ICMPv6::trailer_size() const {
    uint32_t output = 0;
    if (has_extensions()) {
        output += extensions_.size();
        if (inner_pdu()) {
            // This gets how much padding we'll use. 
            // If the next pdu size is lower than 128 bytes, then padding = 128 - pdu size
            // If the next pdu size is greater than 128 bytes, 
            // then padding = pdu size padded to next 32 bit boundary - pdu size
            const uint32_t upper_bound = std::max(get_adjusted_inner_pdu_size(), 128U);
            output += upper_bound - inner_pdu()->size();
        }
    }
    return output;
}

void ICMPv6::use_length_field(bool value) {
    // We just need a non 0 value here, we'll use the right value on 
    // write_serialization
    _header.rfc4884.length = value ? 1 : 0;
}

bool ICMPv6::matches_response(const uint8_t *ptr, uint32_t total_sz) const {
    if(total_sz < sizeof(icmp6hdr))
        return false;
    const icmp6hdr *hdr_ptr = (const icmp6hdr*)ptr;
    if(type() == ECHO_REQUEST && hdr_ptr->type == ECHO_REPLY)
        return hdr_ptr->u_echo.identifier == _header.u_echo.identifier &&
                hdr_ptr->u_echo.sequence == _header.u_echo.sequence;
    return false;
}

void ICMPv6::write_serialization(uint8_t *buffer, uint32_t total_sz, const PDU *parent) {
    OutputMemoryStream stream(buffer, total_sz);

    // If extensions are allowed and we have to set the length field
    if (are_extensions_allowed()) {
        uint32_t length_value = get_adjusted_inner_pdu_size();
        // If the next pdu size is greater than 128, we are forced to set the length field
        if (length() != 0 || length_value > 128) {
            length_value = length_value ? std::max(length_value, 128U) : 0;
            // This field uses 64 bit words as the unit
            _header.rfc4884.length = length_value / sizeof(uint64_t);
        }
    }
    // Initially set checksum to 0, we'll calculate it at the end
    _header.cksum = 0;
    stream.write(_header);

    if (has_target_addr()) {
        stream.write(_target_address);
    }
    if (has_dest_addr()) {
        stream.write(_dest_address);
    }
    if (type() == ROUTER_ADVERT) {
        stream.write(reach_time);
        stream.write(retrans_timer);
    }
    for(options_type::const_iterator it = _options.begin(); it != _options.end(); ++it) {
        write_option(*it, stream);
    }

    if (has_extensions()) {
        uint8_t* extensions_ptr = stream.pointer();
        if (inner_pdu()) {
            // Get the size of the next pdu, padded to the next 32 bit boundary
            uint32_t inner_pdu_size = get_adjusted_inner_pdu_size();
            // If it's lower than 128, we need to padd enough zeroes to make it 128 bytes long
            if (inner_pdu_size < 128) {
                memset(extensions_ptr + inner_pdu_size, 0, 
                    128 - inner_pdu_size);
                inner_pdu_size = 128;
            }
            else {
                // If the packet has to be padded to 64 bits, append the amount 
                // of zeroes we need
                uint32_t diff = inner_pdu_size - inner_pdu()->size();
                memset(extensions_ptr + inner_pdu_size, 0, diff);
            }
            extensions_ptr += inner_pdu_size;
        }
        // Now serialize the exensions where they should be
        extensions_.serialize(
            extensions_ptr, 
            total_sz - (extensions_ptr - stream.pointer())
        );
    }

    const Tins::IPv6 *ipv6 = tins_cast<const Tins::IPv6*>(parent);
    if (ipv6) {
        uint32_t checksum = Utils::pseudoheader_checksum(
            ipv6->src_addr(),  
            ipv6->dst_addr(), 
            size(), 
            Constants::IP::PROTO_ICMPV6
        ) + Utils::do_checksum(buffer, buffer + total_sz);
        while (checksum >> 16) {
            checksum = (checksum & 0xffff) + (checksum >> 16);
        }
        this->checksum(~checksum);
        memcpy(buffer + 2, &_header.cksum, sizeof(uint16_t));
    }
}

// can i haz more?
bool ICMPv6::has_options() const {
    switch (type()) {
        case NEIGHBOUR_SOLICIT:
        case NEIGHBOUR_ADVERT:
        case ROUTER_SOLICIT:
        case ROUTER_ADVERT:
        case REDIRECT:
            return true;
        default:
            return false;
    }
}

void ICMPv6::add_option(const option &option) {
    internal_add_option(option);
    _options.push_back(option);
}

void ICMPv6::internal_add_option(const option &option) {
    _options_size += static_cast<uint32_t>(option.data_size() + sizeof(uint8_t) * 2);
}

bool ICMPv6::remove_option(OptionTypes type) {
    options_type::iterator iter = search_option_iterator(type);
    if (iter == _options.end()) {
        return false;
    }
    _options_size -= static_cast<uint32_t>(iter->data_size() + sizeof(uint8_t) * 2);
    _options.erase(iter);
    return true;
}

void ICMPv6::write_option(const option &opt, OutputMemoryStream& stream) {
    stream.write(opt.option());
    stream.write<uint8_t>((opt.length_field() + sizeof(uint8_t) * 2) / 8);
    stream.write(opt.data_ptr(), opt.data_size());
}

const ICMPv6::option *ICMPv6::search_option(OptionTypes type) const {
    // Search for the iterator. If we found something, return it, otherwise return nullptr.
    options_type::const_iterator iter = search_option_iterator(type);
    return (iter != _options.end()) ? &*iter : 0;
}

ICMPv6::options_type::const_iterator ICMPv6::search_option_iterator(OptionTypes type) const {
    Internals::option_type_equality_comparator<option> comparator(type);
    return find_if(_options.begin(), _options.end(), comparator);
}

ICMPv6::options_type::iterator ICMPv6::search_option_iterator(OptionTypes type) {
    Internals::option_type_equality_comparator<option> comparator(type);
    return find_if(_options.begin(), _options.end(), comparator);
}

// ********************************************************************
//                          Option setters
// ********************************************************************

void ICMPv6::source_link_layer_addr(const hwaddress_type &addr) {
    add_option(option(SOURCE_ADDRESS, addr.begin(), addr.end()));
}

void ICMPv6::target_link_layer_addr(const hwaddress_type &addr) {
    add_option(option(TARGET_ADDRESS, addr.begin(), addr.end()));
}

void ICMPv6::prefix_info(prefix_info_type info) {
    uint8_t buffer[2 + sizeof(uint32_t) * 3 + ipaddress_type::address_size];
    buffer[0] = info.prefix_len;
    buffer[1] = (info.L << 7) | (info.A << 6);
    uint32_t uint32_t_buffer = Endian::host_to_be(info.valid_lifetime);
    std::memcpy(buffer + 2, &uint32_t_buffer, sizeof(uint32_t));
    uint32_t_buffer = Endian::host_to_be(info.preferred_lifetime);
    std::memcpy(buffer + 2 + sizeof(uint32_t), &uint32_t_buffer, sizeof(uint32_t));
    uint32_t_buffer = 0;
    std::memcpy(buffer + 2 + sizeof(uint32_t) * 2, &uint32_t_buffer, sizeof(uint32_t));
    info.prefix.copy(buffer + 2 + sizeof(uint32_t) * 3);
    add_option(
        option(PREFIX_INFO, buffer, buffer + sizeof(buffer))
    );
}

void ICMPv6::redirect_header(const byte_array& data) {
    add_option(option(REDIRECT_HEADER, data.begin(), data.end()));
}

void ICMPv6::mtu(const mtu_type& value) {
    uint8_t buffer[sizeof(uint16_t) + sizeof(uint32_t)] = {0};
    const uint16_t u16_tmp = value.first; 
    const uint32_t u32_tmp = value.second; 
    buffer[0] = u16_tmp >> 8;
    buffer[1] = u16_tmp & 0xff;

    buffer[2] = u32_tmp >> 24;
    buffer[3] = u32_tmp >> 16;
    buffer[4] = u32_tmp >> 8;
    buffer[5] = u32_tmp & 0xff;
    add_option(option(MTU, sizeof(buffer), buffer));
}

void ICMPv6::shortcut_limit(const shortcut_limit_type &value) {
    uint8_t buffer[sizeof(uint16_t) + sizeof(uint32_t)] = {0};
    const uint32_t u32_tmp = value.reserved2; 
    buffer[0] = value.limit;
    buffer[1] = value.reserved1;
    buffer[2] = u32_tmp >> 24;
    buffer[3] = u32_tmp >> 16;
    buffer[4] = u32_tmp >> 8;
    buffer[5] = u32_tmp & 0xff;
    add_option(option(NBMA_SHORT_LIMIT, sizeof(buffer), buffer));
}

void ICMPv6::new_advert_interval(const new_advert_interval_type &value) {
    uint8_t buffer[sizeof(uint16_t) + sizeof(uint32_t)] = {0};
    const uint16_t u16_tmp = value.reserved;
    const uint32_t u32_tmp = value.interval;
    buffer[0] = u16_tmp >> 8;
    buffer[1] = u16_tmp & 0xff;

    buffer[2] = u32_tmp >> 24;
    buffer[3] = u32_tmp >> 16;
    buffer[4] = u32_tmp >> 8;
    buffer[5] = u32_tmp & 0xff;
    add_option(option(ADVERT_INTERVAL, sizeof(buffer), buffer));
}

void ICMPv6::new_home_agent_info(const new_ha_info_type &value) {
    if(value.size() != 3)
        throw malformed_option();
    uint8_t buffer[sizeof(uint16_t) + sizeof(uint32_t)] = {0};
    *((uint16_t*)(buffer + sizeof(uint16_t))) = Endian::host_to_be(value[0]);
    *((uint16_t*)(buffer + sizeof(uint16_t))) = Endian::host_to_be(value[1]);
    *((uint16_t*)(buffer + sizeof(uint16_t) * 2)) = Endian::host_to_be(value[2]);
    add_option(option(HOME_AGENT_INFO, sizeof(buffer), buffer));
}

void ICMPv6::source_addr_list(const addr_list_type &value) {
    add_addr_list(S_ADDRESS_LIST, value);
}

void ICMPv6::target_addr_list(const addr_list_type &value) {
    add_addr_list(T_ADDRESS_LIST, value);
}

void ICMPv6::add_addr_list(uint8_t type, const addr_list_type &value) {
    typedef addr_list_type::addresses_type::const_iterator iterator;
    
    std::vector<uint8_t> buffer;
    buffer.reserve(value.addresses.size() + 6);
    buffer.insert(buffer.end(), value.reserved, value.reserved + 6);
    for(iterator it = value.addresses.begin(); it != value.addresses.end(); ++it)
        buffer.insert(buffer.end(), it->begin(), it->end());
    add_option(option(type, buffer.begin(), buffer.end()));
}

void ICMPv6::rsa_signature(const rsa_sign_type &value) {
    uint32_t total_sz = static_cast<uint32_t>(4 + sizeof(value.key_hash) + value.signature.size());
    uint8_t padding = 8 - total_sz % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer;
    buffer.reserve(total_sz + padding);
    buffer.insert(buffer.end(), 2, 0);
    buffer.insert(buffer.end(), value.key_hash, value.key_hash + sizeof(value.key_hash));
    buffer.insert(buffer.end(), value.signature.begin(), value.signature.end());
    buffer.insert(buffer.end(), padding, 0);
    add_option(option(RSA_SIGN, buffer.begin(), buffer.end()));
}

void ICMPv6::timestamp(const timestamp_type &value) {
    std::vector<uint8_t> buffer(6 + sizeof(uint64_t));
    std::copy(value.reserved, value.reserved + 6, buffer.begin());
    uint64_t uint64_t_buffer = Endian::host_to_be(value.timestamp);
    memcpy(&buffer[6], &uint64_t_buffer, sizeof(uint64_t));
    add_option(option(TIMESTAMP, buffer.begin(), buffer.end()));
}

void ICMPv6::nonce(const nonce_type &value) {
    add_option(option(NONCE, value.begin(), value.end()));
}

void ICMPv6::ip_prefix(const ip_prefix_type &value) {
    std::vector<uint8_t> buffer;
    buffer.reserve(6 + ipaddress_type::address_size);
    buffer.push_back(value.option_code);
    buffer.push_back(value.prefix_len);
    // reserved
    buffer.insert(buffer.end(), sizeof(uint32_t), 0);
    buffer.insert(buffer.end(), value.address.begin(), value.address.end());
    add_option(option(IP_PREFIX, buffer.begin(), buffer.end()));
}

void ICMPv6::link_layer_addr(lladdr_type value) {
    value.address.insert(value.address.begin(), value.option_code);
    uint8_t padding = 8 - (2 + value.address.size()) % 8;
    if(padding == 8)
        padding = 0;
    value.address.insert(value.address.end(), padding, 0);
    add_option(option(LINK_ADDRESS, value.address.begin(), value.address.end()));
}

void ICMPv6::naack(const naack_type &value) {
    uint8_t buffer[6];
    buffer[0] = value.code;
    buffer[1] = value.status;
    std::copy(value.reserved, value.reserved + 4, buffer + 2);
    add_option(option(NAACK, buffer, buffer + sizeof(buffer)));
}

void ICMPv6::map(const map_type &value) {
    uint8_t buffer[sizeof(uint8_t) * 2 + sizeof(uint32_t) + ipaddress_type::address_size];
    buffer[0] = value.dist << 4 | value.pref;
    buffer[1] = value.r << 7;
    uint32_t uint32_t_buffer = Endian::host_to_be(value.valid_lifetime);
    std::memcpy(buffer + 2, &uint32_t_buffer, sizeof(uint32_t));
    value.address.copy(buffer + 2 + sizeof(uint32_t));
    add_option(option(MAP, buffer, buffer + sizeof(buffer)));
}

void ICMPv6::route_info(const route_info_type &value) {
    uint8_t padding = 8 - value.prefix.size() % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer(2 + sizeof(uint32_t) + value.prefix.size() + padding);
    buffer[0] = value.prefix_len;
    buffer[1] = value.pref << 3;
    uint32_t uint32_t_buffer = Endian::host_to_be(value.route_lifetime);
    std::memcpy(&buffer[2], &uint32_t_buffer, sizeof(uint32_t));
    // copy the prefix and then fill with padding
    buffer.insert(
        std::copy(value.prefix.begin(), value.prefix.end(), buffer.begin() + 2 + sizeof(uint32_t)),
        padding, 
        0
    );
    add_option(option(ROUTE_INFO, buffer.begin(), buffer.end()));
}

void ICMPv6::recursive_dns_servers(const recursive_dns_type &value) {
    std::vector<uint8_t> buffer(
        2 + sizeof(uint32_t) + value.servers.size() * ipaddress_type::address_size
    );
    
    buffer[0] = buffer[1] = 0;
    uint32_t tmp_lifetime = Endian::host_to_be(value.lifetime);
    std::memcpy(&buffer[2], &tmp_lifetime, sizeof(uint32_t));
    std::vector<uint8_t>::iterator out = buffer.begin() + 2 + sizeof(uint32_t);
    typedef recursive_dns_type::servers_type::const_iterator iterator;
    for(iterator it = value.servers.begin(); it != value.servers.end(); ++it)
        out = it->copy(out);
    add_option(option(RECURSIVE_DNS_SERV, buffer.begin(), buffer.end()));
}

void ICMPv6::handover_key_request(const handover_key_req_type &value) {
    uint8_t padding = 8 - (value.key.size() + 4) % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer(2 + value.key.size() + padding);
    buffer[0] = padding;
    buffer[1] = value.AT << 4;
    // copy the key, and fill with padding
    std::fill(
        std::copy(value.key.begin(), value.key.end(), buffer.begin() + 2),
        buffer.end(),
        0
    );
    add_option(option(HANDOVER_KEY_REQ, buffer.begin(), buffer.end()));
}

void ICMPv6::handover_key_reply(const handover_key_reply_type &value) {
    const uint32_t data_size = static_cast<uint32_t>(value.key.size() + 2 + sizeof(uint16_t));
    uint8_t padding = 8 - (data_size+2) % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer(data_size + padding);
    buffer[0] = padding;
    buffer[1] = value.AT << 4;
    uint16_t tmp_lifetime = Endian::host_to_be(value.lifetime);
    std::memcpy(&buffer[2], &tmp_lifetime, sizeof(uint16_t));
    // copy the key, and fill with padding
    std::fill(
        std::copy(value.key.begin(), value.key.end(), buffer.begin() + 2 + sizeof(uint16_t)),
        buffer.end(),
        0
    );
    add_option(option(HANDOVER_KEY_REPLY, buffer.begin(), buffer.end()));
}

void ICMPv6::handover_assist_info(const handover_assist_info_type &value) {
    const uint32_t data_size = static_cast<uint32_t>(value.hai.size() + 2);
    uint8_t padding = 8 - (data_size+2) % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer(data_size + padding);
    buffer[0] = value.option_code;
    buffer[1] = static_cast<uint8_t>(value.hai.size());
    // copy hai + padding
    buffer.insert(
        std::copy(value.hai.begin(), value.hai.end(), buffer.begin() + 2),
        padding,
        0
    );
    add_option(option(HANDOVER_ASSIST_INFO, buffer.begin(), buffer.end()));
}

void ICMPv6::mobile_node_identifier(const mobile_node_id_type &value) {
    const uint32_t data_size = static_cast<uint32_t>(value.mn.size() + 2);
    uint8_t padding = 8 - (data_size+2) % 8;
    if(padding == 8)
        padding = 0;
    std::vector<uint8_t> buffer(data_size + padding);
    buffer[0] = value.option_code;
    buffer[1] = static_cast<uint8_t>(value.mn.size());
    // copy mn + padding
    buffer.insert(
        std::copy(value.mn.begin(), value.mn.end(), buffer.begin() + 2),
        padding,
        0
    );
    add_option(option(MOBILE_NODE_ID, buffer.begin(), buffer.end()));
}

void ICMPv6::dns_search_list(const dns_search_list_type &value) {
    // at least it's got this size
    std::vector<uint8_t> buffer(2 + sizeof(uint32_t));
    uint32_t tmp_lifetime = Endian::host_to_be(value.lifetime);
    std::memcpy(&buffer[2], &tmp_lifetime, sizeof(uint32_t));
    typedef dns_search_list_type::domains_type::const_iterator iterator;
    for(iterator it = value.domains.begin(); it != value.domains.end(); ++it) {
        size_t prev = 0, index;
        do {
            index = it->find('.', prev);
            std::string::const_iterator end = (index == std::string::npos) ? it->end() : (it->begin() + index);
            buffer.push_back(static_cast<uint8_t>(end - (it->begin() + prev)));
            buffer.insert(buffer.end(), it->begin() + prev, end);
            prev = index + 1;
        } while(index != std::string::npos);
        // delimiter
        buffer.push_back(0);
    }
    uint8_t padding = 8 - (buffer.size() + 2) % 8;
    if (padding == 8) {
        padding = 0;
    }
    buffer.insert(buffer.end(), padding, 0);
    add_option(option(DNS_SEARCH_LIST, buffer.begin(), buffer.end()));
}

uint32_t ICMPv6::get_adjusted_inner_pdu_size() const {
    // This gets the size of the next pdu, padded to the next 64 bit word boundary
    return Internals::get_padded_icmp_inner_pdu_size(inner_pdu(), sizeof(uint64_t));
}

void ICMPv6::try_parse_extensions(InputMemoryStream& stream) {
    // Check if this is one of the types defined in RFC 4884
    if (are_extensions_allowed()) {
        Internals::try_parse_icmp_extensions(stream, length() * sizeof(uint64_t), 
            extensions_);
    }
}

bool ICMPv6::are_extensions_allowed() const {
    return type() == TIME_EXCEEDED;
}

// ********************************************************************
//                          Option getters
// ********************************************************************

ICMPv6::hwaddress_type ICMPv6::source_link_layer_addr() const {
    return search_and_convert<hwaddress_type>(SOURCE_ADDRESS);
}

ICMPv6::hwaddress_type ICMPv6::target_link_layer_addr() const {
    return search_and_convert<hwaddress_type>(TARGET_ADDRESS);
}

ICMPv6::prefix_info_type ICMPv6::prefix_info() const {
    return search_and_convert<prefix_info_type>(PREFIX_INFO);
}

byte_array ICMPv6::redirect_header() const {
    return search_and_convert<PDU::serialization_type>(REDIRECT_HEADER);
}

ICMPv6::mtu_type ICMPv6::mtu() const {
    return search_and_convert<mtu_type>(MTU);
}

ICMPv6::shortcut_limit_type ICMPv6::shortcut_limit() const {
    return search_and_convert<shortcut_limit_type>(NBMA_SHORT_LIMIT);
}

ICMPv6::new_advert_interval_type ICMPv6::new_advert_interval() const {
    return search_and_convert<new_advert_interval_type>(ADVERT_INTERVAL);
}

ICMPv6::new_ha_info_type ICMPv6::new_home_agent_info() const {
    return search_and_convert<new_ha_info_type>(HOME_AGENT_INFO);
}

ICMPv6::addr_list_type ICMPv6::source_addr_list() const {
    return search_addr_list(S_ADDRESS_LIST);
}

ICMPv6::addr_list_type ICMPv6::target_addr_list() const {
    return search_addr_list(T_ADDRESS_LIST);
}

ICMPv6::addr_list_type ICMPv6::search_addr_list(OptionTypes type) const {
    return search_and_convert<addr_list_type>(type);
}

ICMPv6::rsa_sign_type ICMPv6::rsa_signature() const {
    return search_and_convert<rsa_sign_type>(RSA_SIGN);
}

ICMPv6::timestamp_type ICMPv6::timestamp() const {
    return search_and_convert<timestamp_type>(TIMESTAMP);
}

ICMPv6::nonce_type ICMPv6::nonce() const {
    return search_and_convert<nonce_type>(NONCE);
}

ICMPv6::ip_prefix_type ICMPv6::ip_prefix() const {
    return search_and_convert<ip_prefix_type>(IP_PREFIX);
}   

ICMPv6::lladdr_type ICMPv6::link_layer_addr() const {
    return search_and_convert<lladdr_type>(LINK_ADDRESS);
}

ICMPv6::naack_type ICMPv6::naack() const {
    return search_and_convert<naack_type>(NAACK);
}

ICMPv6::map_type ICMPv6::map() const {
    return search_and_convert<map_type>(MAP);
}

ICMPv6::route_info_type ICMPv6::route_info() const {
    return search_and_convert<route_info_type>(ROUTE_INFO);
}

ICMPv6::recursive_dns_type ICMPv6::recursive_dns_servers() const {
    return search_and_convert<recursive_dns_type>(RECURSIVE_DNS_SERV);
}

ICMPv6::handover_key_req_type ICMPv6::handover_key_request() const {
    return search_and_convert<handover_key_req_type>(HANDOVER_KEY_REQ);
}

ICMPv6::handover_key_reply_type ICMPv6::handover_key_reply() const {
    return search_and_convert<handover_key_reply_type>(HANDOVER_KEY_REPLY);
}

ICMPv6::handover_assist_info_type ICMPv6::handover_assist_info() const {
    return search_and_convert<handover_assist_info_type>(HANDOVER_ASSIST_INFO);
}

ICMPv6::mobile_node_id_type ICMPv6::mobile_node_identifier() const {
    return search_and_convert<mobile_node_id_type>(MOBILE_NODE_ID);
}

ICMPv6::dns_search_list_type ICMPv6::dns_search_list() const {
    return search_and_convert<dns_search_list_type>(DNS_SEARCH_LIST);
}

// Options stuff

ICMPv6::addr_list_type ICMPv6::addr_list_type::from_option(const option &opt) 
{
    if(opt.data_size() < 6 + ipaddress_type::address_size || (opt.data_size() - 6) % ipaddress_type::address_size != 0)
        throw malformed_option();
    addr_list_type output;
    const uint8_t *ptr = opt.data_ptr(), *end = opt.data_ptr() + opt.data_size();
    std::copy(ptr, ptr + 6, output.reserved);
    ptr += 6;
    while(ptr < end) {
        output.addresses.push_back(ICMPv6::ipaddress_type(ptr));
        ptr += ICMPv6::ipaddress_type::address_size;
    }
    return output;
}

ICMPv6::naack_type ICMPv6::naack_type::from_option(const option &opt) 
{
    if(opt.data_size() != 6)
        throw malformed_option();
    return naack_type(*opt.data_ptr(), opt.data_ptr()[1]);
}

ICMPv6::lladdr_type ICMPv6::lladdr_type::from_option(const option &opt)
{
    if(opt.data_size() < 2)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    lladdr_type output(*ptr++);
    output.address.assign(ptr, opt.data_ptr() + opt.data_size());
    return output;
}

ICMPv6::prefix_info_type ICMPv6::prefix_info_type::from_option(const option &opt) 
{
    if(opt.data_size() != 2 + sizeof(uint32_t) * 3 + ICMPv6::ipaddress_type::address_size)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    prefix_info_type output;
    output.prefix_len = *ptr++;
    output.L = (*ptr >> 7) & 0x1;
    output.A = (*ptr++ >> 6) & 0x1;
    std::memcpy(&output.valid_lifetime, ptr, sizeof(uint32_t));
    output.valid_lifetime = Endian::be_to_host(output.valid_lifetime);
    ptr += sizeof(uint32_t);
    std::memcpy(&output.preferred_lifetime, ptr, sizeof(uint32_t));
    output.preferred_lifetime = Endian::be_to_host(output.preferred_lifetime);
    output.prefix = ptr + sizeof(uint32_t) * 2;
    return output;
}

ICMPv6::rsa_sign_type ICMPv6::rsa_sign_type::from_option(const option &opt)
{
    // 2 bytes reserved + at least 1 byte signature.
    // 16 == sizeof(rsa_sign_type::key_hash), removed the sizeof
    // expression since gcc 4.2 doesn't like it
    if(opt.data_size() < 2 + 16 + 1)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr() + 2;
    rsa_sign_type output;
    std::copy(ptr, ptr + sizeof(output.key_hash), output.key_hash);
    ptr += sizeof(output.key_hash);
    output.signature.assign(ptr, opt.data_ptr() + opt.data_size());
    return output;
}

ICMPv6::ip_prefix_type ICMPv6::ip_prefix_type::from_option(const option &opt)
{
    // 2 bytes + 4 padding + ipv6 address
    if(opt.data_size() != 2 + 4 + ICMPv6::ipaddress_type::address_size)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    ip_prefix_type output;
    output.option_code = *ptr++;
    output.prefix_len = *ptr++;
    // skip padding
    ptr += sizeof(uint32_t);
    output.address = ICMPv6::ipaddress_type(ptr);
    return output;
}

ICMPv6::map_type ICMPv6::map_type::from_option(const option &opt)
{
    if(opt.data_size() != 2 + sizeof(uint32_t) + ipaddress_type::address_size)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    map_type output;
    output.dist = (*ptr >> 4) & 0x0f;
    output.pref = *ptr++ & 0x0f;
    output.r = (*ptr++ >> 7) & 0x01;
    std::memcpy(&output.valid_lifetime, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    output.address = ptr;
    return output;
}

ICMPv6::route_info_type ICMPv6::route_info_type::from_option(const option &opt) 
{
    if(opt.data_size() < 2 + sizeof(uint32_t))
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    route_info_type output;
    output.prefix_len = *ptr++;
    output.pref = (*ptr++ >> 3) & 0x3;
    std::memcpy(&output.route_lifetime, ptr, sizeof(uint32_t));
    output.route_lifetime = Endian::be_to_host(output.route_lifetime);
    ptr += sizeof(uint32_t);
    output.prefix.assign(ptr, opt.data_ptr() + opt.data_size());
    return output;
}

ICMPv6::recursive_dns_type ICMPv6::recursive_dns_type::from_option(const option &opt)
{
    if(opt.data_size() < 2 + sizeof(uint32_t) + ICMPv6::ipaddress_type::address_size)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr() + 2, *end = opt.data_ptr() + opt.data_size();
    recursive_dns_type output;
    std::memcpy(&output.lifetime, ptr, sizeof(uint32_t));
    output.lifetime = Endian::be_to_host(output.lifetime);
    ptr += sizeof(uint32_t);
    while(ptr < end) {
        if(ptr + ICMPv6::ipaddress_type::address_size > end)
            throw option_not_found();
        output.servers.push_back(ptr);
        ptr += ICMPv6::ipaddress_type::address_size;
    }
    return output;
}

ICMPv6::handover_key_req_type ICMPv6::handover_key_req_type::from_option(const option &opt)
{
    if(opt.data_size() < 2 + sizeof(uint32_t))
        throw option_not_found();
    const uint8_t *ptr = opt.data_ptr() + 1, *end = opt.data_ptr() + opt.data_size();
    handover_key_req_type output;
    output.AT = (*ptr++ >> 4) & 0x3;
    // is there enough size for the indicated padding?
    if(end - ptr < *opt.data_ptr())
        throw malformed_option();
    output.key.assign(ptr, ptr + ((end - ptr) - *opt.data_ptr()));
    return output;
}

ICMPv6::handover_key_reply_type ICMPv6::handover_key_reply_type::from_option(const option &opt)
{
    if(opt.data_size() < 2 + sizeof(uint32_t))
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr() + 1, *end = opt.data_ptr() + opt.data_size();
    handover_key_reply_type output;
    output.AT = (*ptr++ >> 4) & 0x3;
    std::memcpy(&output.lifetime, ptr, sizeof(uint16_t));
    output.lifetime = Endian::be_to_host(output.lifetime);
    ptr += sizeof(uint16_t);
    // is there enough size for the indicated padding?
    if(end - ptr < *opt.data_ptr())
        throw malformed_option();
    output.key.assign(ptr, ptr + ((end - ptr) - *opt.data_ptr()));
    return output;
}

ICMPv6::handover_assist_info_type ICMPv6::handover_assist_info_type::from_option(const option &opt)
{
    if(opt.data_size() < 2)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr(), *end = ptr + opt.data_size();
    handover_assist_info_type output;
    output.option_code = *ptr++;
    if((end - ptr - 1) < *ptr)
        throw malformed_option();
    output.hai.assign(ptr + 1, ptr + 1 + *ptr);
    return output;
}

ICMPv6::mobile_node_id_type ICMPv6::mobile_node_id_type::from_option(const option &opt)
{
    if(opt.data_size() < 2)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr(), *end = ptr + opt.data_size();
    mobile_node_id_type output;
    output.option_code = *ptr++;
    if((end - ptr - 1) < *ptr)
        throw malformed_option();
    output.mn.assign(ptr + 1, ptr + 1 + *ptr);
    return output;
}

ICMPv6::dns_search_list_type ICMPv6::dns_search_list_type::from_option(const option &opt)
{
    if(opt.data_size() < 2 + sizeof(uint32_t))
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr(), *end = ptr + opt.data_size();
    dns_search_list_type output;
    std::memcpy(&output.lifetime, ptr + 2, sizeof(uint32_t));
    output.lifetime = Endian::be_to_host(output.lifetime);
    ptr += 2 + sizeof(uint32_t);
    while(ptr < end && *ptr) {
        std::string domain;
        while(ptr < end && *ptr && *ptr < (end - ptr)) {
            if(!domain.empty())
                domain.push_back('.');
            domain.insert(domain.end(), ptr + 1, ptr + *ptr + 1);
            ptr += *ptr + 1;
        }
        // not enough size
        if(ptr < end && *ptr != 0)
            throw option_not_found();
        output.domains.push_back(domain);
        ptr++;
    }
    return output;
}

ICMPv6::timestamp_type ICMPv6::timestamp_type::from_option(const option &opt)
{
    if(opt.data_size() != 6 + sizeof(uint64_t))
        throw malformed_option();
    uint64_t uint64_t_buffer;
    std::memcpy(&uint64_t_buffer, opt.data_ptr() + 6, sizeof(uint64_t));
    timestamp_type output(Endian::be_to_host(uint64_t_buffer));
    std::copy(opt.data_ptr(), opt.data_ptr() + 6, output.reserved);
    return output;
}

ICMPv6::shortcut_limit_type ICMPv6::shortcut_limit_type::from_option(const option &opt)
{
    if(opt.data_size() != 6)
        throw malformed_option();
    const uint8_t *ptr = opt.data_ptr();
    shortcut_limit_type output(*ptr++);
    output.reserved1 = *ptr++;
    std::memcpy(&output.reserved2, ptr, sizeof(uint32_t));
    output.reserved2 = Endian::be_to_host(output.reserved2);
    return output;
}

ICMPv6::new_advert_interval_type ICMPv6::new_advert_interval_type::from_option(const option &opt)
{
    if(opt.data_size() != 6)
        throw malformed_option();
    new_advert_interval_type output;
    std::memcpy(&output.reserved, opt.data_ptr(), sizeof(uint16_t));
    output.reserved = Endian::be_to_host(output.reserved);
    std::memcpy(&output.interval, opt.data_ptr() + sizeof(uint16_t), sizeof(uint32_t));
    output.interval = Endian::be_to_host(output.interval);
    return output;
}
}

