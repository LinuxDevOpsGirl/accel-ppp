#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>
#include <linux/if.h>

#include "events.h"
#include "list.h"
#include "triton.h"
#include "log.h"
#include "mempool.h"
#include "memdebug.h"
#include "ap_session.h"
#include "ipdb.h"

#include "dhcpv4.h"

#define DHCP_SERV_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC "\x63\x82\x53\x63"


#define BUF_SIZE 4096


static int conf_verbose;
static in_addr_t conf_dns1;
static in_addr_t conf_dns2;

static mempool_t pack_pool;
static mempool_t opt_pool;

static int dhcpv4_read(struct triton_md_handler_t *h);

static struct dhcpv4_iprange *parse_range(const char *str)
{
	unsigned int f1,f2,f3,f4,m,n, mask, start, end, len;
	struct dhcpv4_iprange *r;

	n = sscanf(str, "%u.%u.%u.%u/%u", &f1, &f2, &f3, &f4, &m);
	
	if (n != 5)
		goto parse_err;
	if (f1 > 255)
		goto parse_err;
	if (f2 > 255)
		goto parse_err;
	if (f3 > 255)
		goto parse_err;
	if (f4 > 255)
		goto parse_err;
	if (m == 0 || m > 30)
		goto parse_err;
	
	start = (f1 << 24) | (f2 << 16) | (f3 << 8) | f4;
	mask = ~((1 << (32 - m)) - 1);
	start = start & mask;
	end = start | ~mask;

	len = (end - start - 1) / (8 * sizeof(long)) + 1;

	r = _malloc(sizeof(*r) + len * sizeof(long));
	memset(r, 0, sizeof(*r));
	memset(r->free, 0xff, len * sizeof(long));
	r->routerip = start + 1;
	r->startip = start;
	r->mask = m;
	r->len = len;
	pthread_mutex_init(&r->lock, NULL);

	end -= start;
	r->free[(end - 1) / ( 8 * sizeof(long))] &= (1 << ((end - 1) % (8 * sizeof(long)) + 1)) - 1;
	r->free[0] &= ~3;

	return r;

parse_err:
	log_emerg("dhcpv4: failed to parse range=%s\n", str);
	return NULL;
}

struct dhcpv4_serv *dhcpv4_create(struct triton_context_t *ctx, const char *ifname, const char *opt)
{
	struct dhcpv4_serv *serv;
	int sock, raw_sock;
	struct sockaddr_in addr;
	struct sockaddr_ll ll_addr;
	struct ifreq ifr;
	int f = 1;
	char *str0, *str, *ptr1, *ptr2;
	int end;

	memset(&ifr, 0, sizeof(ifr));

	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(sock_fd, SIOCGIFINDEX, &ifr)) {
		log_error("dhcpv4(%s): ioctl(SIOCGIFINDEX): %s\n", ifname, strerror(errno));
		return NULL;
	}

	raw_sock = socket(AF_PACKET, SOCK_RAW, ntohs(ETH_P_IP));
	if (raw_sock < 0) {
		log_error("dhcpv4: packet socket is not supported by kernel\n");
		return NULL;
	}

	memset(&ll_addr, 0, sizeof(ll_addr));
	ll_addr.sll_family = AF_PACKET;
	ll_addr.sll_ifindex = ifr.ifr_ifindex;
	ll_addr.sll_protocol = ntohs(ETH_P_IP);

	if (bind(raw_sock, (struct sockaddr *)&ll_addr, sizeof(ll_addr))) {
		log_error("dhcpv4(%s): bind: %s\n", ifname, strerror(errno));
		close(raw_sock);
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(DHCP_SERV_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &f, sizeof(f)))
		log_error("setsockopt(SO_REUSEADDR): %s\n", strerror(errno));
	

	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &f, sizeof(f))) {
		log_error("setsockopt(SO_BROADCAST): %s\n", strerror(errno));
		goto out_err;
	}

	if (bind(sock, &addr, sizeof(addr))) {
		log_error("bind: %s\n", strerror(errno));
		goto out_err;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname))) {
		log_error("setsockopt(SO_BINDTODEVICE): %s\n", strerror(errno));
		goto out_err;
	}
	
	if (ioctl(sock, SIOCGIFHWADDR, &ifr)) {
		log_error("dhcpv4(%s): ioctl(SIOCGIFHWADDR): %s\n", ifname, strerror(errno));
		goto out_err;
	}
	
	fcntl(raw_sock, F_SETFL, O_NONBLOCK);
	fcntl(raw_sock, F_SETFD, fcntl(raw_sock, F_GETFD) | FD_CLOEXEC);

	fcntl(sock, F_SETFL, O_NONBLOCK);
	fcntl(sock, F_SETFD, fcntl(sock, F_GETFD) | FD_CLOEXEC);
	
	serv = _malloc(sizeof(*serv));
	memset(serv, 0, sizeof(*serv));

	memcpy(serv->hwaddr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	serv->ctx = ctx;
	serv->hnd.fd = sock;
	serv->hnd.read = dhcpv4_read;
	serv->raw_sock = raw_sock;

	str0 = strchr(opt, ',');
	if (str0) {
		str0 = _strdup(str0 + 1);
		str = str0;
	
		while (1) {
			for (ptr1 = str + 1; *ptr1 && *ptr1 != '='; ptr1++);

			if (!*ptr1)
				break;
		
			*ptr1 = 0;

			for (ptr2 = ++ptr1; *ptr2 && *ptr2 != ','; ptr2++);

			end = *ptr2 == 0;

			if (!end)
				*ptr2 = 0;

			if (ptr2 == ptr1)
				break;

			if (strcmp(str, "range") == 0)
				serv->range = parse_range(ptr1);

			if (end)
				break;

			str = ptr2 + 1;
		}
		
		_free(str0);
	}

	triton_md_register_handler(ctx, &serv->hnd);
	triton_md_enable_handler(&serv->hnd, MD_MODE_READ);

	return serv;

out_err:
	close(raw_sock);
	close(sock);
	return NULL;
}

