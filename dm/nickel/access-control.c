#include <dm/config.h>
#include <dm/qemu_glue.h>
#include <dm/control.h>
#include <dm/async-op.h>

#include "access-control.h"
#include "dns/dns.h"
#include "dns/dns-fake.h"
#include "log.h"
#include "nickel.h"
#include "rpc.h"
#include "service.h"
#include "lava.h"

/* assume a 3Ghz CPU, it's around 20s */
#define FLUSH_TSC_GAP   (1ULL<<36)

#define EXAMINE_IP_FROM_DNS     0x1
#define DONT_CACHE_ALLOWED_IP   0x2

#define HASH_CACHE_NO_FLUSH     0x1

struct sock_addr {
    struct net_addr addr;
    uint8_t prefix_len;
    uint16_t port;
};

struct ac_host {
    struct ac_host *next;
    char        name[256];
    uint64_t    accessed_tsc;
};

struct ac_network {
    struct ac_network  *next;
    struct sock_addr net_sock;
    uint32_t        options;
    uint64_t        accessed_tsc;
};

static void set_policy_type(struct nickel *ni, int policy);
static void query_access_policy(struct nickel *ni);
static void query_access_policy(struct nickel *ni);
static void query_proxy_config(struct nickel *ni);

static struct net_addr ipv6_loopback_addr = { 0 };

/* RPC */
#define RPC_STR_ALLOC_GRAN  256
static int ac_rpc_IsIPAddressAllowed(struct nickel *ni, const struct net_addr *addr,
                                     uint16_t port,
                                     unsigned long options,
                                     int *block_all_for_port, int *allowed)
{
    int ret = 0;
    char buf[64];
    dict args, response;
    char netabuf[NETADDR_MAXSTRLEN];

    args = dict_new();

    dict_put_string(args, "addr", NETADDR_STR(addr, netabuf, sizeof(netabuf)));

    snprintf(buf, 64, "%lu", options);
    dict_put_string(args, "options", buf);
    dict_put_integer(args, "port", ntohs(port));

    if (ni_rpc_send_sync(ni, "nc_IsIPAddressAllowed", args, &response))
       goto out;
    *block_all_for_port = (int) dict_get_integer_default(response, "block_all_for_port", 0);
    *allowed = dict_get_integer(response, "allowed");
    dict_free(response);
    ret = 1;

out:
    dict_free(args);
    return ret;
}

static int ac_rpc_IsListIPAddressAllowed(struct nickel *ni, const struct net_addr **ips, int len,
        uint16_t port, unsigned long options, int *block_all_for_port, char **ret_mask)
{
    int ret = 0, i;
    char buf[NETADDR_MAXSTRLEN];
    char netabuf[NETADDR_MAXSTRLEN];
    dict args = NULL, response;
    char *addr_list = NULL, *tmp;
    const char *tmp2;
    int blen = 0;

    args = dict_new();
    if (!args)
        goto mem_err;
    addr_list = calloc(1, RPC_STR_ALLOC_GRAN + 1);
    if (!addr_list)
        goto mem_err;
    blen += RPC_STR_ALLOC_GRAN;

    for (i = 0; i < len; i++) {
        if (strlen(addr_list) + NETADDR_MAXSTRLEN > blen) {
            tmp = realloc(addr_list, blen + RPC_STR_ALLOC_GRAN + 1);
            if (!tmp)
                goto mem_err;
            blen += RPC_STR_ALLOC_GRAN;
            addr_list = tmp;
        }

        buf[NETADDR_MAXSTRLEN - 1] = 0;
        strncpy(buf, NETADDR_STR(ips[i], netabuf, sizeof(netabuf)),
                NETADDR_MAXSTRLEN-1);
        if (*addr_list)
            strcat(addr_list, ",");
        strcat(addr_list, buf);
    }

    dict_put_string(args, "addr-list", addr_list);

    snprintf(buf, NETADDR_MAXSTRLEN, "%lu", options);
    dict_put_string(args, "options", buf);
    dict_put_integer(args, "port", ntohs(port));

    if (ni_rpc_send_sync(ni, "nc_IsListIPAddressAllowed", args, &response))
       goto out;
    *block_all_for_port = (int) dict_get_integer_default(response, "block_all_for_port", 0);
    *ret_mask = NULL;
    tmp2 = dict_get_string(response, "allowed-mask");
    if (tmp2)
        *ret_mask = strdup(tmp2);
    dict_free(response);
    ret = 1;
out:
    if (args)
        dict_free(args);
    free(addr_list);
    return ret;
mem_err:
    warnx("%s: memory ERROR", __FUNCTION__);
    ret = 0;
    goto out;
}

