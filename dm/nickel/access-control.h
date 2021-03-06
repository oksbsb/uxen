#ifndef _NICKEL_ACCESS_CONTROL_H_
#define _NICKEL_ACCESS_CONTROL_H_

#include <dm/dict.h>
#include <dm/libnickel.h>
enum AccessPolicy {
    DENY_ALL,
    ALLOW_ALL,
    RESTRICTED
};

#define BAC_HASHSIZE    5
/* copy from dnsproxy */
#define BAC_HASH(n)         ((n) & ((1<<BAC_HASHSIZE)-1))
#define BAC_HASHADDR(ip)    (((ip)->ipv4.s_addr) & ((1<<BAC_HASHSIZE)-1))

struct nickel;
struct ac_host;
struct ac_network;
struct net_addr;

int ac_init(struct nickel *ni);
void ac_exit(struct nickel *ni);
int ac_post_init(struct nickel *ni);
bool ac_proxy_set(struct nickel *ni);
void ac_save(QEMUFile *f, struct nickel *ni);
int ac_load(QEMUFile *f, struct nickel *ni, int version_id);
int ac_tcp_input_syn(struct nickel *ni, struct sockaddr_in saddr,
        struct sockaddr_in daddr);
int ac_udp_input(struct nickel *ni, struct sockaddr_in saddr,
        struct sockaddr_in daddr);
void ac_add_ip_from_dns(struct nickel *ni, const struct net_addr *a);
bool ac_is_dnsname_allowed(struct nickel *ni, const char *name);
bool ac_is_ip_port_allowed(struct nickel *ni, const struct net_addr *addr, uint16_t port);
int ac_check_dns_ips_port(struct nickel *ni, struct net_addr *ips, uint16_t port,
                           char *ret_mask, int len);
#endif