void dhcpv4_free(struct dhcpv4_serv *serv)
{
	triton_md_unregister_handler(&serv->hnd);
	close(serv->hnd.fd);
	if (serv->range)
		_free(serv->range);
	_free(serv);
}

void dhcpv4_print_packet(struct dhcpv4_packet *pack, void (*print)(const char *fmt, ...))
{
	const char *msg_name[] = {"Discover", "Offer", "Request", "Decline", "Ack", "Nak", "Release", "Inform"};

	print("[DHCPv4 %s xid=%x ", msg_name[pack->msg_type - 1], pack->hdr->xid);

	if (pack->hdr->ciaddr)
		print("ciaddr=%i.%i.%i.%i ",
			pack->hdr->ciaddr & 0xff,
			(pack->hdr->ciaddr >> 8) & 0xff,
			(pack->hdr->ciaddr >> 16) & 0xff,
			(pack->hdr->ciaddr >> 24) & 0xff);
	
	if (pack->hdr->yiaddr)
		print("yiaddr=%i.%i.%i.%i ",
			pack->hdr->yiaddr & 0xff,
			(pack->hdr->yiaddr >> 8) & 0xff,
			(pack->hdr->yiaddr >> 16) & 0xff,
			(pack->hdr->yiaddr >> 24) & 0xff);
	
	if (pack->hdr->siaddr)
		print("siaddr=%i.%i.%i.%i ",
			pack->hdr->siaddr & 0xff,
			(pack->hdr->siaddr >> 8) & 0xff,
			(pack->hdr->siaddr >> 16) & 0xff,
			(pack->hdr->siaddr >> 24) & 0xff);
	
	if (pack->hdr->giaddr)
		print("giaddr=%i.%i.%i.%i ",
			pack->hdr->giaddr & 0xff,
			(pack->hdr->giaddr >> 8) & 0xff,
			(pack->hdr->giaddr >> 16) & 0xff,
			(pack->hdr->giaddr >> 24) & 0xff);
	
	print("chaddr=%02x:%02x:%02x:%02x:%02x:%02x ",
		pack->hdr->chaddr[0],
		pack->hdr->chaddr[1],
		pack->hdr->chaddr[2],
		pack->hdr->chaddr[3],
		pack->hdr->chaddr[4],
		pack->hdr->chaddr[5],
		pack->hdr->chaddr[6]);

	dhcpv4_print_options(pack, print);

	print("]\n");
}