static int ac_rpc_IsDNSNameAllowed(struct nickel *ni, const char *dnsname, int *allowed)
{
    int ret = 0;
    dict args, response;

    args = dict_new();
    dict_put_string(args, "name", dnsname);
    if (ni_rpc_send_sync(ni, "nc_IsDNSNameAllowed", args, &response))
        goto out;
    *allowed = dict_get_integer(response, "result");
    dict_free(response);
    ret = 1;
out:
    dict_free(args);
    return ret;
}

static int ac_rpc_QueryPolicyType(struct nickel *ni, int *type)
{
    int ret = 0;
    dict args, response;

    args = dict_new();
    if (ni_rpc_send_sync(ni, "nc_QueryPolicyType", args, &response))
        goto out;
    *type = dict_get_integer(response, "result");
    dict_free(response);
    ret = 1;
out:
    dict_free(args);
    return ret;
}

static int ac_rpc_HasConfig(struct nickel *ni, int *result)
{
    int ret = 0;
    dict args, response;

    args = dict_new();
    if (ni_rpc_send_sync(ni, "nc_HasConfig", args, &response))
        goto out;
    *result = dict_get_integer(response, "result");
    ret = 1;
    dict_free(response);
out:
    dict_free(args);
    return ret;
}

static void rpc_on_event(void *opaque)
{
    struct ni_rpc_response *r = opaque;
    const char *command;
    int val;

    command = dict_get_string(r->d, "command");
    if (!command)
        goto out;

    if (!strcmp(command, "nc_AccessControlChange") ||
        !strcmp(command, "nc_NetworkPolicyRefresh")) {

        if (!r->ni->ac_enabled)
            goto out;

        val = dict_get_integer_default(r->d, "policy", -1);
        if (val != -1)
            set_policy_type(r->ni, val);
#ifdef _WIN32
        query_access_policy(r->ni);
        query_proxy_config(r->ni);
#else
        val = dict_get_integer_default(r->d, "proxy-config", -1);
        if (val != -1) {
            r->ni->ac_proxy_has_config = (val != 0);
            NETLOG("nc_AccessControlChange: proxy config is now %s",
                    r->ni->ac_proxy_has_config ? "ON" : "OFF");
        }
#endif
    }

out:
    dict_free(r->d);
    free(r);
}

static inline uint64_t _rdtsc()
{
    uint32_t low, high;
    uint64_t val;
    asm volatile("rdtsc" : "=a" (low), "=d" (high));
    val = high;
    val <<= 32;
    val |= low;
    return val;
}

/* this string hash function is from openssl */
/* The following hash seems to work very well on normal text strings
 * no collisions on /usr/dict/words and it distributes on %2^n quite
 * well, not as good as MD5, but still good.
 */
static uint32_t lh_strhash(const char *c)
{
    uint32_t ret = 0;
    uint32_t n;
    uint32_t v;
    int r;

    if ((c == NULL) || (*c == '\0'))
        return(ret);

    /*
       unsigned char b[16];
       MD5(c,strlen(c),b);
       return(b[0]|(b[1]<<8)|(b[2]<<16)|(b[3]<<24));
     */

    n = 0x100;
    while (*c) {
        v = n | (*c);
        n += 0x100;
        r = (int)((v >> 2) ^ v) & 0x0f;
        ret = (ret << r) | (ret >> (32 - r));
        ret &= 0xFFFFFFFFL;
        ret ^= v * v;
        c++;
    }

    return ((ret >> 16) ^ ret);
}

static bool hash_find_host(struct nickel *ni, const char *hostname, struct ac_host **hosts)
{
    bool ret = false;
    struct ac_host *host, **phost;
    uint64_t tsc;

    critical_section_enter(&ni->ac_lk);
    host = hosts[BAC_HASH(lh_strhash(hostname))];
    phost = &hosts[BAC_HASH(lh_strhash(hostname))];
    tsc = _rdtsc();    //RDTSC is the fastest

    while (host) {
        if (0 == strncmp(host->name, hostname, 255)) {
            host->accessed_tsc = tsc;
            NETLOG5("(ac) found %s host %s.",
                        hosts == ni->ac_denied_hosts ? "denied" : "allowed",
                        hostname);
            ret = true;
            goto out;
        }

        if ((tsc - host->accessed_tsc) > FLUSH_TSC_GAP)  { //wrap? impossible!
            NETLOG5("(ac) remove entry %s.", host->name);

            *phost = host->next;
            free(host);
            host = *phost;

            if (hosts == ni->ac_denied_hosts)
                ni->ac_n_denied_hosts--;
            else
                ni->ac_n_allowed_hosts--;
        } else {
            phost = &host->next;
            host = host->next;
        }
    }

out:
    critical_section_leave(&ni->ac_lk);
    return ret;
}

