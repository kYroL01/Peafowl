/*
 * peafowl.c
 *
 * Created on: 19/09/2012
 * =========================================================================
 *  Copyright (C) 2012-2013, Daniele De Sensi (d.desensi.software@gmail.com)
 *
 *  This file is part of Peafowl.
 *
 *  Peafowl is free software: you can redistribute it and/or
 *  modify it under the terms of the Lesser GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 3 of the License, or (at your option) any later version.

 *  Peafowl is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  Lesser GNU General Public License for more details.
 *
 *  You should have received a copy of the Lesser GNU General Public
 *  License along with Peafowl.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 * =========================================================================
 */

#include <peafowl/peafowl.h>
#include <peafowl/config.h>
#include <peafowl/flow_table.h>
#include <peafowl/hash_functions.h>
#include <peafowl/inspectors/inspectors.h>
#include <peafowl/ipv4_reassembly.h>
#include <peafowl/ipv6_reassembly.h>
#include <peafowl/tcp_stream_management.h>
#include <peafowl/utils.h>

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef WITH_PROMETHEUS
#include "prometheus.h"
#endif

#define debug_print(fmt, ...)                         \
  do {                                                \
    if (DPI_DEBUG) fprintf(stderr, fmt, __VA_ARGS__); \
  } while (0)

static const pfwl_protocol_l7 const
    dpi_well_known_ports_association_tcp[DPI_MAX_UINT_16 + 1] =
        {[0 ... DPI_MAX_UINT_16] = DPI_PROTOCOL_UNKNOWN,
         [port_dns] = DPI_PROTOCOL_DNS,
         [port_http] = DPI_PROTOCOL_HTTP,
         [port_bgp] = DPI_PROTOCOL_BGP,
         [port_smtp_1] = DPI_PROTOCOL_SMTP,
         [port_smtp_2] = DPI_PROTOCOL_SMTP,
         [port_smtp_ssl] = DPI_PROTOCOL_SMTP,
         [port_pop3] = DPI_PROTOCOL_POP3,
         [port_pop3_ssl] = DPI_PROTOCOL_POP3,
         [port_imap] = DPI_PROTOCOL_IMAP,
         [port_imap_ssl] = DPI_PROTOCOL_IMAP,
         [port_ssl] = DPI_PROTOCOL_SSL,
         [port_hangout_19305] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19306] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19307] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19308] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19309] = DPI_PROTOCOL_HANGOUT,
         [port_ssh] = DPI_PROTOCOL_SSH,};

static const pfwl_protocol_l7 const
    dpi_well_known_ports_association_udp[DPI_MAX_UINT_16 + 1] =
        {[0 ... DPI_MAX_UINT_16] = DPI_PROTOCOL_UNKNOWN,
         [port_dns] = DPI_PROTOCOL_DNS,
         [port_mdns] = DPI_PROTOCOL_MDNS,
         [port_dhcp_1] = DPI_PROTOCOL_DHCP,
         [port_dhcp_2] = DPI_PROTOCOL_DHCP,
         [port_dhcpv6_1] = DPI_PROTOCOL_DHCPv6,
         [port_dhcpv6_2] = DPI_PROTOCOL_DHCPv6,
         [port_sip] = DPI_PROTOCOL_SIP,
         [port_ntp] = DPI_PROTOCOL_NTP,
         [port_hangout_19302] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19303] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19304] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19305] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19306] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19307] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19308] = DPI_PROTOCOL_HANGOUT,
         [port_hangout_19309] = DPI_PROTOCOL_HANGOUT,
         [port_dropbox] = DPI_PROTOCOL_DROPBOX,
         [port_spotify] = DPI_PROTOCOL_SPOTIFY,};

typedef struct{
  const char* name;
  dpi_inspector_callback dissector;
  pfwl_get_extracted_fields_callback get_extracted_fields;
  int extracted_fields_num;
}pfwl_protocol_descriptor_t;

static const pfwl_protocol_descriptor_t const protocols_descriptors[DPI_NUM_PROTOCOLS] =
  {
    [DPI_PROTOCOL_DHCP]     = {"DHCP"    , check_dhcp    , NULL, 0},
    [DPI_PROTOCOL_DHCPv6]   = {"DHCPv6"  , check_dhcpv6  , NULL, 0},
    [DPI_PROTOCOL_DNS]      = {"DNS"     , check_dns     , get_extracted_fields_dns, DPI_FIELDS_DNS_NUM},
    [DPI_PROTOCOL_MDNS]     = {"MDNS"    , check_mdns    , NULL, 0},
    [DPI_PROTOCOL_SIP]      = {"SIP"     , check_sip     , get_extracted_fields_sip, DPI_FIELDS_SIP_NUM},
    [DPI_PROTOCOL_RTP]      = {"RTP"     , check_rtp     , NULL, 0},
    [DPI_PROTOCOL_SSH]      = {"SSH"     , check_ssh     , NULL, 0},
    [DPI_PROTOCOL_SKYPE]    = {"Skype"   , check_skype   , NULL, 0},
    [DPI_PROTOCOL_NTP]      = {"NTP"     , check_ntp     , NULL, 0},
    [DPI_PROTOCOL_BGP]      = {"BGP"     , check_bgp     , NULL, 0},
    [DPI_PROTOCOL_HTTP]     = {"HTTP"    , check_http    , NULL, 0},
    [DPI_PROTOCOL_SMTP]     = {"SMTP"    , check_smtp    , NULL, 0},
    [DPI_PROTOCOL_POP3]     = {"POP3"    , check_pop3    , NULL, 0},
    [DPI_PROTOCOL_IMAP]     = {"IMAP"    , check_imap    , NULL, 0},
    [DPI_PROTOCOL_SSL]      = {"SSL"     , check_ssl     , NULL, 0},
    [DPI_PROTOCOL_HANGOUT]  = {"Hangout" , check_hangout , NULL, 0},
    [DPI_PROTOCOL_WHATSAPP] = {"WhatsApp", check_whatsapp, NULL, 0},
    [DPI_PROTOCOL_TELEGRAM] = {"Telegram", check_telegram, NULL, 0},
    [DPI_PROTOCOL_DROPBOX]  = {"Dropbox" , check_dropbox , NULL, 0},
    [DPI_PROTOCOL_SPOTIFY]  = {"Spotify" , check_spotify , NULL, 0},
};

static const dpi_inspector_callback const callbacks_manager[DPI_NUM_PROTOCOLS] = {
    [DPI_PROTOCOL_HTTP] = invoke_callbacks_http,
    [DPI_PROTOCOL_SSL] = invoke_callbacks_ssl,
};

typedef struct dpi_l7_skipping_infos_key {
  u_int16_t port;
  u_int8_t l4prot;
} dpi_l7_skipping_infos_key_t;

typedef struct dpi_l7_skipping_infos {
  dpi_l7_skipping_infos_key_t key;
  pfwl_protocol_l7 protocol;
  UT_hash_handle hh; /* makes this structure hashable */
} dpi_l7_skipping_infos_t;