static int parse_opt82(struct dhcpv4_packet *pack, struct dhcpv4_option *opt)
{
	uint8_t *ptr = opt->data;
	uint8_t *endptr = ptr + opt->len;
	int type, len;
	struct dhcpv4_option *opt1;

	while (ptr < endptr) {
		type = *ptr++;
		len = *ptr++;
		if (ptr + len > endptr)
			return -1;
		if (type == 1 || type == 2) {
			opt1 = mempool_alloc(opt_pool);
			if (!opt1) {
				log_emerg("out of memory\n");
				return -1;
			}

			opt1->type = type;
			opt1->len = len;
			opt1->data = ptr;

			if (type == 1)
				pack->agent_circuit_id = opt1;
			else
				pack->agent_remote_id = opt1;
		}

		ptr += len;
	}

	return 0;
}

static int dhcpv4_parse_packet(struct dhcpv4_packet *pack, int len)
{
	struct dhcpv4_option *opt;
	uint8_t *ptr, *endptr = pack->data + len;

	if (len < sizeof(struct dhcpv4_hdr)) {
		if (conf_verbose)
			log_warn("dhcpv4: short packet received\n");
		return -1;
	}

	if (pack->hdr->op != DHCP_OP_REQUEST)
		return -1;
	
	if (pack->hdr->htype != 1)
		return -1;
	
	if (pack->hdr->hlen != 6)
		return -1;

	if (memcmp(pack->hdr->magic, DHCP_MAGIC, 4))
		return -1;

	ptr = pack->data + sizeof(struct dhcpv4_hdr);

	while (ptr < endptr) {
		if (*ptr == 0) {
			ptr++;
			continue;
		}
		
		if (*ptr == 0xff)
			break;

		opt = mempool_alloc(opt_pool);
		if (!opt) {
			log_emerg("out of memory\n");
			return -1;
		}
		memset(opt, 0, sizeof(*opt));
		opt->type = *ptr++;
		opt->len = *ptr++;
		opt->data = ptr;
		ptr += opt->len;

		if (ptr > endptr)
			return -1;

		list_add_tail(&opt->entry, &pack->options);

		if (opt->type == 53)
			pack->msg_type = opt->data[0];
		else if (opt->type == 82)
			parse_opt82(pack, opt);
		else if (opt->type == 50)
			pack->request_ip = *(uint32_t *)opt->data;
		else if (opt->type == 54)
			pack->server_id = *(uint32_t *)opt->data;
	}

	if (pack->msg_type == 0 || pack->msg_type > 8)
		return -1;

	if (dhcpv4_check_options(pack))
		return -1;
	
	/*if (conf_verbose) {
		log_info2("recv ");
		print_packet(pack, log_info2);
	}*/

	return 0;
}

static struct dhcpv4_packet *dhcpv4_packet_alloc()
{
	struct dhcpv4_packet *pack = mempool_alloc(pack_pool);

	if (!pack)
		return NULL;

	memset(pack, 0, sizeof(*pack));

	INIT_LIST_HEAD(&pack->options);

	pack->hdr = (struct dhcpv4_hdr *)pack->data;
	pack->ptr = (uint8_t *)(pack->hdr + 1);

	memcpy(pack->hdr->magic, DHCP_MAGIC, 4);

	return pack;
}

static int dhcpv4_read(struct triton_md_handler_t *h)
{
	struct dhcpv4_packet *pack;
	struct dhcpv4_serv *serv = container_of(h, typeof(*serv), hnd);
	struct sockaddr_in addr;
	socklen_t len;
	int n;

	while (1) {
		pack = dhcpv4_packet_alloc();
		if (!pack) {
			log_emerg("out of memory\n");
			return 1;
		}

		len = sizeof(addr);
		n = recvfrom(h->fd, pack->data, BUF_SIZE, 0, &addr, &len);
		if (n == -1) {
			mempool_free(pack);
			if (errno == EAGAIN)
				return 0;
			log_error("dhcpv4: recv: %s\n", strerror(errno));
			continue;
		}

		if (dhcpv4_parse_packet(pack, n)) {
			dhcpv4_packet_free(pack);
			continue;
		}

		if (serv->recv)
			serv->recv(serv, pack);
	}
}

uint16_t ip_csum(uint16_t *buf, int len)
{
	uint32_t sum=0;
	int i;
    
	for (i=0; i < len; i += 2)
		sum += *buf++;
	
	// take only 16 bits out of the 32 bit sum and add up the carries
	while (sum >> 16)
	  sum = (sum & 0xffff) + (sum >> 16);

	// one's complement the result
	sum = ~sum;
	
	return sum & 0xffff;
}


