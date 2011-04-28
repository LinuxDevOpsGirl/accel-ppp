#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "ppp.h"
#include "events.h"
#include "ppp_lcp.h"
#include "log.h"

#include "ppp_auth.h"

#include "memdebug.h"

static LIST_HEAD(auth_handlers);
static int extra_opt_len = 0;

static struct lcp_option_t *auth_init(struct ppp_lcp_t *lcp);
static void auth_free(struct ppp_lcp_t *lcp, struct lcp_option_t *opt);
static int auth_send_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int auth_recv_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int auth_recv_conf_nak(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int auth_recv_conf_rej(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static int auth_recv_conf_ack(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr);
static void auth_print(void (*print)(const char *fmt,...), struct lcp_option_t*, uint8_t *ptr);

static struct ppp_layer_data_t *auth_layer_init(struct ppp_t*);
static int auth_layer_start(struct ppp_layer_data_t *);
static void auth_layer_finish(struct ppp_layer_data_t *);
static void auth_layer_free(struct ppp_layer_data_t *);

struct auth_option_t
{
	struct lcp_option_t opt;
	struct list_head auth_list;
	struct auth_data_t *auth;
	struct auth_data_t *peer_auth;
	int started:1;
};

struct auth_layer_data_t
{
	struct ppp_layer_data_t ld;
	struct auth_option_t auth_opt;
	struct ppp_t *ppp;
};

static struct lcp_option_handler_t auth_opt_hnd = 
{
	.init = auth_init,
	.send_conf_req = auth_send_conf_req,
	.send_conf_nak = auth_send_conf_req,
	.recv_conf_req = auth_recv_conf_req,
	.recv_conf_nak = auth_recv_conf_nak,
	.recv_conf_rej = auth_recv_conf_rej,
	.recv_conf_ack = auth_recv_conf_ack,
	.free = auth_free,
	.print = auth_print,
};

static struct ppp_layer_t auth_layer = 
{
	.init = auth_layer_init,
	.start = auth_layer_start,
	.finish = auth_layer_finish,
	.free = auth_layer_free,
};

static struct lcp_option_t *auth_init(struct ppp_lcp_t *lcp)
{
	struct ppp_auth_handler_t *h;
	struct auth_data_t *d;
	struct auth_layer_data_t *ad;

	ad = container_of(ppp_find_layer_data(lcp->ppp, &auth_layer), typeof(*ad), ld);

	ad->auth_opt.opt.id = CI_AUTH;
	ad->auth_opt.opt.len = 4 + extra_opt_len;

	INIT_LIST_HEAD(&ad->auth_opt.auth_list);

	list_for_each_entry(h, &auth_handlers, entry) {
		d = h->init(lcp->ppp);
		d->h = h;
		list_add_tail(&d->entry, &ad->auth_opt.auth_list);
	}

	return &ad->auth_opt.opt;
}

static void auth_free(struct ppp_lcp_t *lcp, struct lcp_option_t *opt)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);
	struct auth_data_t *d;

	if (auth_opt->started && auth_opt->auth) {
		auth_opt->auth->h->finish(lcp->ppp, auth_opt->auth);
		auth_opt->started = 0;
	}

	while(!list_empty(&auth_opt->auth_list)) {
		d = list_entry(auth_opt->auth_list.next, typeof(*d), entry);
		list_del(&d->entry);
		d->h->free(lcp->ppp, d);
	}
}

static int auth_send_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);
	struct lcp_opt16_t *opt16 = (struct lcp_opt16_t*)ptr;
	struct auth_data_t *d;
	int n;

	if (list_empty(&auth_opt->auth_list))
		return 0;

	if (!auth_opt->auth || auth_opt->auth->state == LCP_OPT_NAK) {
		list_for_each_entry(d, &auth_opt->auth_list, entry) {
			if (d->state == LCP_OPT_NAK || d->state == LCP_OPT_REJ)
				continue;
			auth_opt->auth = d;
			break;
		}
	}

	opt16->hdr.id = CI_AUTH;
	opt16->val = htons(auth_opt->auth->proto);
	n = auth_opt->auth->h->send_conf_req(lcp->ppp, auth_opt->auth, (uint8_t*)(opt16 + 1));
	opt16->hdr.len = 4 + n;

	return 4 + n;
}