static void hash_insert_host(struct nickel *ni, const char *hostname, struct ac_host **hosts)
{
    struct ac_host *new;
    uint32_t hash;

    new = calloc(1, sizeof(struct ac_host));
    if (!new)
        return;

    critical_section_enter(&ni->ac_lk);

    strncpy(new->name, hostname, 256);
    new->accessed_tsc = _rdtsc(); //RDTSC should be the fastest

    hash = BAC_HASH(lh_strhash(hostname));
    new->next = hosts[hash];
    hosts[hash] = new;

    NETLOG5("(ac) adding %s host %s.",
                hosts == ni->ac_denied_hosts ? "denied" : "allowed", hostname);

    if (hosts == ni->ac_denied_hosts)
        ni->ac_n_denied_hosts++;
    else
        ni->ac_n_allowed_hosts++;

    critical_section_leave(&ni->ac_lk);
}

static bool hash_find_ip_port(struct nickel *ni, const struct net_addr *ip, uint16_t port,
                            struct ac_network **networks)
{
    bool ret = false;
    struct ac_network *network, **pnetwork;
    uint64_t tsc;
    char netabuf[NETADDR_MAXSTRLEN];

    critical_section_enter(&ni->ac_lk);

    NETLOG5("(ac) search %s,%u in %s network.",
                NETADDR_STR(ip, netabuf, sizeof(netabuf)), ntohs(port),
                networks == ni->ac_denied_networks ? "denied" : "allowed");


    network = networks[BAC_HASHADDR(ip)];
    pnetwork = &networks[BAC_HASHADDR(ip)];
    tsc = _rdtsc();    //RDTSC is the fastest

    while (network) {
        if ((network->net_sock.prefix_len == 0 ||
            NETADDR_CMP(ip, &network->net_sock.addr, network->net_sock.prefix_len) == 0) &&
            port == network->net_sock.port) {

            network->accessed_tsc = tsc;
            NETLOG5("(ac) found %s network %s/%d,%u",
                        networks == ni->ac_denied_networks ? "denied" : "allowed",
                        NETADDR_STR(&network->net_sock.addr, netabuf, sizeof(netabuf)),
                        (int) network->net_sock.prefix_len,
                        (unsigned) ntohs(network->net_sock.port));
            ret = true;
            goto out;
        }

        if (!(network->options & HASH_CACHE_NO_FLUSH) &&
            (tsc - network->accessed_tsc) > FLUSH_TSC_GAP)
        {
            NETLOG5("(ac) remove entry %s/%d,%u.",
                        NETADDR_STR(&network->net_sock.addr, netabuf, sizeof(netabuf)),
                        (int) network->net_sock.prefix_len,
                        (unsigned) ntohs(network->net_sock.port));

            *pnetwork = network->next;
            free(network);
            network = *pnetwork;

            if (networks == ni->ac_denied_networks)
                ni->ac_n_denied_networks--;
            else
                ni->ac_n_allowed_networks--;
        } else {
            pnetwork = &network->next;
            network = network->next;
        }
    }

out:
    critical_section_leave(&ni->ac_lk);
    return ret;
}

static void hash_insert_network_port(struct nickel *ni, const struct net_addr *ip,
                                uint8_t prefix_len,
                                uint16_t port,
                                uint32_t options,
                                struct ac_network **networks)
{
    struct ac_network *new;
    uint32_t hash;
    char netabuf[NETADDR_MAXSTRLEN];

    new = calloc(1, sizeof(struct ac_network));
    if (!new)
        return;

    critical_section_enter(&ni->ac_lk);

    new->net_sock.addr = *ip;
    new->net_sock.port = port;
    if (ip->family == AF_INET)
        new->net_sock.addr.ipv4.s_addr &= ((1ULL << prefix_len) - 1);
    new->net_sock.prefix_len    = prefix_len;
    new->options       = options;
    new->accessed_tsc  = _rdtsc(); //RDTSC should be the fastest

    hash = BAC_HASHADDR(ip);
    new->next = networks[hash];
    networks[hash] = new;

    NETLOG5("(ac) adding %s ip,port %s/%d,%u options %x.",
                networks == ni->ac_denied_networks ? "denied" : "allowed",
                NETADDR_STR(&new->net_sock.addr, netabuf, sizeof(netabuf)),
                (int) prefix_len, (unsigned) ntohs(port),
                (unsigned int) options);