static int dhcpv4_send_raw(struct dhcpv4_serv *serv, struct dhcpv4_packet *pack, in_addr_t saddr, in_addr_t daddr)
{
	uint8_t hdr[sizeof(struct ether_header) + sizeof(struct iphdr) + sizeof(struct udphdr)];
	struct ether_header *eth = (struct ether_header *)hdr;
	struct iphdr *ip = (struct iphdr *)(eth + 1);
	struct udphdr *udp = (struct udphdr *)(ip + 1);
	int len = pack->ptr - pack->data;
	struct iovec iov[2];

	memcpy(eth->ether_dhost, pack->hdr->chaddr, ETH_ALEN);
	memcpy(eth->ether_shost, serv->hwaddr, ETH_ALEN);
	eth->ether_type = htons(ETH_P_IP);

	ip->ihl = 5;
	ip->version = 4;
	ip->tos = 0x10;
	ip->tot_len = ntohs(sizeof(*ip) + sizeof(*udp) + len);
	ip->id = 0;
	ip->frag_off = 0;
	ip->ttl = 128;
	ip->protocol = IPPROTO_UDP;
	ip->check = 0;
	ip->saddr = saddr;
	ip->daddr = daddr;
	ip->check = ip_csum((uint16_t *)ip, 20);

	udp->source = ntohs(DHCP_SERV_PORT);
	udp->dest = ntohs(DHCP_CLIENT_PORT);
	udp->len = htons(sizeof(*udp) + len);
	udp->check = 0;

	iov[0].iov_base = hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = pack->data;
	iov[1].iov_len = len;

	len = writev(serv->raw_sock, iov, 2);

	if (len < 0)
		return -1;
	
	return 0;
}

static int dhcpv4_send_udp(struct dhcpv4_serv *serv, struct dhcpv4_packet *pack)
{
	struct sockaddr_in addr;
	int n;
	int len = pack->ptr - pack->data;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(DHCP_CLIENT_PORT);
	addr.sin_addr.s_addr = pack->hdr->giaddr;

	n = sendto(serv->hnd.fd, pack->data, len, 0, (struct sockaddr *)&addr, sizeof(addr));
	if (n != len)
		return -1;
	
	return 0;
}

static int dhcpv4_send(struct dhcpv4_serv *serv, struct dhcpv4_packet *pack, in_addr_t saddr, in_addr_t daddr)
{
	if (pack->hdr->giaddr)
		return dhcpv4_send_udp(serv, pack);
	
	return dhcpv4_send_raw(serv, pack, saddr, daddr);
}

void dhcpv4_packet_free(struct dhcpv4_packet *pack)
{
	struct dhcpv4_option *opt;

	while (!list_empty(&pack->options)) {
		opt = list_entry(pack->options.next, typeof(*opt), entry);
		list_del(&opt->entry);
		mempool_free(opt);
	}

	if (pack->agent_circuit_id)
		mempool_free(pack->agent_circuit_id);
	
	if (pack->agent_remote_id)
		mempool_free(pack->agent_remote_id);
	
	mempool_free(pack);
}

int dhcpv4_packet_add_opt(struct dhcpv4_packet *pack, int type, const void *data, int len)
{
	struct dhcpv4_option *opt = mempool_alloc(opt_pool);

	if (!opt) {
		log_emerg("out of memory\n");
		return -1;
	}

	*pack->ptr++ = type;
	*pack->ptr++ = len;

	opt->type = type;
	opt->len = len;
	opt->data = pack->ptr; 
	pack->ptr += len;

	memcpy(opt->data, data, len);

	list_add_tail(&opt->entry, &pack->options);

	return 0;
}

