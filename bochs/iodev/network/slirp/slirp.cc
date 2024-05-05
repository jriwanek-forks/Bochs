/* SPDX-License-Identifier: MIT */
/*
 * libslirp glue
 *
 * Copyright (c) 2004-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define BX_PLUGGABLE

#include "slirp.h"

#if BX_NETWORKING && BX_NETMOD_SLIRP

#ifndef _WIN32
#include <net/if.h>
#endif

/* https://gitlab.freedesktop.org/slirp/libslirp/issues/18 */
#if defined(__NetBSD__) && defined(if_mtu)
#undef if_mtu
#endif

int slirp_debug;

/* host loopback address */
struct in_addr loopback_addr;
/* host loopback network mask */
unsigned long loopback_mask;

/* emulated hosts use the MAC addr 52:55:IP:IP:IP:IP */
static const uint8_t special_ethaddr[ETH_ALEN] = {
    0x52, 0x55, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t zero_ethaddr[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };

unsigned curtime;

static struct in_addr dns_addr;
static u_int dns_addr_time;

#define TIMEOUT_FAST 2  /* milliseconds */
#define TIMEOUT_SLOW 499  /* milliseconds */
/* for the aging of certain requests like DNS */
#define TIMEOUT_DEFAULT 1000  /* milliseconds */

#if defined(_WIN32) || defined(__CYGWIN__)

#include <iphlpapi.h>
#include <winerror.h>

int get_dns_addr(struct in_addr *pdns_addr)
{
    FIXED_INFO *FixedInfo=NULL;
    ULONG    BufLen;
    DWORD    ret;
    IP_ADDR_STRING *pIPAddr;
    struct in_addr tmp_addr;

    if (dns_addr.s_addr != 0 && (curtime - dns_addr_time) < TIMEOUT_DEFAULT) {
        *pdns_addr = dns_addr;
        return 0;
    }

    FixedInfo = (FIXED_INFO *)GlobalAlloc(GPTR, sizeof(FIXED_INFO));
    BufLen = sizeof(FIXED_INFO);

    if (ERROR_BUFFER_OVERFLOW == GetNetworkParams(FixedInfo, &BufLen)) {
        if (FixedInfo) {
            GlobalFree(FixedInfo);
            FixedInfo = NULL;
        }
        FixedInfo = (FIXED_INFO *)GlobalAlloc(GPTR, BufLen);
    }

    if ((ret = GetNetworkParams(FixedInfo, &BufLen)) != ERROR_SUCCESS) {
        printf("GetNetworkParams failed. ret = %08x\n", (u_int)ret );
        if (FixedInfo) {
            GlobalFree(FixedInfo);
            FixedInfo = NULL;
        }
        return -1;
    }

    pIPAddr = &(FixedInfo->DnsServerList);
    inet_aton(pIPAddr->IpAddress.String, &tmp_addr);
    *pdns_addr = tmp_addr;
    dns_addr = tmp_addr;
    dns_addr_time = curtime;
    if (FixedInfo) {
        GlobalFree(FixedInfo);
        FixedInfo = NULL;
    }
    return 0;
}

#else

static struct stat dns_addr_stat;

int get_dns_addr(struct in_addr *pdns_addr)
{
    char buff[512];
    char buff2[257];
    FILE *f;
    int found = 0;
    struct in_addr tmp_addr;

    if (dns_addr.s_addr != 0) {
        struct stat old_stat;
        if ((curtime - dns_addr_time) < TIMEOUT_DEFAULT) {
            *pdns_addr = dns_addr;
            return 0;
        }
        old_stat = dns_addr_stat;
        if (stat("/etc/resolv.conf", &dns_addr_stat) != 0)
            return -1;
        if ((dns_addr_stat.st_dev == old_stat.st_dev)
            && (dns_addr_stat.st_ino == old_stat.st_ino)
            && (dns_addr_stat.st_size == old_stat.st_size)
            && (dns_addr_stat.st_mtime == old_stat.st_mtime)) {
            *pdns_addr = dns_addr;
            return 0;
        }
    }

    f = fopen("/etc/resolv.conf", "r");
    if (!f)
        return -1;

#ifdef DEBUG
    printf("IP address of your DNS(s): ");
#endif
    while (fgets(buff, 512, f) != NULL) {
        if (sscanf(buff, "nameserver%*[ \t]%256s", buff2) == 1) {
            if (!inet_aton(buff2, &tmp_addr))
                continue;
            /* If it's the first one, set it to dns_addr */
            if (!found) {
                *pdns_addr = tmp_addr;
                dns_addr = tmp_addr;
                dns_addr_time = curtime;
            }
#ifdef DEBUG
            else
                printf(", ");
#endif
            if (++found > 3) {
#ifdef DEBUG
                printf("(more)");
#endif
                break;
            }
#ifdef DEBUG
            else
                printf("%s", inet_ntoa(tmp_addr));
#endif
        }
    }
    fclose(f);
    if (!found)
        return -1;
    return 0;
}

#endif

#ifdef _WIN32
static void CDECL winsock_cleanup(void)
{
    WSACleanup();
}
#endif

static void slirp_init_once(void)
{
    static int initialized;
#ifdef _WIN32
    WSADATA Data;
#endif

    if (initialized) {
        return;
    }
    initialized = 1;

#ifdef _WIN32
    WSAStartup(MAKEWORD(2,0), &Data);
    atexit(winsock_cleanup);
#endif

    loopback_addr.s_addr = htonl(INADDR_LOOPBACK);
    loopback_mask = htonl(IN_CLASSA_NET);
}

Slirp *slirp_new(const SlirpConfig *cfg, const SlirpCb *callbacks, void *opaque)
{
    Slirp *slirp = (Slirp*)malloc(sizeof(Slirp));
    memset(slirp, 0, sizeof(Slirp));

    slirp_init_once();

    slirp->cfg_version = cfg->version;
    slirp->opaque = opaque;
    slirp->cb = callbacks;
    slirp->restricted = cfg->restricted;

    slirp->in_enabled = cfg->in_enabled;
    slirp->in6_enabled = cfg->in6_enabled;

    if_init(slirp);
    ip_init(slirp);

    m_init(slirp);

    slirp->vnetwork_addr = cfg->vnetwork;
    slirp->vnetwork_mask = cfg->vnetmask;
    slirp->vhost_addr = cfg->vhost;
    slirp->vprefix_addr6 = cfg->vprefix_addr6;
    slirp->vprefix_len = cfg->vprefix_len;
    slirp->vhost_addr6 = cfg->vhost6;
    if (cfg->vhostname) {
        slirp_pstrcpy(slirp->client_hostname, sizeof(slirp->client_hostname),
                      cfg->vhostname);
    }
    if (cfg->tftp_path) {
        slirp->tftp_prefix = strdup(cfg->tftp_path);
    }
    if (cfg->bootfile) {
        slirp->bootp_filename = strdup(cfg->bootfile);
    }
    if (cfg->vdomainname) {
        slirp->vdomainname = strdup(cfg->vdomainname);
    }
    slirp->vdhcp_startaddr = cfg->vdhcp_start;
    slirp->vnameserver_addr = cfg->vnameserver;
    slirp->vnameserver_addr6 = cfg->vnameserver6;
    if (cfg->tftp_server_name) {
        slirp->tftp_server_name = strdup(cfg->tftp_server_name);
    }

    if (cfg->vdnssearch) {
        translate_dnssearch(slirp, cfg->vdnssearch);
    }
    slirp->if_mtu = cfg->if_mtu == 0 ? IF_MTU_DEFAULT : cfg->if_mtu;
    slirp->if_mru = cfg->if_mru == 0 ? IF_MRU_DEFAULT : cfg->if_mru;
    slirp->disable_host_loopback = cfg->disable_host_loopback;
    slirp->enable_emu = cfg->enable_emu;

    if (cfg->version >= 3) {
        slirp->disable_dns = cfg->disable_dns;
    } else {
        slirp->disable_dns = false;
    }

    if (cfg->version >= 4) {
        slirp->disable_dhcp = cfg->disable_dhcp;
    } else {
        slirp->disable_dhcp = false;
    }

    return slirp;
}

Slirp *slirp_init(int restricted, bool in_enabled, struct in_addr vnetwork,
                  struct in_addr vnetmask, struct in_addr vhost,
                  bool in6_enabled, struct in6_addr vprefix_addr6,
                  uint8_t vprefix_len, struct in6_addr vhost6,
                  const char *vhostname, const char *tftp_server_name,
                  const char *tftp_path, const char *bootfile,
                  struct in_addr vdhcp_start, struct in_addr vnameserver,
                  struct in6_addr vnameserver6, const char **vdnssearch,
                  const char *vdomainname, const SlirpCb *callbacks,
                  void *opaque)
{
    SlirpConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;
    cfg.restricted = restricted;
    cfg.in_enabled = in_enabled;
    cfg.vnetwork = vnetwork;
    cfg.vnetmask = vnetmask;
    cfg.vhost = vhost;
    cfg.in6_enabled = in6_enabled;
    cfg.vprefix_addr6 = vprefix_addr6;
    cfg.vprefix_len = vprefix_len;
    cfg.vhost6 = vhost6;
    cfg.vhostname = vhostname;
    cfg.tftp_server_name = tftp_server_name;
    cfg.tftp_path = tftp_path;
    cfg.bootfile = bootfile;
    cfg.vdhcp_start = vdhcp_start;
    cfg.vnameserver = vnameserver;
    cfg.vnameserver6 = vnameserver6;
    cfg.vdnssearch = vdnssearch;
    cfg.vdomainname = vdomainname;
    return slirp_new(&cfg, callbacks, opaque);
}

void slirp_cleanup(Slirp *slirp)
{
    struct gfwd_list *ex_ptr;

    ip_cleanup(slirp);
    m_cleanup(slirp);

    while (slirp->guestfwd_list != NULL) {
        ex_ptr = slirp->guestfwd_list->ex_next;
        free((void*)slirp->guestfwd_list->ex_exec);
        free(slirp->guestfwd_list);
        slirp->guestfwd_list = ex_ptr;
    }

    free(slirp->vdomainname);
    free(slirp->tftp_prefix);
    free(slirp->tftp_server_name);
    free(slirp->bootp_filename);
    free(slirp);
}

#define CONN_CANFSEND(so) (((so)->so_state & (SS_FCANTSENDMORE|SS_ISFCONNECTED)) == SS_ISFCONNECTED)
#define CONN_CANFRCV(so) (((so)->so_state & (SS_FCANTRCVMORE|SS_ISFCONNECTED)) == SS_ISFCONNECTED)
#define UPD_NFDS(x) if (nfds < (x)) nfds = (x)

static void slirp_update_timeout(Slirp *slirp, uint32_t *timeout)
{
    uint32_t t;

    if (*timeout <= TIMEOUT_FAST) {
        return;
    }

    t = MIN(1000, *timeout);

    /* If we have tcp timeout with slirp, then we will fill @timeout with
     * more precise value.
     */
    if (slirp->time_fasttimo) {
        *timeout = TIMEOUT_FAST;
        return;
    }
    if (slirp->do_slowtimo) {
        t = MIN(TIMEOUT_SLOW, t);
    }
    *timeout = t;
}

void slirp_pollfds_fill(Slirp *slirp, uint32_t *timeout,
                        SlirpAddPollCb add_poll, void *opaque)
{
    struct socket *so, *so_next;

    /*
     * First, TCP sockets
     */

    /*
     * *_slowtimo needs calling if there are IP fragments
     * in the fragment queue, or there are TCP connections active
     */
    slirp->do_slowtimo = ((slirp->tcb.so_next != &slirp->tcb) ||
                          (&slirp->ipq.ip_link != slirp->ipq.ip_link.next));

    for (so = slirp->tcb.so_next; so != &slirp->tcb; so = so_next) {
        int events = 0;

        so_next = so->so_next;

        so->pollfds_idx = -1;

        /*
         * See if we need a tcp_fasttimo
         */
        if (slirp->time_fasttimo == 0 && so->so_tcpcb->t_flags & TF_DELACK) {
            slirp->time_fasttimo = curtime; /* Flag when want a fasttimo */
        }

        /*
         * NOFDREF can include still connecting to local-host,
         * newly socreated() sockets etc. Don't want to select these.
         */
        if (so->so_state & SS_NOFDREF || so->s == -1) {
            continue;
        }

        /*
         * Set for reading sockets which are accepting
         */
        if (so->so_state & SS_FACCEPTCONN) {
            so->pollfds_idx = add_poll(
                so->s, SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR, opaque);
            continue;
        }

        /*
         * Set for writing sockets which are connecting
         */
        if (so->so_state & SS_ISFCONNECTING) {
            so->pollfds_idx =
                add_poll(so->s, SLIRP_POLL_OUT | SLIRP_POLL_ERR, opaque);
            continue;
        }

        /*
         * Set for writing if we are connected, can send more, and
         * we have something to send
         */
        if (CONN_CANFSEND(so) && so->so_rcv.sb_cc) {
            events |= SLIRP_POLL_OUT | SLIRP_POLL_ERR;
        }

        /*
         * Set for reading (and urgent data) if we are connected, can
         * receive more, and we have room for it.
         *
         * If sb is already half full, we will wait for the guest to consume it,
         * and notify again in sbdrop() when the sb becomes less than half full.
         */
        if (CONN_CANFRCV(so) &&
            (so->so_snd.sb_cc < (so->so_snd.sb_datalen / 2))) {
            events |= SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR |
                      SLIRP_POLL_PRI;
        }

        if (events) {
            so->pollfds_idx = add_poll(so->s, events, opaque);
        }
    }

    /*
     * UDP sockets
     */
    for (so = slirp->udb.so_next; so != &slirp->udb; so = so_next) {
        so_next = so->so_next;

        so->pollfds_idx = -1;

        /*
         * See if it's timed out
         */
        if (so->so_expire) {
            if (so->so_expire <= curtime) {
                udp_detach(so);
                continue;
            } else {
                slirp->do_slowtimo = true; /* Let socket expire */
            }
        }

        /*
         * When UDP packets are received from over the
         * link, they're sendto()'d straight away, so
         * no need for setting for writing
         * Limit the number of packets queued by this session
         * to 4.  Note that even though we try and limit this
         * to 4 packets, the session could have more queued
         * if the packets needed to be fragmented
         * (XXX <= 4 ?)
         */
        if ((so->so_state & SS_ISFCONNECTED) && so->so_queued <= 4) {
            so->pollfds_idx = add_poll(
                so->s, SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR, opaque);
        }
    }

    /*
     * ICMP sockets
     */
    for (so = slirp->icmp.so_next; so != &slirp->icmp; so = so_next) {
        so_next = so->so_next;

        so->pollfds_idx = -1;

        /*
         * See if it's timed out
         */
        if (so->so_expire) {
            if (so->so_expire <= curtime) {
                icmp_detach(so);
                continue;
            } else {
                slirp->do_slowtimo = true; /* Let socket expire */
            }
        }

        if (so->so_state & SS_ISFCONNECTED) {
            so->pollfds_idx = add_poll(
                so->s, SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR, opaque);
        }
    }

    slirp_update_timeout(slirp, timeout);
}

void slirp_pollfds_poll(Slirp *slirp, int select_error,
                        SlirpGetREventsCb get_revents, void *opaque)
{
    struct socket *so, *so_next;
    int ret;

    curtime = slirp->cb->clock_get_ns(slirp->opaque) / SCALE_MS;

    /*
     * See if anything has timed out
     */
    if (slirp->time_fasttimo &&
        ((curtime - slirp->time_fasttimo) >= TIMEOUT_FAST)) {
        tcp_fasttimo(slirp);
        slirp->time_fasttimo = 0;
    }
    if (slirp->do_slowtimo &&
        ((curtime - slirp->last_slowtimo) >= TIMEOUT_SLOW)) {
        ip_slowtimo(slirp);
        tcp_slowtimo(slirp);
        slirp->last_slowtimo = curtime;
    }

    /*
     * Check sockets
     */
    if (!select_error) {
        /*
         * Check TCP sockets
         */
        for (so = slirp->tcb.so_next; so != &slirp->tcb; so = so_next) {
            int revents;

            so_next = so->so_next;

            revents = 0;
            if (so->pollfds_idx != -1) {
                revents = get_revents(so->pollfds_idx, opaque);
            }

            if (so->so_state & SS_NOFDREF || so->s == -1) {
                continue;
            }

#ifndef __APPLE__
            /*
             * Check for URG data
             * This will soread as well, so no need to
             * test for SLIRP_POLL_IN below if this succeeds.
             *
             * This is however disabled on MacOS, which apparently always
             * reports data as PRI when it is the last data of the
             * connection. We would then report it out of band, which the guest
             * would most probably not be ready for.
             */
            if (revents & SLIRP_POLL_PRI) {
                ret = sorecvoob(so);
                if (ret < 0) {
                    /* Socket error might have resulted in the socket being
                     * removed, do not try to do anything more with it. */
                    continue;
                }
            }
            /*
             * Check sockets for reading
             */
            else
#endif
                if (revents &
                     (SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR | SLIRP_POLL_PRI)) {
                /*
                 * Check for incoming connections
                 */
                if (so->so_state & SS_FACCEPTCONN) {
                    tcp_connect(so);
                    continue;
                } /* else */
                ret = soread(so);

                /* Output it if we read something */
                if (ret > 0) {
                    tcp_output(sototcpcb(so));
                }
                if (ret < 0) {
                    /* Socket error might have resulted in the socket being
                     * removed, do not try to do anything more with it. */
                    continue;
                }
            }

            /*
             * Check sockets for writing
             */
            if (!(so->so_state & SS_NOFDREF) &&
                (revents & (SLIRP_POLL_OUT | SLIRP_POLL_ERR))) {
                /*
                 * Check for non-blocking, still-connecting sockets
                 */
                if (so->so_state & SS_ISFCONNECTING) {
                    /* Connected */
                    so->so_state &= ~SS_ISFCONNECTING;

                    ret = send(so->s, (const void *)&ret, 0, 0);
                    if (ret < 0) {
                        /* XXXXX Must fix, zero bytes is a NOP */
                        if (errno == EAGAIN || errno == EWOULDBLOCK ||
                            errno == EINPROGRESS || errno == ENOTCONN) {
                            continue;
                        }

                        /* else failed */
                        so->so_state &= SS_PERSISTENT_MASK;
                        so->so_state |= SS_NOFDREF;
                    }
                    /* else so->so_state &= ~SS_ISFCONNECTING; */

                    /*
                     * Continue tcp_input
                     */
                    tcp_input((struct mbuf *)NULL, sizeof(struct ip), so);
                    /* continue; */
                } else {
                    ret = sowrite(so);
                    if (ret > 0) {
                        /* Call tcp_output in case we need to send a window
                         * update to the guest, otherwise it will be stuck
                         * until it sends a window probe. */
                        tcp_output(sototcpcb(so));
                    }
                }
            }
        }

        /*
         * Now UDP sockets.
         * Incoming packets are sent straight away, they're not buffered.
         * Incoming UDP data isn't buffered either.
         */
        for (so = slirp->udb.so_next; so != &slirp->udb; so = so_next) {
            int revents;

            so_next = so->so_next;

            revents = 0;
            if (so->pollfds_idx != -1) {
                revents = get_revents(so->pollfds_idx, opaque);
            }

            if (so->s != -1 &&
                (revents & (SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR))) {
                sorecvfrom(so);
            }
        }

        /*
         * Check incoming ICMP relies.
         */
        for (so = slirp->icmp.so_next; so != &slirp->icmp; so = so_next) {
            int revents;

            so_next = so->so_next;

            revents = 0;
            if (so->pollfds_idx != -1) {
                revents = get_revents(so->pollfds_idx, opaque);
            }

            if (so->s != -1 &&
                (revents & (SLIRP_POLL_IN | SLIRP_POLL_HUP | SLIRP_POLL_ERR))) {
                icmp_receive(so);
            }
        }
    }

    if_start(slirp);
}

static void arp_input(Slirp *slirp, const uint8_t *pkt, int pkt_len)
{
    struct slirp_arphdr *ah = (struct slirp_arphdr *)(pkt + ETH_HLEN);
    uint8_t arp_reply[MAX(ETH_HLEN + sizeof(struct slirp_arphdr), 64)];
    struct ethhdr *reh = (struct ethhdr *)arp_reply;
    struct slirp_arphdr *rah = (struct slirp_arphdr *)(arp_reply + ETH_HLEN);
    int ar_op;
    struct gfwd_list *ex_ptr;

    ar_op = ntohs(ah->ar_op);
    switch(ar_op) {
    case ARPOP_REQUEST:
        if (ah->ar_tip == ah->ar_sip) {
            /* Gratuitous ARP */
            arp_table_add(slirp, ah->ar_sip, ah->ar_sha);
            return;
        }

        if ((ah->ar_tip & slirp->vnetwork_mask.s_addr) ==
            slirp->vnetwork_addr.s_addr) {
            if (ah->ar_tip == slirp->vnameserver_addr.s_addr ||
                ah->ar_tip == slirp->vhost_addr.s_addr)
                goto arp_ok;
            for (ex_ptr = slirp->guestfwd_list; ex_ptr; ex_ptr = ex_ptr->ex_next) {
                if (ex_ptr->ex_addr.s_addr == ah->ar_tip)
                    goto arp_ok;
            }
            return;
        arp_ok:
            memset(arp_reply, 0, sizeof(arp_reply));

            arp_table_add(slirp, ah->ar_sip, ah->ar_sha);

            /* ARP request for alias/dns mac address */
            memcpy(reh->h_dest, pkt + ETH_ALEN, ETH_ALEN);
            memcpy(reh->h_source, special_ethaddr, ETH_ALEN - 4);
            memcpy(&reh->h_source[2], &ah->ar_tip, 4);
            reh->h_proto = htons(ETH_P_ARP);

            rah->ar_hrd = htons(1);
            rah->ar_pro = htons(ETH_P_IP);
            rah->ar_hln = ETH_ALEN;
            rah->ar_pln = 4;
            rah->ar_op = htons(ARPOP_REPLY);
            memcpy(rah->ar_sha, reh->h_source, ETH_ALEN);
            rah->ar_sip = ah->ar_tip;
            memcpy(rah->ar_tha, ah->ar_sha, ETH_ALEN);
            rah->ar_tip = ah->ar_sip;
            slirp_send_packet_all(slirp, arp_reply, sizeof(arp_reply));
        }
        break;
    case ARPOP_REPLY:
        arp_table_add(slirp, ah->ar_sip, ah->ar_sha);
        break;
    default:
        break;
    }
}

void slirp_input(Slirp *slirp, const uint8_t *pkt, int pkt_len)
{
    struct mbuf *m;
    int proto;

    if (pkt_len < ETH_HLEN)
        return;

    proto = ntohs(*(uint16_t *)(pkt + 12));
    switch(proto) {
    case ETH_P_ARP:
        arp_input(slirp, pkt, pkt_len);
        break;
    case ETH_P_IP:
    case ETH_P_IPV6:
        m = m_get(slirp);
        if (!m)
            return;
        /* Note: we add to align the IP header */
        if (M_FREEROOM(m) < pkt_len + 2) {
            m_inc(m, pkt_len + 2);
        }
        m->m_len = pkt_len + 2;
        memcpy(m->m_data + 2, pkt, pkt_len);

        m->m_data += 2 + ETH_HLEN;
        m->m_len -= 2 + ETH_HLEN;

        if (proto == ETH_P_IP) {
          ip_input(m);
        } else {
          slirp_warning("IPv6 packet not supported yet", slirp->opaque);
        }
        break;
    default:
        break;
    }
}

/* Prepare the IPv4 packet to be sent to the ethernet device. Returns 1 if no
 * packet should be sent, 0 if the packet must be re-queued, 2 if the packet
 * is ready to go.
 */
static int if_encap4(Slirp *slirp, struct mbuf *ifm, struct ethhdr *eh,
                     uint8_t ethaddr[ETH_ALEN])
{
    const struct ip *iph = (const struct ip *)ifm->m_data;

    if (!arp_table_search(slirp, iph->ip_dst.s_addr, ethaddr)) {
        uint8_t arp_req[2 + ETH_HLEN + sizeof(struct slirp_arphdr)];
        struct ethhdr *reh = (struct ethhdr *)(arp_req + 2);
        struct slirp_arphdr *rah = (struct slirp_arphdr *)(arp_req + 2 + ETH_HLEN);

        if (!ifm->resolution_requested) {
            /* If the client addr is not known, send an ARP request */
            memset(reh->h_dest, 0xff, ETH_ALEN);
            memcpy(reh->h_source, special_ethaddr, ETH_ALEN - 4);
            memcpy(&reh->h_source[2], &slirp->vhost_addr, 4);
            reh->h_proto = htons(ETH_P_ARP);
            rah->ar_hrd = htons(1);
            rah->ar_pro = htons(ETH_P_IP);
            rah->ar_hln = ETH_ALEN;
            rah->ar_pln = 4;
            rah->ar_op = htons(ARPOP_REQUEST);

            /* source hw addr */
            memcpy(rah->ar_sha, special_ethaddr, ETH_ALEN - 4);
            memcpy(&rah->ar_sha[2], &slirp->vhost_addr, 4);

            /* source IP */
            rah->ar_sip = slirp->vhost_addr.s_addr;

            /* target hw addr (none) */
            memset(rah->ar_tha, 0, ETH_ALEN);

            /* target IP */
            rah->ar_tip = iph->ip_dst.s_addr;
            slirp->client_ipaddr = iph->ip_dst;
            slirp_send_packet_all(slirp, arp_req + 2, sizeof(arp_req) - 2);
            ifm->resolution_requested = true;

            /* Expire request and drop outgoing packet after 1 second */
            ifm->expiration_date =
                slirp->cb->clock_get_ns(slirp->opaque) + 1000000000ULL;
        }
        return 0;
    } else {
        memcpy(eh->h_source, special_ethaddr, ETH_ALEN - 4);
        /* XXX: not correct */
        memcpy(&eh->h_source[2], &slirp->vhost_addr, 4);
        eh->h_proto = htons(ETH_P_IP);

        /* Send this */
        return 2;
    }
}

/* Prepare the IPv6 packet to be sent to the ethernet device. Returns 1 if no
 * packet should be sent, 0 if the packet must be re-queued, 2 if the packet
 * is ready to go.
 */
static int if_encap6(Slirp *slirp, struct mbuf *ifm, struct ethhdr *eh,
                     uint8_t ethaddr[ETH_ALEN])
{
    slirp_warning("IPv6 packet not supported yet", slirp->opaque);
    return 0;
}

/* Output the IP packet to the ethernet device. Returns 0 if the packet must be
 * re-queued.
 */
int if_encap(Slirp *slirp, struct mbuf *ifm)
{
    uint8_t buf[IF_MTU_MAX + 100];
    struct ethhdr *eh = (struct ethhdr *)(buf + 2);
    uint8_t ethaddr[ETH_ALEN];
    const struct ip *iph = (const struct ip *)ifm->m_data;
    int ret;
//  char ethaddr_str[ETH_ADDRSTRLEN];

    if (ifm->m_len + ETH_HLEN > (int)sizeof(buf) - 2) {
        return 1;
    }

    switch (iph->ip_v) {
    case IPVERSION:
        ret = if_encap4(slirp, ifm, eh, ethaddr);
        if (ret < 2) {
            return ret;
        }
        break;

    case IP6VERSION:
        ret = if_encap6(slirp, ifm, eh, ethaddr);
        if (ret < 2) {
            return ret;
        }
        break;

    default:
        slirp_warning("unknown protocol", slirp->opaque);
    }

    memcpy(eh->h_dest, ethaddr, ETH_ALEN);
    DEBUG_ARG("src = %s", slirp_ether_ntoa(eh->h_source, ethaddr_str,
                                           sizeof(ethaddr_str)));
    DEBUG_ARG("dst = %s", slirp_ether_ntoa(eh->h_dest, ethaddr_str,
                                           sizeof(ethaddr_str)));
    memcpy(buf + 2 + sizeof(struct ethhdr), ifm->m_data, ifm->m_len);
    slirp_send_packet_all(slirp, buf + 2, ifm->m_len + ETH_HLEN);
    return 1;
}

/* Drop host forwarding rule, return 0 if found. */
int slirp_remove_hostfwd(Slirp *slirp, int is_udp, struct in_addr host_addr,
                         int host_port)
{
    struct socket *so;
    struct socket *head = (is_udp ? &slirp->udb : &slirp->tcb);
    struct sockaddr_in addr;
    int port = htons(host_port);
    socklen_t addr_len;

    for (so = head->so_next; so != head; so = so->so_next) {
        addr_len = sizeof(addr);
        if ((so->so_state & SS_HOSTFWD) &&
            getsockname(so->s, (struct sockaddr *)&addr, &addr_len) == 0 &&
            addr.sin_addr.s_addr == host_addr.s_addr &&
            addr.sin_port == port) {
            close(so->s);
            sofree(so);
            return 0;
        }
    }

    return -1;
}

int slirp_add_hostfwd(Slirp *slirp, int is_udp, struct in_addr host_addr,
                      int host_port, struct in_addr guest_addr, int guest_port)
{
    if (!guest_addr.s_addr) {
        guest_addr = slirp->vdhcp_startaddr;
    }
    if (is_udp) {
        if (!udp_listen(slirp, host_addr.s_addr, htons(host_port),
                        guest_addr.s_addr, htons(guest_port), SS_HOSTFWD))
            return -1;
    } else {
        if (!tcp_listen(slirp, host_addr.s_addr, htons(host_port),
                        guest_addr.s_addr, htons(guest_port), SS_HOSTFWD))
            return -1;
    }
    return 0;
}

/* TODO: IPv6 */
static bool check_guestfwd(Slirp *slirp, struct in_addr *guest_addr,
                           int guest_port)
{
    struct gfwd_list *tmp_ptr;

    if (!guest_addr->s_addr) {
        guest_addr->s_addr = slirp->vnetwork_addr.s_addr |
                             (htonl(0x0204) & ~slirp->vnetwork_mask.s_addr);
    }
    if ((guest_addr->s_addr & slirp->vnetwork_mask.s_addr) !=
            slirp->vnetwork_addr.s_addr ||
        guest_addr->s_addr == slirp->vhost_addr.s_addr ||
        guest_addr->s_addr == slirp->vnameserver_addr.s_addr) {
        return false;
    }

    /* check if the port is "bound" */
    for (tmp_ptr = slirp->guestfwd_list; tmp_ptr; tmp_ptr = tmp_ptr->ex_next) {
        if (guest_port == tmp_ptr->ex_fport &&
            guest_addr->s_addr == tmp_ptr->ex_addr.s_addr)
            return false;
    }

    return true;
}

int slirp_add_exec(Slirp *slirp, const char *cmdline,
                   struct in_addr *guest_addr, int guest_port)
{
    if (!check_guestfwd(slirp, guest_addr, guest_port)) {
        return -1;
    }

    add_exec(&slirp->guestfwd_list, cmdline, *guest_addr, htons(guest_port));
    return 0;
}

int slirp_add_unix(Slirp *slirp, const char *unixsock,
                   struct in_addr *guest_addr, int guest_port)
{
#ifndef _WIN32
    if (!check_guestfwd(slirp, guest_addr, guest_port)) {
        return -1;
    }

    add_unix(&slirp->guestfwd_list, unixsock, *guest_addr, htons(guest_port));
    return 0;
#else
    return -1;
#endif
}

int slirp_add_guestfwd(Slirp *slirp, SlirpWriteCb write_cb, void *opaque,
                       struct in_addr *guest_addr, int guest_port)
{
    if (!check_guestfwd(slirp, guest_addr, guest_port)) {
        return -1;
    }

    add_guestfwd(&slirp->guestfwd_list, write_cb, opaque, *guest_addr,
                 htons(guest_port));
    return 0;
}

int slirp_remove_guestfwd(Slirp *slirp, struct in_addr guest_addr,
                          int guest_port)
{
    return remove_guestfwd(&slirp->guestfwd_list, guest_addr,
                           htons(guest_port));
}

slirp_ssize_t slirp_send(struct socket *so, const void *buf, size_t len, int flags)
{
    if (so->s == -1 && so->guestfwd) {
        /* XXX this blocks entire thread. Rewrite to use
         * qemu_chr_fe_write and background I/O callbacks */
        so->guestfwd->write_cb(buf, len, so->guestfwd->opaque);
        return len;
    }

    if (so->s == -1) {
        /*
         * This should in theory not happen but it is hard to be
         * sure because some code paths will end up with so->s == -1
         * on a failure but don't dispose of the struct socket.
         * Check specifically, so we don't pass -1 to send().
         */
        errno = EBADF;
        return -1;
    }

    return send(so->s, buf, len, flags);
}

struct socket *slirp_find_ctl_socket(Slirp *slirp, struct in_addr guest_addr,
                                            int guest_port)
{
    struct socket *so;

    for (so = slirp->tcb.so_next; so != &slirp->tcb; so = so->so_next) {
        if (so->so_faddr.s_addr == guest_addr.s_addr &&
            htons(so->so_fport) == guest_port) {
            return so;
        }
    }
    return NULL;
}

size_t slirp_socket_can_recv(Slirp *slirp, struct in_addr guest_addr,
                             int guest_port)
{
    struct iovec iov[2];
    struct socket *so;

    so = slirp_find_ctl_socket(slirp, guest_addr, guest_port);

    if (!so || so->so_state & SS_NOFDREF) {
        return 0;
    }

    if (!CONN_CANFRCV(so) || so->so_snd.sb_cc >= (so->so_snd.sb_datalen/2)) {
        return 0;
    }

    return sopreprbuf(so, iov, NULL);
}

void slirp_socket_recv(Slirp *slirp, struct in_addr guest_addr, int guest_port,
                       const uint8_t *buf, int size)
{
    int ret;
    struct socket *so = slirp_find_ctl_socket(slirp, guest_addr, guest_port);

    if (!so)
        return;

    ret = soreadbuf(so, (const char *)buf, size);

    if (ret > 0)
        tcp_output(sototcpcb(so));
}

void slirp_send_packet_all(Slirp *slirp, const void *buf, size_t len)
{
    slirp_ssize_t ret = slirp->cb->send_packet(buf, len, slirp->opaque);

    if (ret < 0) {
        slirp_warning("Failed to send packet", slirp->opaque);
    } else if ((size_t)ret < len) {
        slirp_warning("send_packet() didn't send all data", slirp->opaque);
    }
}

const char *slirp_version_string(void)
{
    return SLIRP_VERSION_STRING;
}

#endif