    if (networks == ni->ac_denied_networks)
        ni->ac_n_denied_networks++;
    else {
        ni->ac_n_allowed_networks++;
        /* port = 0 indicates IP from guest DNS lookups */
        if (port == 0)
            ni->ac_n_allowed_dns_ips++;
    }

    critical_section_leave(&ni->ac_lk);
}

/* flush hash cache */
static void hash_flush(struct nickel *ni, uint32_t options)
{
    struct ac_host *host, **phost;
    struct ac_network *network, **pnetwork;
    int i;


    critical_section_enter(&ni->ac_lk);
    for (i = 0; i < (1<<BAC_HASHSIZE); i++) {
        host = ni->ac_allowed_hosts[i];
        phost = &ni->ac_allowed_hosts[i];
        while (host) {
            *phost = host->next;
            free(host);
            host = *phost;

            ni->ac_n_allowed_hosts--;
        }

        host = ni->ac_denied_hosts[i];
        phost = &ni->ac_denied_hosts[i];
        while (host) {
            *phost = host->next;
            free(host);
            host = *phost;

            ni->ac_n_denied_hosts--;
        }

        network = ni->ac_allowed_networks[i];
        pnetwork = &ni->ac_allowed_networks[i];
        while (network) {
            if ((options & HASH_CACHE_NO_FLUSH) &&
                (network->options & HASH_CACHE_NO_FLUSH))
            {
                pnetwork = &network->next;
                network = network->next;
                continue;
            }

            *pnetwork = network->next;
            free(network);
            network = *pnetwork;

            ni->ac_n_allowed_networks--;
        }

        network = ni->ac_denied_networks[i];
        pnetwork = &ni->ac_denied_networks[i];
        while (network) {
            *pnetwork = network->next;
            free(network);
            network = *pnetwork;

            ni->ac_n_denied_networks--;
        }
    }

    if (ni->ac_n_allowed_hosts    != 0 ||
        ni->ac_n_denied_hosts     != 0 ||
        (!(options & HASH_CACHE_NO_FLUSH) && ni->ac_n_allowed_networks != 0) ||
        ni->ac_n_denied_networks  != 0) {
        NETLOG("(ac) memory leak in hash cache");
    } else {
        NETLOG("(ac) memory in hash cache released");
    }

    critical_section_leave(&ni->ac_lk);
}

static void hash_flush_all(struct nickel *ni)
{
    hash_flush(ni, 0);
}

static bool is_ipv6_loopback_address(const struct net_addr *addr)
{
    return NETADDR_CMP(&ipv6_loopback_addr, addr, 0) == 0;
}

static int get_dns_ips(struct nickel *ni, struct net_addr **dns_ips, uint32_t *dns_ips_size)
{
    int ret = 1;
    struct ac_network *network;
    int i, nr_dns_ips;
    struct net_addr *ips;

    critical_section_enter(&ni->ac_lk);
    if (dns_ips_size == NULL || dns_ips == NULL) {
        ret = 0;
        goto out;
    }

    if (!ni->ac_dns_ip_only) {
        *dns_ips = NULL;
        *dns_ips_size = 0;
        ret = 1;
        goto out;
    }

    nr_dns_ips = ni->ac_n_allowed_dns_ips;

    ips = calloc(1, sizeof(*ips) * nr_dns_ips);
    if (ips == NULL) {
        ret = 0;
        goto out;
    }

    *dns_ips = ips;
    *dns_ips_size = nr_dns_ips;

    for (i = 0; i < (1<<BAC_HASHSIZE); i++) {
        network = ni->ac_allowed_networks[i];
        while (network) {
            if (network->options & HASH_CACHE_NO_FLUSH) {
                if (nr_dns_ips == 0) {
                    free(*dns_ips);
                    *dns_ips = NULL;
                    *dns_ips_size = 0;
                    ret = 0;
                    goto out;
                }

                *ips++ = network->net_sock.addr;
                nr_dns_ips--;
            }
            network = network->next;
        }
    }


out:
    critical_section_leave(&ni->ac_lk);
    return ret;
}

static void set_dns_ips(struct nickel *ni, struct net_addr *dns_ips, uint32_t dns_ips_size)
{
    int i;
    struct net_addr net_ip;

    if (dns_ips_size)
        ni->ac_dns_ip_only = 1;

    for (i = 0; i < dns_ips_size; i++) {
        net_ip = *dns_ips++;
        ac_add_ip_from_dns(ni, &net_ip);
    }
}