static int auth_recv_conf_req(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt,typeof(*auth_opt),opt);
	struct lcp_opt16_t *opt16 = (struct lcp_opt16_t*)ptr;
	struct auth_data_t *d;
	int r;

	if (list_empty(&auth_opt->auth_list))
		return LCP_OPT_REJ;
		
	if (!ptr)
		return LCP_OPT_ACK;


	list_for_each_entry(d, &auth_opt->auth_list, entry) {
		if (d->proto == ntohs(opt16->val)) {
			r = d->h->recv_conf_req(lcp->ppp, d, (uint8_t*)(opt16 + 1));
			if (r == LCP_OPT_FAIL)
				return LCP_OPT_FAIL;
			if (r == LCP_OPT_REJ)
				break;
			auth_opt->peer_auth = d;
			return r;
		}
	}
		
	list_for_each_entry(d, &auth_opt->auth_list, entry) {
		if (d->state != LCP_OPT_NAK) {
			auth_opt->peer_auth = d;
			return LCP_OPT_NAK;
		}
	}

	log_ppp_error("cann't negotiate authentication type\n");
	return LCP_OPT_FAIL;
}

static int auth_recv_conf_ack(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);

	auth_opt->peer_auth = NULL;

	return 0;
}

static int auth_recv_conf_nak(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);
	struct auth_data_t *d;

	if (!auth_opt->auth) {
		log_ppp_error("auth: unexcepcted configure-nak\n");
		return -1;
	}
	auth_opt->auth->state = LCP_OPT_NAK;
	if (auth_opt->peer_auth)
		auth_opt->auth = auth_opt->peer_auth;
	
	list_for_each_entry(d, &auth_opt->auth_list, entry) {
		if (d->state != LCP_OPT_NAK)
			return 0;
	}

	log_ppp_error("cann't negotiate authentication type\n");
	return -1;
}

static int auth_recv_conf_rej(struct ppp_lcp_t *lcp, struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);
	struct auth_data_t *d;

	if (!auth_opt->auth) {
		log_ppp_error("auth: unexcepcted configure-reject\n");
		return -1;
	}
	
	auth_opt->auth->state = LCP_OPT_NAK;
	if (auth_opt->peer_auth)
		auth_opt->auth = auth_opt->peer_auth;
	
	list_for_each_entry(d, &auth_opt->auth_list, entry) {
		if (d->state != LCP_OPT_NAK)
			return 0;
	}

	log_ppp_error("cann't negotiate authentication type\n");
	return -1;
}

static void auth_print(void (*print)(const char *fmt,...), struct lcp_option_t *opt, uint8_t *ptr)
{
	struct auth_option_t *auth_opt = container_of(opt, typeof(*auth_opt), opt);
	struct lcp_opt16_t *opt16 = (struct lcp_opt16_t*)ptr;
	struct auth_data_t *d;

	if (ptr) {
		list_for_each_entry(d, &auth_opt->auth_list, entry) {
			if (d->proto == ntohs(opt16->val) && (!d->h->check || d->h->check((uint8_t *)(opt16 + 1))))
				goto print_d;
		}

		print("<auth %02x>", ntohs(opt16->val));
		return;
	} else if (auth_opt->auth)
		d = auth_opt->auth;
	else
		return;

print_d:
	print("<auth %s>", d->h->name);
}

static struct ppp_layer_data_t *auth_layer_init(struct ppp_t *ppp)
{
	struct auth_layer_data_t *ad = _malloc(sizeof(*ad));

	log_ppp_debug("auth_layer_init\n");
	
