#pragma once

// Based on the official picow_freertos_http_client_sys example
// (pico-examples/pico_w/wifi/freertos/http_client), merged from its
// lwipopts.h + lwipopts_examples_common.h into a single file, since this
// project doesn't share config across multiple example targets the way
// pico-examples does.
// See https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details.

// Full FreeRTOS integration (pico_cyw43_arch_lwip_sys_freertos): WiFi/lwIP work
// happens in their own FreeRTOS task rather than an IRQ. Hardcoded here rather
// than via a CMake compile definition, per the upstream example's own comment
// that this "generally would be in your lwipopts.h".
#define NO_SYS                      0

#define TCPIP_THREAD_STACKSIZE      1024
#define DEFAULT_THREAD_STACKSIZE    1024
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define TCPIP_MBOX_SIZE             8
#define LWIP_TIMEVAL_PRIVATE        0
#define LWIP_TCPIP_CORE_LOCKING_INPUT 1
#define LWIP_SO_RCVTIMEO            1

// The auto-calculated default (LWIP_NUM_SYS_TIMEOUT_INTERNAL) only counts core modules
// (TCP/ARP/DHCP/DNS) - it doesn't know about SNTP, MQTT keepalive, or TLS handshake/
// retransmit timers, all of which register their own sys_timeout() independently. With
// all of those running concurrently (Phase 4/5), the default pool was exhausted with a
// hard panic ("MEMP_SYS_TIMEOUT is empty") right as the MQTT/TLS connection added its
// own timer on top of what WiFi/DHCP/SNTP already had running.
#define MEMP_NUM_SYS_TIMEOUT        16

#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0 // incompatible with non-polling (sys/FreeRTOS) archs
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1 // needed to resolve the AWS IoT endpoint hostname
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// TLS support via lwIP's altcp layer, backed by mbedtls - required for the
// MQTT-over-TLS connection to AWS IoT.
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1

// SNTP time sync (Phase 4) - needed before any TLS handshake, since mbedtls's X.509
// validity checks use the standard time() function.
#include "time_task.h" // for time_task_sntp_set_system_time(), used just below
#define SNTP_SERVER_DNS             1 // allow a hostname (pool.ntp.org) as the server name
#define SNTP_SET_SYSTEM_TIME(sec)   time_task_sntp_set_system_time(sec)

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

// Quiet by default; flip individual ones to LWIP_DBG_ON when debugging a specific layer.
#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define API_LIB_DEBUG                LWIP_DBG_OFF
#define API_MSG_DEBUG                LWIP_DBG_OFF
#define SOCKETS_DEBUG                LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_OFF
#define INET_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define IP_REASS_DEBUG              LWIP_DBG_OFF
#define RAW_DEBUG                   LWIP_DBG_OFF
#define MEM_DEBUG                   LWIP_DBG_OFF
#define MEMP_DEBUG                  LWIP_DBG_OFF
#define SYS_DEBUG                   LWIP_DBG_OFF
#define TCP_DEBUG                   LWIP_DBG_OFF
#define TCP_INPUT_DEBUG              LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG             LWIP_DBG_OFF
#define TCP_RTO_DEBUG                LWIP_DBG_OFF
#define TCP_CWND_DEBUG               LWIP_DBG_OFF
#define TCP_WND_DEBUG                LWIP_DBG_OFF
#define TCP_FR_DEBUG                 LWIP_DBG_OFF
#define TCP_QLEN_DEBUG               LWIP_DBG_OFF
#define TCP_RST_DEBUG                LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define TCPIP_DEBUG                 LWIP_DBG_OFF
#define PPP_DEBUG                   LWIP_DBG_OFF
#define SLIP_DEBUG                  LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_OFF

// Note: bug in lwIP with LWIP_ALTCP + LWIP_DEBUG together -
// https://savannah.nongnu.org/bugs/index.php?62159 - the #ifndef NDEBUG guard
// above already keeps LWIP_DEBUG undefined in Release builds, avoiding it.
#define ALTCP_MBEDTLS_DEBUG          LWIP_DBG_ON