static void set_policy_type(struct nickel *ni, int policy)
{
    if (policy == ni->ac_prev_policy)
        return;
    ni->ac_prev_policy = policy;

    if (policy == BrNAPT_DENY_ALL) {
        hash_flush_all(ni);
        ni->ac_policy = DENY_ALL;
        goto access_policy_done;
    }

    if (policy == BrNAPT_ALLOW_ALL) {
        hash_flush_all(ni);
        ni->ac_policy = ALLOW_ALL;
        goto access_policy_done;
    }

    hash_flush(ni, HASH_CACHE_NO_FLUSH);

    if (policy == BrNAPT_RESTRICTED_IP)
        ni->ac_dns_ip_only = 1;
    else
        ni->ac_dns_ip_only = 0;

    ni->ac_policy = RESTRICTED;

access_policy_done:
    NETLOG("(ac) access policy is %s all",
            ni->ac_policy == RESTRICTED ? "restricted" :
                                          (ni->ac_policy == ALLOW_ALL ? "allow" : "deny"));
}

static void query_access_policy(struct nickel *ni)
{
    int policy = ALLOW_ALL;

    policy = BrNAPT_DENY_ALL;
    if (!ac_rpc_QueryPolicyType(ni, &policy) && ni->ac_prev_policy > 0)
        return;
    set_policy_type(ni, policy);
}

static void query_proxy_config(struct nickel *ni)
{
    int result = 0;

    if (ac_rpc_HasConfig(ni, &result))
        ni->ac_proxy_has_config = (result != 0);
}

static int
check_ips_port(struct nickel *ni, const struct net_addr *dst_ips, uint16_t port,
        unsigned int options, char *ret_mask, int len)
{
    int ret = 0;
    int i, j, k;
    uint8_t prefix_len = 32;
    int block_all_for_port = 0;
    const struct net_addr **list_ips = NULL;
    char *rpc_resp_mask = NULL;
    char netabuf[NETADDR_MAXSTRLEN];

    if (len <= 0)
        goto out;
    memset(ret_mask, '1', len);
    if (ni->ac_policy == ALLOW_ALL)
        goto out;

    if (ni->ac_policy == DENY_ALL) {
        memset(ret_mask, '0', len);
        goto out;
    }

    NETLOG5("(ac) check list ips in local cache, port %u ...", (unsigned) ntohs(port));

    k = 0;
    list_ips = calloc(1, sizeof(void *) * len);
    if (!list_ips) {
        ret = -1;
        goto out;
    }
    memset(ret_mask, '2', len);
    for (i = 0; i < len; i++) {
        if (is_ipv6_loopback_address(dst_ips + i)) {
            NETLOG5("IPv6 lookup address DENIED");
            ret_mask[i] = '0';
            continue;
        }

        /* local cache lookup first */
        if (hash_find_ip_port(ni, dst_ips + i, port, ni->ac_denied_networks)) {
            ret_mask[i] = '0';
            continue;
        }

        if (hash_find_ip_port(ni, dst_ips + i, port, ni->ac_allowed_networks)) {
            ret_mask[i] = '1';
            continue;
        }

        netabuf[0] = 0;
        NETADDR_STR(dst_ips + i, netabuf, sizeof(netabuf));
        if (netabuf[0] == 0) {
            NETLOG2("WARN - NETADDR_STR failed - IP DENIED");
            ret_mask[i] = '0';
            continue;
        }

        list_ips[k++] = dst_ips + i;
    }

    if (!k)
        goto out;

    /* slow path */
    NETLOG5("(ac) fall to slow path to check a list of %d ip addresses,port ...", k);
    if (!ac_rpc_IsListIPAddressAllowed(ni, list_ips, k, port, options & EXAMINE_IP_FROM_DNS,
                &block_all_for_port, &rpc_resp_mask)) {
        ret = -1;
        NETLOG("%s: rpc ERROR on ac_rpc_IsListIPAddressAllowed", __FUNCTION__);
        goto out;
    }

    if (!rpc_resp_mask)
        goto out;

    if (k != strlen(rpc_resp_mask))
        goto out;

    if (block_all_for_port) {
        prefix_len = 0;
        memset(ret_mask, '0', len); /* all DENIED */
        hash_insert_network_port(ni, list_ips[0], prefix_len, port, 0, ni->ac_denied_networks);
        NETLOG2("(ac) connections to (any IPs, port %u) DENIED", (unsigned) ntohs(port));
        ret = 0;
        goto out;
    }

    for (j = 0, i = 0; j < k; j++) {
        while (i < len && ret_mask[i] != '2')
            i++;
        if (i < len)
            ret_mask[i++] = rpc_resp_mask[j];
        if (options & DONT_CACHE_ALLOWED_IP)
            continue;

        if (rpc_resp_mask[j] == '0') {
            hash_insert_network_port(ni, list_ips[j], prefix_len, port, 0, ni->ac_denied_networks);
            continue;
        }

        if (rpc_resp_mask[j] != '1')
            continue;

        hash_insert_network_port(ni, list_ips[j], prefix_len, port, 0, ni->ac_allowed_networks);
    }

    ret = 0;
out:
    free(rpc_resp_mask);
    free(list_ips);
    return ret;
}