int dhcpv4_send_reply(int msg_type, struct dhcpv4_serv *serv, struct dhcpv4_packet *req, uint32_t yiaddr, uint32_t siaddr, uint32_t mask, int lease_time)
{
	struct dhcpv4_packet *pack;
	int val, r;
	struct dns {
		in_addr_t dns1;
		in_addr_t dns2;
	} dns;
	
	pack = dhcpv4_packet_alloc();
	if (!pack) {
		log_emerg("out of memory\n");
		return -1;
	}

	memcpy(pack->hdr, req->hdr, sizeof(*req->hdr));

	pack->hdr->op = DHCP_OP_REPLY;
	pack->hdr->ciaddr = 0;
	pack->hdr->yiaddr = yiaddr;
	if (msg_type == DHCPOFFER)
		pack->hdr->siaddr = siaddr;
	else
		pack->hdr->siaddr = 0;

	if (dhcpv4_packet_add_opt(pack, 53, &msg_type, 1))
		goto out_err;
	
	if (dhcpv4_packet_add_opt(pack, 54, &siaddr, 4))
		goto out_err;
	
	val = ntohl(lease_time);
	if (dhcpv4_packet_add_opt(pack, 51, &val, 4))
		goto out_err;

	if (dhcpv4_packet_add_opt(pack, 3, &siaddr, 4))
		goto out_err;
	
	val = htonl(~((1 << (32 - mask)) - 1));
	if (dhcpv4_packet_add_opt(pack, 1, &val, 4))
		goto out_err;
	
	if (conf_dns1 && conf_dns2) {
		dns.dns1 = conf_dns1;
		dns.dns2 = conf_dns2;
		if (dhcpv4_packet_add_opt(pack, 6, &dns, 8))
			goto out_err;
	} else if (conf_dns1) {
		if (dhcpv4_packet_add_opt(pack, 6, &conf_dns1, 4))
			goto out_err;
	}

	*pack->ptr++ = 255;

	if (conf_verbose) {
		pack->msg_type = msg_type;
		log_ppp_info2("send ");
		dhcpv4_print_packet(pack, log_ppp_info2);
	}

	r = dhcpv4_send(serv, pack, siaddr, yiaddr);

	dhcpv4_packet_free(pack);

	return r;

out_err:
	dhcpv4_packet_free(pack);
	return -1;
}

int dhcpv4_send_nak(struct dhcpv4_serv *serv, struct dhcpv4_packet *req)
{
	struct dhcpv4_packet *pack;
	int val, r;
	
	pack = dhcpv4_packet_alloc();
	if (!pack) {
		log_emerg("out of memory\n");
		return -1;
	}

	memcpy(pack->hdr, req->hdr, sizeof(*req->hdr));

	pack->hdr->op = DHCP_OP_REPLY;
	pack->hdr->ciaddr = 0;
	pack->hdr->yiaddr = 0;
	pack->hdr->siaddr = 0;

	val = DHCPNAK;
	if (dhcpv4_packet_add_opt(pack, 53, &val, 1))
		goto out_err;
		
	*pack->ptr++ = 255;

	if (conf_verbose) {
		pack->msg_type = DHCPNAK;
		log_info2("send ");
		dhcpv4_print_packet(pack, log_info2);
	}

	r = dhcpv4_send(serv, pack, 0, 0xffffffff);

	dhcpv4_packet_free(pack);

	return r;

out_err:
	dhcpv4_packet_free(pack);
	return -1;

	return 0;
}

int dhcpv4_get_ip(struct dhcpv4_serv *serv, uint32_t *yiaddr, uint32_t *siaddr, int *mask)
{
	int i, k;

	if (!serv->range)
		return 0;
	
	pthread_mutex_lock(&serv->range->lock);

	while (1) {
		for (i = serv->range->pos; i < serv->range->len; i++) {
			k = ffsl(serv->range->free[i]);
			if (k) {
				serv->range->free[i] &= ~(1 << (k - 1));
				serv->range->pos = i;
				pthread_mutex_unlock(&serv->range->lock);
				*yiaddr = htonl(serv->range->startip + i * 8 * sizeof(long) + k - 1);
				*siaddr = htonl(serv->range->routerip);
				*mask = serv->range->mask;
				return 1;
			}
		}

		if (serv->range->pos == 0)
			break;
	
		serv->range->pos = 0;
	}

	pthread_mutex_unlock(&serv->range->lock);
	return 0;
}

void dhcpv4_put_ip(struct dhcpv4_serv *serv, uint32_t ip)
{
	int n = ntohl(ip) - serv->range->startip;
	pthread_mutex_lock(&serv->range->lock);
	serv->range->free[n / (8 * sizeof(long))] |= 1 << (n % (8 * sizeof(long)));
	pthread_mutex_unlock(&serv->range->lock);
}

static void load_config()
{
	const char *opt; 

	opt = conf_get_opt("ipoe", "verbose");
	if (opt)
		conf_verbose = atoi(opt);

	opt = conf_get_opt("dns", "dns1");
	if (opt)
		conf_dns1 = inet_addr(opt);
	
	opt = conf_get_opt("dns", "dns2");
	if (opt)
		conf_dns2 = inet_addr(opt);
}

static void init()
{
	pack_pool = mempool_create(BUF_SIZE + sizeof(struct dhcpv4_packet));
	opt_pool = mempool_create(sizeof(struct dhcpv4_option));

	load_config();

	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(100, init);