	memset(ad, 0, sizeof(*ad));

	ad->ppp = ppp;

	return &ad->ld;
}

static int auth_layer_start(struct ppp_layer_data_t *ld)
{
	struct auth_layer_data_t *ad = container_of(ld,typeof(*ad),ld);
	
	log_ppp_debug("auth_layer_start\n");
		
	if (ad->auth_opt.auth) {
		ad->auth_opt.started = 1;
		ad->auth_opt.auth->h->start(ad->ppp, ad->auth_opt.auth);
	} else {
		log_ppp_debug("auth_layer_started\n");
		ppp_layer_started(ad->ppp, ld);
	}
	
	return 0;
}

static void auth_layer_finish(struct ppp_layer_data_t *ld)
{
	struct auth_layer_data_t *ad = container_of(ld, typeof(*ad), ld);
	
	log_ppp_debug("auth_layer_finish\n");
	
	if (ad->auth_opt.auth)
		ad->auth_opt.auth->h->finish(ad->ppp, ad->auth_opt.auth);
	
	ad->auth_opt.started = 0;

	log_ppp_debug("auth_layer_finished\n");
	ppp_layer_finished(ad->ppp, ld);
}

static void auth_layer_free(struct ppp_layer_data_t *ld)
{
	struct auth_layer_data_t *ad = container_of(ld, typeof(*ad), ld);

	log_ppp_debug("auth_layer_free\n");

	_free(ad);
}

static void ppp_terminate_sec(struct ppp_t *ppp)
{
	ppp_terminate(ppp, TERM_NAS_REQUEST, 0);
}

int __export ppp_auth_successed(struct ppp_t *ppp, char *username)
{
	struct ppp_t *p;
	struct auth_layer_data_t *ad = container_of(ppp_find_layer_data(ppp, &auth_layer), typeof(*ad), ld);
	
	pthread_rwlock_rdlock(&ppp_lock);
	list_for_each_entry(p, &ppp_list, entry) {
		if (p->username && !strcmp(p->username, username)) {
			if (conf_single_session == 0) {
				pthread_rwlock_unlock(&ppp_lock);
				log_ppp_info1("%s: second session denied\n", username);
				return -1;
			} else {
				if (conf_single_session == 1)
					triton_context_call(p->ctrl->ctx, (triton_event_func)ppp_terminate_sec, p);
				break;
			}
		}
	}
	pthread_rwlock_unlock(&ppp_lock);

	pthread_rwlock_wrlock(&ppp_lock);
	ppp->username = username;
	pthread_rwlock_unlock(&ppp_lock);

	log_ppp_debug("auth_layer_started\n");
	ppp_layer_started(ppp, &ad->ld);
	log_ppp_info1("%s: authentication successed\n", username);
	triton_event_fire(EV_PPP_AUTHORIZED, ppp);

	return 0;
}

void __export ppp_auth_failed(struct ppp_t *ppp, const char *username)
{
	if (username)
		log_ppp_info1("%s: authentication failed\n", username);
	else
		log_ppp_info1("authentication failed\n");
	ppp_terminate(ppp, TERM_AUTH_ERROR, 0);
}

int __export ppp_auth_register_handler(struct ppp_auth_handler_t *h)
{
	list_add_tail(&h->entry, &auth_handlers);
	return 0;
}

int __export ppp_auth_restart(struct ppp_t *ppp)
{
	struct auth_layer_data_t *ad = container_of(ppp_find_layer_data(ppp, &auth_layer), typeof(*ad), ld);
	log_ppp_debug("ppp_auth_restart\n");

	if (!ad->auth_opt.auth->h->restart)
		return -1;
	
	if (ad->auth_opt.auth->h->restart(ppp, ad->auth_opt.auth))
		return -1;
	
	return 0;
}

static void __init ppp_auth_init()
{
	ppp_register_layer("auth", &auth_layer);
	lcp_option_register(&auth_opt_hnd);
}