static bool is_ip_port_allowed(struct nickel *ni, const struct net_addr *dst_ip, uint16_t port,
                               unsigned int options)
{
    int ret;
    uint8_t prefix_len = 32;
    int block_all_for_port = 0;
    char netabuf[NETADDR_MAXSTRLEN];

    if (ni->ac_policy == ALLOW_ALL)
        return true;

    if (ni->ac_policy == DENY_ALL)
        return false;

    if (dst_ip->family == AF_INET && fakedns_is_fake(&dst_ip->ipv4)) {
        if (fakedns_is_denied(&dst_ip->ipv4))
            return false;

        return true; /* they will be further checked at connection time */
    }

    if (is_ipv6_loopback_address(dst_ip)) {
        static int log_once = 0;

        if (!log_once)
            NETLOG4("(ac) IPv6 loopback address destination (always) DENIED");
        log_once = 1;
        return false;
    }

    NETLOG5("(ac) check ip %s,%d in local cache ...",
            NETADDR_STR(dst_ip, netabuf, sizeof(netabuf)),
            ntohs(port));

    /* local cache lookup first */
    if (hash_find_ip_port(ni, dst_ip, port, ni->ac_denied_networks))
        return false;

    if (hash_find_ip_port(ni, dst_ip, port, ni->ac_allowed_networks))
        return true;

    netabuf[0] = 0;
    NETADDR_STR(dst_ip, netabuf, sizeof(netabuf));
    if (netabuf[0] == 0) {
        NETLOG2("NETADDR_STR failed - IP DENIED");
        return false;
    }

    /* slow path */
    NETLOG5("(ac) fall to slow path to check ip,port %s,%u ...",
            NETADDR_STR(dst_ip, netabuf, sizeof(netabuf)),
            (unsigned) ntohs(port));
    if (!ac_rpc_IsIPAddressAllowed(ni, dst_ip, port, options & EXAMINE_IP_FROM_DNS,
                &block_all_for_port, &ret)) {
        NETLOG("(ac) ERROR when querying IP,port %s,%u",
                NETADDR_STR(dst_ip, netabuf, sizeof(netabuf)),
                (unsigned) ntohs(port));
        return false;
    }

    NETLOG2("(ac) ip %s/%d,%u %s%s", NETADDR_STR(dst_ip, netabuf, sizeof(netabuf)),
                (int) prefix_len, (unsigned) ntohs(port),
                ret ? "allowed" : "DENIED",
                !ret && block_all_for_port ? " (all IPs blocked for that port)" : "");

    if (ret) {
        if (options & DONT_CACHE_ALLOWED_IP)
            return true;
        hash_insert_network_port(ni, dst_ip, prefix_len, port, 0, ni->ac_allowed_networks);
        return true;
    }
    if (block_all_for_port)
        prefix_len = 0;
    hash_insert_network_port(ni, dst_ip, prefix_len, port, 0, ni->ac_denied_networks);

    return false;
}

bool ac_is_dnsname_allowed(struct nickel *ni, const char *hostname)
{
    int ret;

    if (!ni->ac_enabled)
        return true;

    if (ni->ac_policy == ALLOW_ALL)
        return true;

    if (ni->ac_policy == DENY_ALL)
        return false;

    NETLOG5("(ac) check host %s in local cache ...", hostname);

    /* local cache lookup first */
    if (hash_find_host(ni, hostname, ni->ac_denied_hosts))
        return false;

    if (hash_find_host(ni, hostname, ni->ac_allowed_hosts))
        return true;

    /* slow path */
    NETLOG5("(ac) fall to slow path to check host %s ...", (char*)hostname);
    if (!ac_rpc_IsDNSNameAllowed(ni, hostname, &ret)) {
        NETLOG("(ac) ERROR when querying DNS name %s", hostname);
        return false;
    }

    NETLOG2("(ac) host %s %s", hostname,
                ret ? "allowed" : "DENIED");

    if (ret) {
        hash_insert_host(ni, hostname, ni->ac_allowed_hosts);
        return true;
    }

    hash_insert_host(ni, hostname, ni->ac_denied_hosts);
    return false;
}

bool ac_is_ip_port_allowed(struct nickel *ni, const struct net_addr *addr, uint16_t port)
{
    return is_ip_port_allowed(ni, addr, port, 0);
}