/**
 * Initializes the state of the library. If not specified otherwise after
 * the initialization, the library will consider all the protocols active.
 * Using this API, the hash table is divided in num_table_partitions
 * partitions. These partitions can be accessed concurrently in a thread
 * safe way from different threads if and only if each thread access only
 * to its partition.
 * @param size_v4 Size of the array of pointers used to build the database
 *        for v4 flows.
 * @param size_v6 Size of the array of pointers used to build the database
 *        for v6 flows.
 * @param max_active_v4_flows The maximum number of IPv4 flows which can
 *        be active at any time. After reaching this threshold, new flows
 *        will not be created.
 * @param max_active_v6_flows The maximum number of IPv6 flows which can
 *        be active at any time. After reaching this threshold, new flows
 *        will not be created.
 * @param num_table_partitions The number of partitions of the hash table.
 * @return A pointer to the state of the library otherwise.
 */
dpi_library_state_t* dpi_init_stateful_num_partitions(
    uint32_t size_v4, uint32_t size_v6, uint32_t max_active_v4_flows,
    uint32_t max_active_v6_flows, uint16_t num_table_partitions) {
  dpi_library_state_t* state =
      (dpi_library_state_t*)malloc(sizeof(dpi_library_state_t));

  assert(state);

  bzero(state, sizeof(dpi_library_state_t));

#if DPI_FLOW_TABLE_USE_MEMORY_POOL
  state->db4 = dpi_flow_table_create_v4(
      size_v4, max_active_v4_flows, num_table_partitions,
      DPI_FLOW_TABLE_MEMORY_POOL_DEFAULT_SIZE_v4);
  state->db6 = dpi_flow_table_create_v6(
      size_v6, max_active_v6_flows, num_table_partitions,
      DPI_FLOW_TABLE_MEMORY_POOL_DEFAULT_SIZE_v6);
#else
  state->db4 = dpi_flow_table_create_v4(size_v4, max_active_v4_flows,
                                        num_table_partitions);
  state->db6 = dpi_flow_table_create_v6(size_v6, max_active_v6_flows,
                                        num_table_partitions);
#endif
  dpi_set_max_trials(state, DPI_DEFAULT_MAX_TRIALS_PER_FLOW);
  dpi_inspect_all(state);

  dpi_ipv4_fragmentation_enable(state,
                                DPI_IPv4_FRAGMENTATION_DEFAULT_TABLE_SIZE);
  dpi_ipv6_fragmentation_enable(state,
                                DPI_IPv6_FRAGMENTATION_DEFAULT_TABLE_SIZE);

  dpi_tcp_reordering_enable(state);

  state->l7_skip = NULL;

  for(size_t i = 0; i < DPI_NUM_PROTOCOLS; i++){
    size_t num_callbacks = protocols_descriptors[i].extracted_fields_num;
    state->fields_extraction[i].fields = (uint8_t*) malloc(sizeof(uint8_t)*num_callbacks);
    state->fields_extraction[i].fields_num = 0;
    for(size_t j = 0; j < num_callbacks; j++){
      state->fields_extraction[i].fields[j] = 0;
    }
  }

  return state;
}

dpi_library_state_t* dpi_init_stateful(uint32_t size_v4, uint32_t size_v6,
                                       uint32_t max_active_v4_flows,
                                       uint32_t max_active_v6_flows) {
  return dpi_init_stateful_num_partitions(size_v4, size_v6, max_active_v4_flows,
                                          max_active_v6_flows, 1);
}

/**
 * Initializes the state of the library. If not specified otherwise after
 * the initialization, the library will consider all the protocols active.
 * @return A pointer to the state of the library otherwise.
 */
dpi_library_state_t* dpi_init_stateless(void) {
  return dpi_init_stateful(0, 0, 0, 0);
}

/**
 * Sets the maximum number of times that the library tries to guess the
 * protocol. During the flow protocol identification, after this number
 * of trials, in the case in which it cannot decide between two or more
 * protocols, one of them will be chosen, otherwise DPI_PROTOCOL_UNKNOWN
 * will be returned.
 * @param state A pointer to the state of the library.
 * @param max_trials Maximum number of trials. Zero will be consider as
 *                   infinity.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded, DPI_STATE_UPDATE_FAILURE
 *         otherwise.
 */
uint8_t dpi_set_max_trials(dpi_library_state_t* state, uint16_t max_trials) {
  state->max_trials = max_trials;
  return DPI_STATE_UPDATE_SUCCESS;
}

/**
 * Enable IPv4 defragmentation.
 * @param state        A pointer to the library state.
 * @param table_size   The size of the table to be used to store IPv4
 *                     fragments informations.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded, DPI_STATE_UPDATE_FAILURE
 *         otherwise.
 */
uint8_t dpi_ipv4_fragmentation_enable(dpi_library_state_t* state,
                                      uint16_t table_size) {
  if (likely(state)) {
    state->ipv4_frag_state =
        dpi_reordering_enable_ipv4_fragmentation(table_size);
    if (state->ipv4_frag_state)
      return DPI_STATE_UPDATE_SUCCESS;
    else
      return DPI_STATE_UPDATE_FAILURE;
  } else
    return DPI_STATE_UPDATE_FAILURE;
}

/**
 * Enable IPv6 defragmentation.
 * @param state        A pointer to the library state.
 * @param table_size   The size of the table to be used to store IPv6
 *                     fragments informations.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded, DPI_STATE_UPDATE_FAILURE
 *         otherwise.
 */