int ac_check_dns_ips_port(struct nickel *ni, struct net_addr *ips, uint16_t port,
                           char *ret_mask, int len)
{
    return check_ips_port(ni, ips, port, EXAMINE_IP_FROM_DNS, ret_mask, len);
}

void ac_add_ip_from_dns(struct nickel *ni, const struct net_addr *dns_ip)
{
    uint16_t port;

    if (!ni->ac_dns_ip_only)
        return;

    /* port is 0 for this special case here */
    port = 0;
    if (hash_find_ip_port(ni, dns_ip, port, ni->ac_allowed_networks))
        return;

    hash_insert_network_port(ni, dns_ip, 32, port, HASH_CACHE_NO_FLUSH, ni->ac_allowed_networks);
}


bool ac_proxy_set(struct nickel *ni)
{
    return ni->ac_proxy_has_config;
}

int ac_tcp_input_syn(struct nickel *ni, struct sockaddr_in saddr,
        struct sockaddr_in daddr)
{
    int ret = -1;
    struct net_addr addr;
    uint32_t options = 0;

    if (!ni->ac_enabled)
        return 0;

    if (ni->ac_max_tcp_conn && ni->ac_max_tcp_conn < ni->number_tcp_sockets) {
        static int log_once = 0;

        if (!log_once) {
            NETLOG("%s: WARNING, max number of guest tcp connections (%u) "
                "exceeded ...", __FUNCTION__, (unsigned int) ni->ac_max_tcp_conn);
            NETLOG("%s: ... DROPPING extra connection(s)", __FUNCTION__);
            log_once = 1;
        }

        goto out;
    }

    if ((daddr.sin_addr.s_addr & ni->network_mask.s_addr) == ni->network_addr.s_addr) {
        if (daddr.sin_addr.s_addr == ni->host_addr.s_addr &&
            ni_is_tcp_vmfwd(ni, daddr.sin_addr, daddr.sin_port)) {

            NETLOG5("(ac) outgoing tcp connection to %s:%d allowed",
                    inet_ntoa(daddr.sin_addr), (int) ntohs(daddr.sin_port));
            goto allow;
        }

        if (ni->ac_allow_loopback_redirect && daddr.sin_addr.s_addr ==
                                            ni->loopback_redirect_addr.s_addr) {

            NETLOG5("(ac) outgoing tcp connection to %s:%d "
                    "will be redirected to the host loopback address",
                    inet_ntoa(daddr.sin_addr), (int) ntohs(daddr.sin_port));

            daddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            goto check_ip;
        }

        NETLOG("(ac) outgoing tcp connection to %s:%d DENIED",
                inet_ntoa(daddr.sin_addr), (int) ntohs(daddr.sin_port));

        goto out;
    }

check_ip:
    addr.family = AF_INET;
    addr.ipv4 = daddr.sin_addr;

    /* coming from previous DNS lookup ? */
    if (ni->ac_dns_ip_only && hash_find_ip_port(ni, &addr, 0, ni->ac_allowed_networks))
        options |= EXAMINE_IP_FROM_DNS;

    if (!is_ip_port_allowed(ni, &addr, daddr.sin_port, options)) {
        NETLOG("(ac) outgoing tcp connection to %s:%d DENIED",
                inet_ntoa(daddr.sin_addr), (int) ntohs(daddr.sin_port));

        goto out;
    }

allow:
    ret = 0;
out:
    return ret;
}

int ac_udp_input(struct nickel *ni, struct sockaddr_in saddr,
        struct sockaddr_in daddr)
{
    int ret = 0;
    struct net_addr addr;
    uint32_t options = 0;

    if (!ni->ac_enabled)
        goto out;

    if ((daddr.sin_addr.s_addr & ni->network_mask.s_addr) == ni->network_addr.s_addr) {
#ifdef __APPLE__
        if (daddr.sin_port == htons(53) && ni->ac_policy == DENY_ALL) {
            ret = -1;
            goto out;
        }
#endif

        if (ni_is_udp_vmfwd(ni, daddr.sin_addr, daddr.sin_port))
            goto out;

         ret = -1;
         goto out;
    }

    if (ni->ac_block_other_udp_icmp) {
        ret = -1;
        goto out;
    }

    addr.family = AF_INET;
    addr.ipv4 = daddr.sin_addr;

    /* coming from previous DNS lookup ? */
    if (ni->ac_dns_ip_only && hash_find_ip_port(ni, &addr, 0, ni->ac_allowed_networks))
        options |= EXAMINE_IP_FROM_DNS;

    if (!is_ip_port_allowed(ni, &addr, daddr.sin_port, options)) {
        ret = -1;
        goto out;
    }

    ret = 0;

out:
    return ret;
}

void ac_set_policy_type(struct nickel *ni, int policy)
{
    if (!ni->ac_enabled)
        return;

    set_policy_type(ni, policy);
}

int ac_init(struct nickel *ni)
{
    int err = 0;

    critical_section_init(&ni->ac_lk);
    if (inet_pton(AF_INET6, "::1", (void *) &ipv6_loopback_addr.ipv6) == 1) {
        ipv6_loopback_addr.family = AF_INET6;
    } else {
        NETLOG("WARNING - inet_pton(::1) FAILED !");
        /* shall we fail here ? */
    }
    ni->ac_prev_policy = -1;
    if (!ni->ac_enabled)
        goto out;

    ni->ac_evt_cb = rpc_on_event;
    ni->ac_policy = RESTRICTED;
    set_policy_type(ni, ni->ac_default_policy);

    if (ni->ac_max_tcp_conn)
        NETLOG("max number of guest tcp connections set to %u",
                (unsigned int) ni->ac_max_tcp_conn);
    if (ni->ac_block_other_udp_icmp)
        NETLOG("icmp and non-service udp packets will be blocked");
    if (ni->ac_allow_loopback_redirect)
        NETLOG("conections to special IP address allowed to be redirected to the"
               " host loopback address");
out:
    return err;
}

void post_init_cb(void *opaque)
{
    struct nickel *ni = opaque;

    NETLOG5("%s: query_access_policy etc ...", __FUNCTION__);
    query_access_policy(ni);
    query_proxy_config(ni);
    NETLOG5("%s: ... Ok", __FUNCTION__);
}

int ac_post_init(struct nickel *ni)
{
    int ret = 0;

    NETLOG5("%s", __FUNCTION__);
    if (!ni->ac_enabled)
        goto out;
    if (ni_schedule_bh(ni, NULL, post_init_cb, ni) < 0) {
        NETLOG("%s: ni_schedule_bh failed", __FUNCTION__);
        ret = -1;
        goto out;
    }
    ret = lava_init(ni);

out:
    NETLOG5("%s ret %d", __FUNCTION__, ret);
    return ret;
}

void ac_exit(struct nickel *ni)
{
    if (!ni->ac_enabled)
        return;
    NETLOG("%s: exiting", __FUNCTION__);
    lava_exit(ni);
}

void ac_save(QEMUFile *f, struct nickel *ni)
{
    struct net_addr *dns_ips = NULL;
    uint32_t i, nr_dns_ips = 0;

    if (!ni->ac_enabled)
        return;
    NETLOG("%s: saving ac state", __FUNCTION__);
    get_dns_ips(ni, &dns_ips, &nr_dns_ips);
    qemu_put_be32(f, nr_dns_ips);

    for (i = 0; i < nr_dns_ips; i++) {
        qemu_put_be32(f, sizeof(dns_ips[i]));
        qemu_put_buffer(f, (uint8_t *) &dns_ips[i], sizeof(dns_ips[i]));
    }
    NETLOG("%s: %u ips saved", __FUNCTION__, (unsigned int) nr_dns_ips);

    free(dns_ips);
}

int ac_load(QEMUFile *f, struct nickel *ni, int version_id)
{
    int ret = 0;
    struct net_addr *dns_ips = NULL;
    uint32_t i, nr_dns_ips = 0;

    if (!ni->ac_enabled)
        goto out;
    if (version_id < 1)
        goto out;

    NETLOG("%s: loading ac state", __FUNCTION__);
    nr_dns_ips = qemu_get_be32(f);
    if (!nr_dns_ips)
        goto out;
    dns_ips = (struct net_addr *) calloc(1, sizeof(*dns_ips) * nr_dns_ips);
    if (!dns_ips) {
        NETLOG("%s: malloc ERROR! we are doomed", __FUNCTION__);
        ret = -1;
        goto out;
    }

    for (i = 0; i < nr_dns_ips; i++) {
        uint32_t l;

        l = qemu_get_be32(f);
        if (l != sizeof(dns_ips[i])) {
            NETLOG("%s: ERROR - len != sizeof(*dns_ips) - %u != %u !",
                   __FUNCTION__, (unsigned) l, (unsigned) sizeof(dns_ips[i]));
            ret = -1;
            goto out;
        }
        qemu_get_buffer(f, (uint8_t *) &dns_ips[i], l);
    }

    set_dns_ips(ni, dns_ips, nr_dns_ips);
    free(dns_ips);
    NETLOG("%s: %u ips loaded", __FUNCTION__, (unsigned int) nr_dns_ips);
out:
    return ret;
}