uint8_t dpi_ipv6_fragmentation_enable(dpi_library_state_t* state,
                                      uint16_t table_size) {
  if (likely(state)) {
    state->ipv6_frag_state =
        dpi_reordering_enable_ipv6_fragmentation(table_size);
    if (state->ipv6_frag_state)
      return DPI_STATE_UPDATE_SUCCESS;
    else
      return DPI_STATE_UPDATE_FAILURE;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the amount of memory that a single host can use for IPv4
 * defragmentation.
 * @param state                   A pointer to the library state.
 * @param per_host_memory_limit   The maximum amount of memory that
 *                                any IPv4 host can use.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv4_fragmentation_set_per_host_memory_limit(
    dpi_library_state_t* state, uint32_t per_host_memory_limit) {
  if (likely(state && state->ipv4_frag_state)) {
    dpi_reordering_ipv4_fragmentation_set_per_host_memory_limit(
        state->ipv4_frag_state, per_host_memory_limit);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the amount of memory that a single host can use for IPv6
 * defragmentation.
 * @param state                   A pointer to the library state.
 * @param per_host_memory_limit   The maximum amount of memory that
 *                                 any IPv6 host can use.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv6_fragmentation_set_per_host_memory_limit(
    dpi_library_state_t* state, uint32_t per_host_memory_limit) {
  if (likely(state && state->ipv6_frag_state)) {
    dpi_reordering_ipv6_fragmentation_set_per_host_memory_limit(
        state->ipv6_frag_state, per_host_memory_limit);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the total amount of memory that can be used for IPv4
 * defragmentation.
 * If fragmentation is disabled and then enabled, this information must be
 * passed again.
 * Otherwise default value will be used.
 * @param state               A pointer to the state of the library
 * @param totel_memory_limit  The maximum amount of memory that can be used
 *                            for IPv4 defragmentation.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv4_fragmentation_set_total_memory_limit(
    dpi_library_state_t* state, uint32_t total_memory_limit) {
  if (likely(state && state->ipv4_frag_state)) {
    dpi_reordering_ipv4_fragmentation_set_total_memory_limit(
        state->ipv4_frag_state, total_memory_limit);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the total amount of memory that can be used for IPv6
 * defragmentation. If fragmentation is disabled and then enabled, this
 * information must be passed again. Otherwise default value will be used.
 * @param state               A pointer to the state of the library
 * @param total_memory_limit  The maximum amount of memory that can be
 *                            used for IPv6 defragmentation.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv6_fragmentation_set_total_memory_limit(
    dpi_library_state_t* state, uint32_t total_memory_limit) {
  if (likely(state && state->ipv6_frag_state)) {
    dpi_reordering_ipv6_fragmentation_set_total_memory_limit(
        state->ipv6_frag_state, total_memory_limit);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the maximum time (in seconds) that can be spent to reassembly an
 * IPv4 fragmented datagram. Is the maximum time gap between the first and
 * last fragments of the datagram.
 * @param state            A pointer to the state of the library.
 * @param timeout_seconds  The reassembly timeout.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv4_fragmentation_set_reassembly_timeout(
    dpi_library_state_t* state, uint8_t timeout_seconds) {
  if (likely(state && state->ipv4_frag_state)) {
    dpi_reordering_ipv4_fragmentation_set_reassembly_timeout(
        state->ipv4_frag_state, timeout_seconds);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Sets the maximum time (in seconds) that can be spent to reassembly an
 * IPv6 fragmented datagram. Is the maximum time gap between the first and
 * last fragments of the datagram.
 * @param state            A pointer to the state of the library.
 * @param timeout_seconds  The reassembly timeout.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv6_fragmentation_set_reassembly_timeout(
    dpi_library_state_t* state, uint8_t timeout_seconds) {
  if (likely(state && state->ipv6_frag_state)) {
    dpi_reordering_ipv6_fragmentation_set_reassembly_timeout(
        state->ipv6_frag_state, timeout_seconds);
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Disable IPv4 defragmentation.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv4_fragmentation_disable(dpi_library_state_t* state) {
  if (likely(state && state->ipv4_frag_state)) {
    dpi_reordering_disable_ipv4_fragmentation(state->ipv4_frag_state);
    state->ipv4_frag_state = NULL;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Disable IPv6 defragmentation.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_ipv6_fragmentation_disable(dpi_library_state_t* state) {
  if (likely(state && state->ipv6_frag_state)) {
    dpi_reordering_disable_ipv6_fragmentation(state->ipv6_frag_state);
    state->ipv6_frag_state = NULL;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * If enabled, the library will reorder out of order TCP packets
 * (enabled by default).
 * @param state  A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_tcp_reordering_enable(dpi_library_state_t* state) {
  if (likely(state)) {
    state->tcp_reordering_enabled = 1;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * If it is called, the library will not reorder out of order TCP packets.
 * Out-of-order segments will be delivered to the inspector as they
 * arrive. This means that the inspector may not be able to identify the
 * application protocol. Moreover, if there are callbacks saved for TCP
 * based protocols, if TCP reordering is disabled, the extracted
 * informations could be erroneous or incomplete.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_tcp_reordering_disable(dpi_library_state_t* state) {
  if (likely(state)) {
    state->tcp_reordering_enabled = 0;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

uint8_t dpi_enable_protocol(dpi_library_state_t* state,
                            pfwl_protocol_l7 protocol) {
  if (protocol < DPI_NUM_PROTOCOLS) {
    BITSET(state->protocols_to_inspect, protocol);
    ++state->active_protocols;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

uint8_t dpi_disable_protocol(dpi_library_state_t* state,
                             pfwl_protocol_l7 protocol) {
  if (protocol < DPI_NUM_PROTOCOLS) {
    BITCLEAR(state->protocols_to_inspect, protocol);
    BITCLEAR(state->active_callbacks, protocol);
    --state->active_protocols;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_SUCCESS;
  }
}

/**
 * Enable all the protocol inspector.
 * @param state      A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_inspect_all(dpi_library_state_t* state) {
  unsigned char nonzero = ~0;
  memset(state->protocols_to_inspect, nonzero, BITNSLOTS(DPI_NUM_PROTOCOLS));
  state->active_protocols = DPI_NUM_PROTOCOLS;
  return DPI_STATE_UPDATE_SUCCESS;
}

/**
 * Disable all the protocol inspector.
 * @param state      A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_inspect_nothing(dpi_library_state_t* state) {
  bzero(state->protocols_to_inspect, BITNSLOTS(DPI_NUM_PROTOCOLS));

  state->active_protocols = 0;

  bzero(state->active_callbacks, DPI_NUM_PROTOCOLS);
  return DPI_STATE_UPDATE_SUCCESS;
}

uint8_t dpi_skip_L7_parsing_by_port(dpi_library_state_t* state, uint8_t l4prot,
                                    uint16_t port, pfwl_protocol_l7 id) {
  dpi_l7_skipping_infos_t* skinfos = malloc(sizeof(dpi_l7_skipping_infos_t));
  memset(skinfos, 0, sizeof(dpi_l7_skipping_infos_t));
  skinfos->key.l4prot = l4prot;
  skinfos->key.port = port;
  skinfos->protocol = id;
  HASH_ADD(hh, state->l7_skip, key, sizeof(skinfos->key), skinfos);
  return DPI_STATE_UPDATE_SUCCESS;
}

/**
 * Terminates the library.
 * @param state A pointer to the state of the library.
 */
void dpi_terminate(dpi_library_state_t* state) {
  if (likely(state)) {
    dpi_http_disable_callbacks(state);
    dpi_ipv4_fragmentation_disable(state);
    dpi_ipv6_fragmentation_disable(state);
    dpi_tcp_reordering_disable(state);

    dpi_flow_table_delete_v4(state->db4, state->flow_cleaner_callback);
    dpi_flow_table_delete_v6(state->db6, state->flow_cleaner_callback);
#ifdef WITH_PROMETHEUS
    dpi_prometheus_terminate(state);
#endif
    for(size_t i = 0; i < DPI_NUM_PROTOCOLS; i++){
      free(state->fields_extraction[i].fields);
    }
    free(state);
  }
}

/*
 * Try to detect the application protocol.
 * @param   state The state of the library.
 * @param   pkt The pointer to the beginning of IP header.
 * @param   data_length Length of the packet (from the beginning of the IP
 *          header, without L2 headers/trailers).
 * @param   current_time The current time in seconds.
 * @return  The status of the operation.  It gives additional informations
 *          about the processing of the request. If lesser than 0, an error
 *          occurred. dpi_get_error_msg() can be used to get a textual
 *          representation of the error. If greater or equal than 0 then
 *          it should not be interpreted as an error but simply gives
 *          additional informations (e.g. if the packet was IP fragmented,
 *          if it was out of order in the TCP stream, if is a segment of a
 *          larger application request, etc..). dpi_get_status_msg() can
 *          be used to get a textual representation of the status. Status
 *          and error codes are defined above in this header file. If an
 *          error occurred, the other returned fields are not meaningful.
 *
 *          The application protocol identifier plus the transport
 *          protocol identifier. The application protocol identifier is
 *          relative to the specific transport protocol.
 *
 * 			The flow specific user data (possibly manipulated by the
 * 			user callbacks).
 */
dpi_identification_result_t dpi_get_protocol(dpi_library_state_t* state,
                                             const unsigned char* pkt,
                                             uint32_t length,
                                             uint32_t current_time) {
  dpi_identification_result_t r;
  r.status = DPI_STATUS_OK;
  dpi_pkt_infos_t infos;
  memset(&infos, 0, sizeof(infos));
  uint8_t l3_status;

  r.status = dpi_parse_L3_L4_headers(state, pkt, length, &infos, current_time);
  l3_status = r.status;
  r.protocol_l4 = infos.l4prot;

  if (unlikely(r.status == DPI_STATUS_IP_FRAGMENT || r.status < 0)) {
    return r;
  }

  uint8_t skip_l7 = 0;
  uint16_t srcport = ntohs(infos.srcport);
  uint16_t dstport = ntohs(infos.dstport);
  dpi_l7_skipping_infos_t* sk = NULL;
  dpi_l7_skipping_infos_key_t key;
  memset(&key, 0, sizeof(key));
  key.l4prot = infos.l4prot;
  key.port = dstport;
  HASH_FIND(hh, state->l7_skip, &key, sizeof(dpi_l7_skipping_infos_key_t), sk);
  if (sk) {
    skip_l7 = 1;
    r.protocol_l7 = sk->protocol;
  } else {
    key.port = srcport;
    HASH_FIND(hh, state->l7_skip, &key, sizeof(dpi_l7_skipping_infos_key_t),
              sk);
    if (sk) {
      skip_l7 = 1;
      r.protocol_l7 = sk->protocol;
    }
  }

  if (!skip_l7) {
    if (infos.l4prot != IPPROTO_TCP && infos.l4prot != IPPROTO_UDP) {
      return r;
    }

    r.status = DPI_STATUS_OK;
    /**
     * We return the status of dpi_stateful_get_app_protocol call,
     * without giving informations on status returned
     * by dpi_parse_L3_L4_headers. Basically we return the status which
     * provides more informations.
     */
    r = dpi_stateful_get_app_protocol(state, &infos);
  }

  if (l3_status == DPI_STATUS_IP_LAST_FRAGMENT) {
    free((unsigned char*)infos.pkt);
  }

  return r;
}

/*
 * Extract from the packet the informations about source and destination
 * addresses, source and destination ports, L4 protocol and the offset
 * where the application data starts.
 * @param   state The state of the library.
 * @param   pkt The pointer to the beginning of IP header.
 * @param   data_length Length of the packet (from the beginning of the
 *          IP header, without L2 headers/trailers).
 * @param   pkt_infos The pointer to the packet infos. It will be filled
 *          by the library.
 * @param   current_time The current time in seconds. It must be
 *          non-decreasing between two consecutive calls.
 * @param	tid The thread identifier.
 * @return  The status of the operation. It gives additional informations
 *          about the processing of the request. If lesser than 0, an
 *          error occurred. dpi_get_error_msg() can be used to get a
 *          textual representation of the error. If greater or equal than
 *          0 then it should not be interpreted as an error but simply
 *          gives additional informations (e.g. if the packet was IP
 *          fragmented, if it was out of order in the TCP stream, if is a
 *          segment of a larger application request, etc..).
 *          dpi_get_status_msg() can be used to get a textual
 *          representation of the status. Status and error codes are
 *          defined above in this header file.
 *
 *          The status is DPI_STATUS_IP_FRAGMENT if the datagram is a
 *          fragment. In this case, if IP fragmentation support is
 *          enabled, the library copied the content of the datagram, so if
 *          the user wants, he can release the resources used to store the
 *          datagram.
 *
 *          The status is DPI_STATUS_IP_LAST_FRAGMENT if the received
 *          datagram allows the library to reconstruct a fragmented
 *          datagram. In this case, pkt_infos->pkt will contain a pointer
 *          to the recomposed datagram. This pointer will be different
 *          from p_pkt. The user should free() this pointer when it is no
 *          more needed (e.g. after calling
 *          dpi_state*_get_app_protocol(..)).
 */
int8_t mc_dpi_extract_packet_infos(dpi_library_state_t* state,
                                   const unsigned char* p_pkt,
                                   uint32_t p_length,
                                   dpi_pkt_infos_t* pkt_infos,
                                   uint32_t current_time, int tid) {
  if (unlikely(p_length == 0)) return DPI_STATUS_OK;
  uint8_t version;
#if __BYTE_ORDER == __LITTLE_ENDIAN
  version = (p_pkt[0] >> 4) & 0x0F;
#elif __BYTE_ORDER == __BIG_ENDIAN
  version = (p_pkt[0] << 4) & 0x0F;
#else
#error "Please fix <bits/endian.h>"
#endif

  unsigned char* pkt = (unsigned char*)p_pkt;
  uint32_t length = p_length;
  uint16_t offset;
  uint8_t more_fragments;

  pkt_infos->l4prot = 0;
  pkt_infos->srcport = 0;
  pkt_infos->dstport = 0;

  /** Offset starting from the beginning of p_pkt. **/
  uint32_t application_offset;
  /**
   * Offset starting from the last identified IPv4 or IPv6 header
   * (used to support tunneling).
   **/
  uint32_t relative_offset;
  uint32_t tmp;
  uint8_t next_header, stop = 0;

  int8_t to_return = DPI_STATUS_OK;

  struct ip6_hdr* ip6 = NULL;
  struct iphdr* ip4 = NULL;

  if (version == DPI_IP_VERSION_4) { /** IPv4 **/
    ip4 = (struct iphdr*)(p_pkt);
    uint16_t tot_len = ntohs(ip4->tot_len);

#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
    if (unlikely(length < (sizeof(struct iphdr)) || tot_len > length ||
                 tot_len <= ((ip4->ihl) * 4))) {
      return DPI_ERROR_L3_TRUNCATED_PACKET;
    }
#endif
    /**
     * At this point we are sure that tot_len<=length, so we set
     * length=tot_len. In some cases indeed there may be an L2 padding
     * at the end of the packet, so capture length (length) may be
     * greater than the effective datagram length.
     */
    length = tot_len;

    offset = ntohs(ip4->frag_off);
    if (unlikely((offset & DPI_IPv4_FRAGMENTATION_MF))) {
      more_fragments = 1;
    } else
      more_fragments = 0;

    /*
     * Offset is in 8-byte blocks. Multiplying by 8 correspond to a
     * right shift by 3 position, but the offset was 13 bit, so it can
     * still fit in a 16 bit integer.
     */
    offset = (offset & DPI_IPv4_FRAGMENTATION_OFFSET_MASK) * 8;

    if (likely((!more_fragments) && (offset == 0))) {
      pkt = (unsigned char*)p_pkt;
    } else if (state->ipv4_frag_state != NULL) {
      pkt = dpi_reordering_manage_ipv4_fragment(state->ipv4_frag_state, p_pkt,
                                                current_time, offset,
                                                more_fragments, tid);
      if (pkt == NULL) {
        return DPI_STATUS_IP_FRAGMENT;
      }
      to_return = DPI_STATUS_IP_LAST_FRAGMENT;
      ip4 = (struct iphdr*)(pkt);
      length = ntohs(((struct iphdr*)(pkt))->tot_len);
    } else {
      return DPI_STATUS_IP_FRAGMENT;
    }

    pkt_infos->src_addr_t.ipv4_srcaddr = ip4->saddr;
    pkt_infos->dst_addr_t.ipv4_dstaddr = ip4->daddr;

    application_offset = (ip4->ihl) * 4;
    relative_offset = application_offset;

    next_header = ip4->protocol;
  } else if (version == DPI_IP_VERSION_6) { /** IPv6 **/
    ip6 = (struct ip6_hdr*)(pkt);
    uint16_t tot_len =
        ntohs(ip6->ip6_ctlun.ip6_un1.ip6_un1_plen) + sizeof(struct ip6_hdr);
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
    if (unlikely(tot_len > length)) {
      return DPI_ERROR_L3_TRUNCATED_PACKET;
    }
#endif

    /**
     * At this point we are sure that tot_len<=length, so we set
     * length=tot_len. In some cases indeed there may be an L2 padding
     * at the end of the packet, so capture length (length) may be
     * greater than the effective datagram length.
     */
    length = tot_len;

    pkt_infos->src_addr_t.ipv6_srcaddr = ip6->ip6_src;
    pkt_infos->dst_addr_t.ipv6_dstaddr = ip6->ip6_dst;

    application_offset = sizeof(struct ip6_hdr);
    relative_offset = application_offset;
    next_header = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;
  } else {
    return DPI_ERROR_WRONG_IPVERSION;
  }

  while (!stop) {
    switch (next_header) {
      case IPPROTO_TCP: { /* TCP */
        struct tcphdr* tcp = (struct tcphdr*)(pkt + application_offset);
#ifdef DPI_ENABLE_L4_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct tcphdr) > length ||
                     application_offset + tcp->doff * 4 > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L4_TRUNCATED_PACKET;
        }
#endif
        pkt_infos->srcport = tcp->source;
        pkt_infos->dstport = tcp->dest;
        pkt_infos->l4offset = application_offset;
        application_offset += (tcp->doff * 4);
        stop = 1;
      } break;
      case IPPROTO_UDP: { /* UDP */
        struct udphdr* udp = (struct udphdr*)(pkt + application_offset);
#ifdef DPI_ENABLE_L4_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct udphdr) > length ||
                     application_offset + ntohs(udp->len) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L4_TRUNCATED_PACKET;
        }
#endif
        pkt_infos->srcport = udp->source;
        pkt_infos->dstport = udp->dest;
        pkt_infos->l4offset = application_offset;
        application_offset += 8;
        stop = 1;
      } break;
      case IPPROTO_HOPOPTS: { /* Hop by hop options */
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct ip6_hbh) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif
        if (likely(version == 6)) {
          struct ip6_hbh* hbh_hdr = (struct ip6_hbh*)(pkt + application_offset);
          tmp = (8 + hbh_hdr->ip6h_len * 8);
          application_offset += tmp;
          relative_offset += tmp;
          next_header = hbh_hdr->ip6h_nxt;
        } else {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED;
        }
      } break;
      case IPPROTO_DSTOPTS: { /* Destination options */
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct ip6_dest) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif
        if (likely(version == 6)) {
          struct ip6_dest* dst_hdr =
              (struct ip6_dest*)(pkt + application_offset);
          tmp = (8 + dst_hdr->ip6d_len * 8);
          application_offset += tmp;
          relative_offset += tmp;
          next_header = dst_hdr->ip6d_nxt;
        } else {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED;
        }
      } break;
      case IPPROTO_ROUTING: { /* Routing header */
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct ip6_rthdr) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif
        if (likely(version == 6)) {
          struct ip6_rthdr* rt_hdr =
              (struct ip6_rthdr*)(pkt + application_offset);
          tmp = (8 + rt_hdr->ip6r_len * 8);
          application_offset += tmp;
          relative_offset += tmp;
          next_header = rt_hdr->ip6r_nxt;
        } else {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED;
        }
      } break;
      case IPPROTO_FRAGMENT: { /* Fragment header */
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct ip6_frag) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif
        if (likely(version == 6)) {
          if (state->ipv6_frag_state) {
            struct ip6_frag* frg_hdr =
                (struct ip6_frag*)(pkt + application_offset);
            uint16_t offset = ((frg_hdr->ip6f_offlg & IP6F_OFF_MASK) >> 3) * 8;
            uint8_t more_fragments =
                ((frg_hdr->ip6f_offlg & IP6F_MORE_FRAG)) ? 1 : 0;
            offset = ntohs(offset);
            uint32_t fragment_size =
                ntohs(ip6->ip6_ctlun.ip6_un1.ip6_un1_plen) +
                sizeof(struct ip6_hdr) - relative_offset -
                sizeof(struct ip6_frag);

            /**
             * If this fragment has been obtained from a
             * defragmentation (e.g. tunneling), then delete
             * it after that the defragmentation support has
             * copied it.
             */
            unsigned char* to_delete = NULL;
            if (pkt != p_pkt) {
              to_delete = pkt;
            }

            /*
             * For our purposes, from the unfragmentable part
             * we need only the IPv6 header, any other
             * optional header can be discarded, for this
             * reason we copy only the IPv6 header bytes.
             */
            pkt = dpi_reordering_manage_ipv6_fragment(
                state->ipv6_frag_state, (unsigned char*)ip6,
                sizeof(struct ip6_hdr),
                ((unsigned char*)ip6) + relative_offset +
                    sizeof(struct ip6_frag),
                fragment_size, offset, more_fragments, frg_hdr->ip6f_ident,
                frg_hdr->ip6f_nxt, current_time, tid);

            if (to_delete) free(to_delete);

            if (pkt == NULL) {
              return DPI_STATUS_IP_FRAGMENT;
            }

            to_return = DPI_STATUS_IP_LAST_FRAGMENT;
            next_header = IPPROTO_IPV6;
            length = ((struct ip6_hdr*)(pkt))->ip6_ctlun.ip6_un1.ip6_un1_plen +
                     sizeof(struct ip6_hdr);
            /**
             * Force the next iteration to analyze the
             * reassembled IPv6 packet.
             **/
            application_offset = relative_offset = 0;
          } else {
            return DPI_STATUS_IP_FRAGMENT;
          }
        } else {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED;
        }
      } break;
      case IPPROTO_IPV6: /** 6in4 and 6in6 tunneling **/
        /** The real packet is now ipv6. **/
        version = 6;
        ip6 = (struct ip6_hdr*)(pkt + application_offset);
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(ntohs(ip6->ip6_ctlun.ip6_un1.ip6_un1_plen) +
                         sizeof(struct ip6_hdr) >
                     length - application_offset)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif

        pkt_infos->src_addr_t.ipv6_srcaddr = ip6->ip6_src;
        pkt_infos->dst_addr_t.ipv6_dstaddr = ip6->ip6_dst;

        application_offset += sizeof(struct ip6_hdr);
        relative_offset = sizeof(struct ip6_hdr);
        next_header = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;
        break;
      case 4: /* 4in4 and 4in6 tunneling */
        /** The real packet is now ipv4. **/
        version = 4;
        ip4 = (struct iphdr*)(pkt + application_offset);
#ifdef DPI_ENABLE_L3_TRUNCATION_PROTECTION
        if (unlikely(application_offset + sizeof(struct iphdr) > length ||
                     application_offset + ((ip4->ihl) * 4) > length ||
                     application_offset + ntohs(ip4->tot_len) > length)) {
          if (unlikely(pkt != p_pkt)) free(pkt);
          return DPI_ERROR_L3_TRUNCATED_PACKET;
        }
#endif
        pkt_infos->src_addr_t.ipv4_srcaddr = ip4->saddr;
        pkt_infos->dst_addr_t.ipv4_dstaddr = ip4->daddr;
        next_header = ip4->protocol;
        tmp = (ip4->ihl) * 4;
        application_offset += tmp;
        relative_offset = tmp;
        break;
      default:
        stop = 1;
        pkt_infos->l4offset = application_offset;
        break;
    }
  }

  pkt_infos->l4prot = next_header;
#ifdef DPI_ENABLE_L4_TRUNCATION_PROTECTION
  if (unlikely(application_offset > length)) {
    if (unlikely(pkt != p_pkt)) free(pkt);
    return DPI_ERROR_L4_TRUNCATED_PACKET;
  }
#endif
  pkt_infos->processing_time = current_time;
  pkt_infos->pkt = pkt;
  pkt_infos->l7offset = application_offset;
  pkt_infos->data_length = length - application_offset;
  pkt_infos->ip_version = version;
  return to_return;
}

int8_t dpi_parse_L3_L4_headers(dpi_library_state_t* state,
                               const unsigned char* p_pkt, uint32_t p_length,
                               dpi_pkt_infos_t* pkt_infos,
                               uint32_t current_time) {
  /**
   * We can pass any thread id, indeed in this case we don't
   * need lock synchronization.
   **/
  return mc_dpi_extract_packet_infos(state, p_pkt, p_length, pkt_infos,
                                     current_time, 0);
}

/*
 * Try to detect the application protocol. Before calling it, a check on
 * L4 protocol should be done and the function should be called only if
 * the packet is TCP or UDP.
 * @param   state The pointer to the library state.
 * @param   pkt_infos The pointer to the packet infos.
 * @return  The status of the operation. It gives additional informations
 *          about the processing of the request. If lesser than 0, an
 *          error occurred. dpi_get_error_msg() can be used to get a
 *          textual representation of the error. If greater or equal
 *          than 0 then it should not be interpreted as an error but
 *          simply gives additional informations (e.g. if the packet was
 *          IP fragmented, if it was out of order in the TCP stream, if is
 *          a segment of a larger application request, etc..).
 *          dpi_get_status_msg() can be used to get a textual
 *          representation of the status. Status and error codes are
 *          defined above in this header file.
 *
 *          The status is DPI_STATUS_IP_FRAGMENT if the datagram is a
 *          fragment. In this case, if IP fragmentation support is
 *          enabled, the library copied the content of the datagram, so if
 *          the user wants, he can release the resources used to store the
 *          datagram.
 *
 *          The status is DPI_STATUS_IP_LAST_FRAGMENT if the received
 *          datagram allows the library to reconstruct a fragmented
 *          datagram. In this case, pkt_infos->pkt will contain a pointer
 *          to the recomposed datagram. This pointer will be different
 *          from p_pkt. The user should free() this pointer when it is no
 *          more needed (e.g. after calling
 *          dpi_state*_get_app_protocol(..)).
 */
dpi_identification_result_t dpi_stateful_get_app_protocol(
    dpi_library_state_t* state, dpi_pkt_infos_t* pkt_infos) {
  dpi_identification_result_t r;
  r.status = DPI_STATUS_OK;

  dpi_flow_infos_t* flow_infos = NULL;
  ipv4_flow_t* ipv4_flow = NULL;
  ipv6_flow_t* ipv6_flow = NULL;

  if (pkt_infos->ip_version == DPI_IP_VERSION_4) {
    ipv4_flow = dpi_flow_table_find_or_create_flow_v4(state, pkt_infos);
    if (ipv4_flow) flow_infos = &(ipv4_flow->infos);
  } else {
    ipv6_flow = dpi_flow_table_find_or_create_flow_v6(state, pkt_infos);
    if (ipv6_flow) flow_infos = &(ipv6_flow->infos);
  }

  if (unlikely(flow_infos == NULL)) {
    r.status = DPI_ERROR_MAX_FLOWS;
    return r;
  }

  r = dpi_stateless_get_app_protocol(state, flow_infos, pkt_infos);

  if (r.status == DPI_STATUS_TCP_CONNECTION_TERMINATED) {
    if (ipv4_flow != NULL) {
      dpi_flow_table_delete_flow_v4(state->db4, state->flow_cleaner_callback,
                                    ipv4_flow);
    } else {
      dpi_flow_table_delete_flow_v6(state->db6, state->flow_cleaner_callback,
                                    ipv6_flow);
    }
  }
  return r;
}

/**
 * Initialize the flow informations passed as argument.
 * @param state       A pointer to the state of the library.
 * @param flow_infos  The informations that will be initialized by
 *                    the library.
 * @param l4prot      The transport protocol identifier.
 */
void dpi_init_flow_infos(dpi_library_state_t* state,
                         dpi_flow_infos_t* flow_infos, uint8_t l4prot) {
  pfwl_protocol_l7 i;

  for (i = 0; i < BITNSLOTS(DPI_NUM_PROTOCOLS); i++) {
    flow_infos->possible_matching_protocols[i] = state->protocols_to_inspect[i];
  }
  flow_infos->possible_protocols = state->active_protocols;

  flow_infos->l7prot = DPI_PROTOCOL_NOT_DETERMINED;
  flow_infos->trials = 0;
  flow_infos->tcp_reordering_enabled = state->tcp_reordering_enabled;
  flow_infos->last_rebuilt_tcp_data = NULL;
  bzero(&(flow_infos->tracking), sizeof(dpi_tracking_informations_t));
}

/*
 * Try to detect the application protocol. Before calling it, a check on
 * L4 protocol should be done and the function should be called only if
 * the packet is TCP or UDP. It should be used if the application already
 * has the concept of 'flow'. In this case the first time that the flow is
 * passed to the call, it must be initialized with
 * dpi_init_flow_infos(...).
 * @param   state The pointer to the library state.
 * @param   flow The informations about the flow. They must be kept by the
 *               user.
 * @param   pkt_infos The pointer to the packet infos.
 * @return  The status of the operation. It gives additional informations
 *          about the processing of the request. If lesser than 0, an error
 *          occurred. dpi_get_error_msg() can be used to get a textual
 *          representation of the error. If greater or equal than 0 then
 *          it should not be interpreted as an error but simply gives
 *          additional informations (e.g. if the packet was IP fragmented,
 *          if it was out of order in the TCP stream, if is a segment of
 *          a larger application request, etc..). dpi_get_status_msg()
 *          can be used to get a textual representation of the status.
 *          Status and error codes are defined above in this header file.
 *
 *          The status is DPI_STATUS_IP_FRAGMENT if the datagram is a
 *          fragment. In this case, if IP fragmentation support is
 *          enabled, the library copied the content of the datagram, so if
 *          the user wants, he can release the resources used to store the
 *          datagram.
 *
 *          The status is DPI_STATUS_IP_LAST_FRAGMENT if the received
 *          datagram allows the library to reconstruct a fragmented
 *          datagram. In this case, pkt_infos->pkt will contain a pointer
 *          to the recomposed datagram. This pointer will be different
 *          from p_pkt. The user should free() this pointer when it is no
 *          more needed (e.g. after calling
 *          dpi_state*_get_app_protocol(..)).
 */
dpi_identification_result_t dpi_stateless_get_app_protocol(
    dpi_library_state_t* state, dpi_flow_infos_t* flow,
    dpi_pkt_infos_t* pkt_infos) {
  dpi_identification_result_t r;
  r.status = DPI_STATUS_OK;
  r.protocol_l4 = pkt_infos->l4prot;
  r.user_flow_data = (flow->tracking.udata);
  pfwl_protocol_l7 i;

  uint8_t check_result = DPI_PROTOCOL_NO_MATCHES;
  const pfwl_protocol_l7* well_known_ports;
  const unsigned char* app_data = pkt_infos->pkt + pkt_infos->l7offset;
  uint32_t data_length = pkt_infos->data_length;
  dpi_tcp_reordering_reordered_segment_t seg;
  seg.status = DPI_TCP_REORDERING_STATUS_IN_ORDER;
  seg.data = NULL;
  seg.connection_terminated = 0;

  if(data_length){
    ++flow->tracking.num_packets;
  }

  if (flow->l7prot < DPI_PROTOCOL_NOT_DETERMINED) {
    r.protocol_l7 = flow->l7prot;
    if (pkt_infos->l4prot == IPPROTO_TCP) {
      if (flow->tcp_reordering_enabled) {
        seg = dpi_reordering_tcp_track_connection(pkt_infos, &(flow->tracking));

        if (seg.status == DPI_TCP_REORDERING_STATUS_OUT_OF_ORDER) {
          r.status = DPI_STATUS_TCP_OUT_OF_ORDER;
          return r;
        } else if (seg.status == DPI_TCP_REORDERING_STATUS_REBUILT) {
          app_data = seg.data;
          data_length = seg.data_length;
          if(flow->last_rebuilt_tcp_data){
            free((void*) flow->last_rebuilt_tcp_data);
          }
          flow->last_rebuilt_tcp_data = app_data;
        }
      } else {
        seg.connection_terminated = dpi_reordering_tcp_track_connection_light(
            pkt_infos, &(flow->tracking));
      }

      if ((BITTEST(state->active_callbacks, flow->l7prot))
          && data_length != 0) {
        (*(callbacks_manager[flow->l7prot]))(state, pkt_infos, app_data,
                                             data_length, &(flow->tracking));
      }
    } else if (pkt_infos->l4prot == IPPROTO_UDP &&
               BITTEST(state->active_callbacks, flow->l7prot)) {
      (*(callbacks_manager[flow->l7prot]))(state, pkt_infos, app_data,
                                           data_length, &(flow->tracking));
    }

    dpi_tracking_informations_t* t = &(flow->tracking);
    if (flow->l7prot < DPI_NUM_PROTOCOLS &&
        state->fields_extraction[flow->l7prot].fields_num) {
      pfwl_protocol_descriptor_t descr = protocols_descriptors[flow->l7prot];
      size_t fields_num = descr.extracted_fields_num;
      r.protocol_fields = (*descr.get_extracted_fields)(t);
      memset(r.protocol_fields, 0, sizeof(pfwl_field_t)*fields_num);
      (*(descr.dissector))(state, pkt_infos, app_data, data_length, t);
      r.protocol_fields_num = fields_num;
    }

    if (seg.connection_terminated) {
      r.status = DPI_STATUS_TCP_CONNECTION_TERMINATED;
    }
    return r;
  } else if (flow->l7prot == DPI_PROTOCOL_NOT_DETERMINED) {
    if (pkt_infos->l4prot == IPPROTO_TCP && state->active_protocols > 0) {
      well_known_ports = dpi_well_known_ports_association_tcp;
      if (flow->tcp_reordering_enabled) {
        seg = dpi_reordering_tcp_track_connection(pkt_infos, &(flow->tracking));

        if (seg.status == DPI_TCP_REORDERING_STATUS_OUT_OF_ORDER) {
          r.status = DPI_STATUS_TCP_OUT_OF_ORDER;
          r.protocol_l7 = DPI_PROTOCOL_UNKNOWN;
          return r;
        } else if (seg.status == DPI_TCP_REORDERING_STATUS_REBUILT) {
          app_data = seg.data;
          data_length = seg.data_length;
          if(flow->last_rebuilt_tcp_data){
            free((void*) flow->last_rebuilt_tcp_data);
          }
          flow->last_rebuilt_tcp_data = app_data;
        }
      } else {
        if (dpi_reordering_tcp_track_connection_light(pkt_infos,
                                                      &(flow->tracking)))
          r.status = DPI_STATUS_TCP_CONNECTION_TERMINATED;
      }
    } else if (pkt_infos->l4prot == IPPROTO_UDP &&
               state->active_protocols > 0) {
      well_known_ports = dpi_well_known_ports_association_udp;
    } else {
      return r;
    }

    /**
     * If we have no payload we don't do anything. We already
     * invoked the TCP reordering to update the connection state.
     */
    if (data_length == 0) {
      r.protocol_l7 = flow->l7prot;
      return r;
    }

    pfwl_protocol_l7 first_protocol_to_check;
    pfwl_protocol_l7 checked_protocols = 0;

    if ((first_protocol_to_check = well_known_ports[pkt_infos->srcport]) ==
            DPI_PROTOCOL_UNKNOWN &&
        (first_protocol_to_check = well_known_ports[pkt_infos->dstport]) ==
            DPI_PROTOCOL_UNKNOWN) {
      first_protocol_to_check = 0;
    }

    for (i = first_protocol_to_check; checked_protocols < DPI_NUM_PROTOCOLS;
         i = (i + 1) % DPI_NUM_PROTOCOLS, ++checked_protocols) {
      if (BITTEST(flow->possible_matching_protocols, i)) {
        pfwl_protocol_descriptor_t descr = protocols_descriptors[i];
        dpi_tracking_informations_t* t = &(flow->tracking);
        size_t fields_num = descr.extracted_fields_num;
        if(descr.get_extracted_fields){
          memset((*descr.get_extracted_fields)(t), 0, sizeof(pfwl_field_t)*fields_num);
        }
        check_result = (*(descr.dissector))(state, pkt_infos, app_data,
                                          data_length, t);
        if (check_result == DPI_PROTOCOL_MATCHES) {
          flow->l7prot = i;
          r.protocol_l7 = flow->l7prot;

          if (flow->l7prot < DPI_NUM_PROTOCOLS &&
              state->fields_extraction[flow->l7prot].fields_num) {
            r.protocol_fields = (*descr.get_extracted_fields)(t);
            r.protocol_fields_num = fields_num;
          }

          if (seg.connection_terminated) {
            r.status = DPI_STATUS_TCP_CONNECTION_TERMINATED;
          }
#ifdef WITH_PROMETHEUS
          flow->prometheus_counter_packets = dpi_prometheus_counter_create(
              state->prometheus_stats, "packets", pkt_infos, flow->l7prot);
          flow->prometheus_counter_bytes = dpi_prometheus_counter_create(
              state->prometheus_stats, "bytes", pkt_infos, flow->l7prot);
#endif
          return r;
        } else if (check_result == DPI_PROTOCOL_NO_MATCHES) {
          BITCLEAR(flow->possible_matching_protocols, i);
          --(flow->possible_protocols);
        }
      }
    }

    /**
     * If all the protocols don't match or if we still have
     * ambiguity after the maximum number of trials, then the
     * library was unable to identify the protocol.
     **/
    if (flow->possible_protocols == 0 ||
        (state->max_trials != 0 &&
         unlikely(++flow->trials == state->max_trials))) {
      flow->l7prot = DPI_PROTOCOL_UNKNOWN;
    }
  }

  r.protocol_l7 = flow->l7prot;

  if(flow->last_rebuilt_tcp_data){
    free((void*) flow->last_rebuilt_tcp_data);
    flow->last_rebuilt_tcp_data = NULL;
  }

  if (seg.connection_terminated) {
    r.status = DPI_STATUS_TCP_CONNECTION_TERMINATED;
  }
  return r;
}

/**
 * Try to guess the protocol looking only at source/destination ports.
 * This could be erroneous because sometimes protocols
 * run over ports which are not their well-known ports.
 * @param    pkt_infos The pointer to the packet infos.
 * @return   Returns the possible matching protocol.
 */
pfwl_protocol_l7 dpi_guess_protocol(dpi_pkt_infos_t* pkt_infos) {
  pfwl_protocol_l7 r = DPI_PROTOCOL_UNKNOWN;
  if (pkt_infos->l4prot == IPPROTO_TCP) {
    r = dpi_well_known_ports_association_tcp[pkt_infos->srcport];
    if (r == DPI_PROTOCOL_UNKNOWN)
      r = dpi_well_known_ports_association_tcp[pkt_infos->dstport];
  } else if (pkt_infos->l4prot == IPPROTO_UDP) {
    r = dpi_well_known_ports_association_udp[pkt_infos->srcport];
    if (r == DPI_PROTOCOL_UNKNOWN)
      r = dpi_well_known_ports_association_udp[pkt_infos->dstport];
  } else {
    r = DPI_PROTOCOL_UNKNOWN;
  }
  return r;
}

uint8_t dpi_set_protocol_accuracy(dpi_library_state_t *state,
                                  pfwl_protocol_l7 protocol,
                                  dpi_inspector_accuracy accuracy) {
  if (state) {
    state->inspectors_accuracy[protocol] = accuracy;
    return DPI_STATE_UPDATE_SUCCESS;
  } else {
    return DPI_STATE_UPDATE_FAILURE;
  }
}

/**
 * Get the string representing the error message associated to the
 * specified error_code.
 * @param   error_code The error code.
 * @return  The error message.
 */
const char* const dpi_get_error_msg(int8_t error_code) {
  switch (error_code) {
    case DPI_ERROR_WRONG_IPVERSION:
      return "ERROR: The packet is neither IPv4 nor IPv6.";
    case DPI_ERROR_IPSEC_NOTSUPPORTED:
      return "ERROR: The packet is encrypted using IPSEC. "
             "IPSEC is not supported.";
    case DPI_ERROR_L3_TRUNCATED_PACKET:
      return "ERROR: The L3 packet is truncated or corrupted.";
    case DPI_ERROR_L4_TRUNCATED_PACKET:
      return "ERROR: The L4 packet is truncated or corrupted.";
    case DPI_ERROR_TRANSPORT_PROTOCOL_NOTSUPPORTED:
      return "ERROR: The transport protocol is not supported.";
    case DPI_ERROR_MAX_FLOWS:
      return "ERROR: The maximum number of active flows has been"
             " reached.";
    default:
      return "ERROR: Not existing error code.";
  }
}

/**
 * Get the string representing the status message associated to the
 * specified status_code.
 * @param   status_code The status code.
 * @return  The status message.
 */
const char* const dpi_get_status_msg(int8_t status_code) {
  switch (status_code) {
    case DPI_STATUS_OK:
      return "STATUS: Everything is ok.";
    case DPI_STATUS_IP_FRAGMENT:
      return "STATUS: The received IP datagram is a fragment of a "
             " bigger datagram.";
    case DPI_STATUS_IP_LAST_FRAGMENT:
      return "STATUS: The received IP datagram is the last fragment"
             " of a bigger datagram. The original datagram has been"
             " recomposed.";
    case DPI_STATUS_TCP_OUT_OF_ORDER:
      return "STATUS: The received TCP segment is out of order in "
             " its stream. It will be buffered waiting for in order"
             " segments.";
    case DPI_STATUS_TCP_CONNECTION_TERMINATED:
      return "STATUS: The TCP connection is terminated.";
    default:
      return "STATUS: Not existing status code.";
  }
}

const char* const dpi_get_protocol_string(pfwl_protocol_l7 protocol) {
  if (protocol < DPI_NUM_PROTOCOLS) {
    return protocols_descriptors[protocol].name;
  } else {
    return "Unknown";
  }
}

pfwl_protocol_l7 dpi_get_protocol_id(const char* const string) {
  size_t i;
  for (i = 0; i < (size_t)DPI_NUM_PROTOCOLS; i++) {
    if (strcasecmp(string, protocols_descriptors[i].name) == 0) {
      return (pfwl_protocol_l7)i;
      ;
    }
  }
  return DPI_NUM_PROTOCOLS;
}

static const char* protocols_strings[DPI_NUM_PROTOCOLS];

const char** const dpi_get_protocols_strings() {
  size_t i;
  for (i = 0; i < (size_t)DPI_NUM_PROTOCOLS; i++) {
    protocols_strings[i] = protocols_descriptors[i].name;
  }
  return protocols_strings;
}

/**
 * Sets the callback that will be called when a flow expires.
 * (Valid only if stateful API is used).
 * @param state     A pointer to the state of the library.
 * @param cleaner   The callback used to clear the user state.
 *
 * @return DPI_STATE_UPDATE_SUCCESS if succeeded,
 *         DPI_STATE_UPDATE_FAILURE otherwise.
 */
uint8_t dpi_set_flow_cleaner_callback(dpi_library_state_t* state,
                                      dpi_flow_cleaner_callback* cleaner) {
  state->flow_cleaner_callback = cleaner;
  return DPI_STATE_UPDATE_SUCCESS;
}

uint8_t pfwl_protocol_field_add(dpi_library_state_t* state,
                                 pfwl_protocol_l7 protocol,
                                 int field_type){
  if(state){
    state->fields_extraction[protocol].fields[field_type] = 1;
    state->fields_extraction[protocol].fields_num++;
    dpi_set_protocol_accuracy(state, protocol, DPI_INSPECTOR_ACCURACY_HIGH);  // TODO: mmm, the problem is that we do not set back the original accuracy when doing field_remove
    return DPI_STATE_UPDATE_SUCCESS;
  }else{
    return DPI_STATE_UPDATE_FAILURE;
  }
}

uint8_t pfwl_protocol_field_remove(dpi_library_state_t* state,
                                    pfwl_protocol_l7 protocol,
                                    int field_type){
  if(state){
    state->fields_extraction[protocol].fields[field_type] = 0;
    state->fields_extraction[protocol].fields_num--;
    return DPI_STATE_UPDATE_SUCCESS;
  }else{
    return DPI_STATE_UPDATE_FAILURE;
  }
}

uint8_t pfwl_protocol_field_required(dpi_library_state_t* state,
                                      pfwl_protocol_l7 protocol,
                                      int field_type){
  if(state){
    return state->fields_extraction[protocol].fields[field_type];
  }else{
    return 0;
  }
}

/**
 * Adds a pointer to some data which will be passed as parameter to all
 * the fields callbacks.
 * @param state A pointer to the state of the library.
 * @param udata
 * @return
 */
uint8_t pfwl_callbacks_fields_set_udata(dpi_library_state_t* state,
                                        void* udata){
    if(state){
        state->callbacks_udata = udata;
        return DPI_STATE_UPDATE_SUCCESS;
    }else{
        return DPI_STATE_UPDATE_FAILURE;
    }
}
