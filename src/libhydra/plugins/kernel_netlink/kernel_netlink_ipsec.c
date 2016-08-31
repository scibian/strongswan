/*
 * Copyright (C) 2006-2010 Tobias Brunner
 * Copyright (C) 2005-2009 Martin Willi
 * Copyright (C) 2008 Andreas Steffen
 * Copyright (C) 2006-2007 Fabian Hartmann, Noah Heusser
 * Copyright (C) 2006 Daniel Roethlisberger
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <linux/ipsec.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/xfrm.h>
#include <linux/udp.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "kernel_netlink_ipsec.h"
#include "kernel_netlink_shared.h"

#include <hydra.h>
#include <debug.h>
#include <threading/thread.h>
#include <threading/mutex.h>
#include <utils/hashtable.h>
#include <processing/jobs/callback_job.h>

/** required for Linux 2.6.26 kernel and later */
#ifndef XFRM_STATE_AF_UNSPEC
#define XFRM_STATE_AF_UNSPEC	32
#endif

/** from linux/in.h */
#ifndef IP_XFRM_POLICY
#define IP_XFRM_POLICY 17
#endif

/* missing on uclibc */
#ifndef IPV6_XFRM_POLICY
#define IPV6_XFRM_POLICY 34
#endif /*IPV6_XFRM_POLICY*/

/** default priority of installed policies */
#define PRIO_LOW 1024
#define PRIO_HIGH 512

/** default replay window size, if not set using charon.replay_window */
#define DEFAULT_REPLAY_WINDOW 32

/**
 * map the limit for bytes and packets to XFRM_INF per default
 */
#define XFRM_LIMIT(x) ((x) == 0 ? XFRM_INF : (x))

/**
 * Create ORable bitfield of XFRM NL groups
 */
#define XFRMNLGRP(x) (1<<(XFRMNLGRP_##x-1))

/**
 * returns a pointer to the first rtattr following the nlmsghdr *nlh and the
 * 'usual' netlink data x like 'struct xfrm_usersa_info'
 */
#define XFRM_RTA(nlh, x) ((struct rtattr*)(NLMSG_DATA(nlh) + NLMSG_ALIGN(sizeof(x))))
/**
 * returns a pointer to the next rtattr following rta.
 * !!! do not use this to parse messages. use RTA_NEXT and RTA_OK instead !!!
 */
#define XFRM_RTA_NEXT(rta) ((struct rtattr*)(((char*)(rta)) + RTA_ALIGN((rta)->rta_len)))
/**
 * returns the total size of attached rta data
 * (after 'usual' netlink data x like 'struct xfrm_usersa_info')
 */
#define XFRM_PAYLOAD(nlh, x) NLMSG_PAYLOAD(nlh, sizeof(x))

typedef struct kernel_algorithm_t kernel_algorithm_t;

/**
 * Mapping of IKEv2 kernel identifier to linux crypto API names
 */
struct kernel_algorithm_t {
	/**
	 * Identifier specified in IKEv2
	 */
	int ikev2;

	/**
	 * Name of the algorithm in linux crypto API
	 */
	char *name;
};

ENUM(xfrm_msg_names, XFRM_MSG_NEWSA, XFRM_MSG_MAPPING,
	"XFRM_MSG_NEWSA",
	"XFRM_MSG_DELSA",
	"XFRM_MSG_GETSA",
	"XFRM_MSG_NEWPOLICY",
	"XFRM_MSG_DELPOLICY",
	"XFRM_MSG_GETPOLICY",
	"XFRM_MSG_ALLOCSPI",
	"XFRM_MSG_ACQUIRE",
	"XFRM_MSG_EXPIRE",
	"XFRM_MSG_UPDPOLICY",
	"XFRM_MSG_UPDSA",
	"XFRM_MSG_POLEXPIRE",
	"XFRM_MSG_FLUSHSA",
	"XFRM_MSG_FLUSHPOLICY",
	"XFRM_MSG_NEWAE",
	"XFRM_MSG_GETAE",
	"XFRM_MSG_REPORT",
	"XFRM_MSG_MIGRATE",
	"XFRM_MSG_NEWSADINFO",
	"XFRM_MSG_GETSADINFO",
	"XFRM_MSG_NEWSPDINFO",
	"XFRM_MSG_GETSPDINFO",
	"XFRM_MSG_MAPPING"
);

ENUM(xfrm_attr_type_names, XFRMA_UNSPEC, XFRMA_KMADDRESS,
	"XFRMA_UNSPEC",
	"XFRMA_ALG_AUTH",
	"XFRMA_ALG_CRYPT",
	"XFRMA_ALG_COMP",
	"XFRMA_ENCAP",
	"XFRMA_TMPL",
	"XFRMA_SA",
	"XFRMA_POLICY",
	"XFRMA_SEC_CTX",
	"XFRMA_LTIME_VAL",
	"XFRMA_REPLAY_VAL",
	"XFRMA_REPLAY_THRESH",
	"XFRMA_ETIMER_THRESH",
	"XFRMA_SRCADDR",
	"XFRMA_COADDR",
	"XFRMA_LASTUSED",
	"XFRMA_POLICY_TYPE",
	"XFRMA_MIGRATE",
	"XFRMA_ALG_AEAD",
	"XFRMA_KMADDRESS"
);

#define END_OF_LIST -1

/**
 * Algorithms for encryption
 */
static kernel_algorithm_t encryption_algs[] = {
/*	{ENCR_DES_IV64,				"***"				}, */
	{ENCR_DES,					"des"				},
	{ENCR_3DES,					"des3_ede"			},
/*	{ENCR_RC5,					"***"				}, */
/*	{ENCR_IDEA,					"***"				}, */
	{ENCR_CAST,					"cast128"			},
	{ENCR_BLOWFISH,				"blowfish"			},
/*	{ENCR_3IDEA,				"***"				}, */
/*	{ENCR_DES_IV32,				"***"				}, */
	{ENCR_NULL,					"cipher_null"		},
	{ENCR_AES_CBC,				"aes"				},
	{ENCR_AES_CTR,				"rfc3686(ctr(aes))"	},
	{ENCR_AES_CCM_ICV8,			"rfc4309(ccm(aes))"	},
	{ENCR_AES_CCM_ICV12,		"rfc4309(ccm(aes))"	},
	{ENCR_AES_CCM_ICV16,		"rfc4309(ccm(aes))"	},
	{ENCR_AES_GCM_ICV8,			"rfc4106(gcm(aes))"	},
	{ENCR_AES_GCM_ICV12,		"rfc4106(gcm(aes))"	},
	{ENCR_AES_GCM_ICV16,		"rfc4106(gcm(aes))"	},
	{ENCR_NULL_AUTH_AES_GMAC,	"rfc4543(gcm(aes))"	},
	{ENCR_CAMELLIA_CBC,			"cbc(camellia)"		},
/*	{ENCR_CAMELLIA_CTR,			"***"				}, */
/*	{ENCR_CAMELLIA_CCM_ICV8,	"***"				}, */
/*	{ENCR_CAMELLIA_CCM_ICV12,	"***"				}, */
/*	{ENCR_CAMELLIA_CCM_ICV16,	"***"				}, */
	{ENCR_SERPENT_CBC,			"serpent"			},
	{ENCR_TWOFISH_CBC,			"twofish"			},
	{END_OF_LIST,				NULL				}
};

/**
 * Algorithms for integrity protection
 */
static kernel_algorithm_t integrity_algs[] = {
	{AUTH_HMAC_MD5_96,			"md5"				},
	{AUTH_HMAC_SHA1_96,			"sha1"				},
	{AUTH_HMAC_SHA2_256_96,		"sha256"			},
	{AUTH_HMAC_SHA2_256_128,	"hmac(sha256)"		},
	{AUTH_HMAC_SHA2_384_192,	"hmac(sha384)"		},
	{AUTH_HMAC_SHA2_512_256,	"hmac(sha512)"		},
/*	{AUTH_DES_MAC,				"***"				}, */
/*	{AUTH_KPDK_MD5,				"***"				}, */
	{AUTH_AES_XCBC_96,			"xcbc(aes)"			},
	{END_OF_LIST,				NULL				}
};

/**
 * Algorithms for IPComp
 */
static kernel_algorithm_t compression_algs[] = {
/*	{IPCOMP_OUI,				"***"				}, */
	{IPCOMP_DEFLATE,			"deflate"			},
	{IPCOMP_LZS,				"lzs"				},
	{IPCOMP_LZJH,				"lzjh"				},
	{END_OF_LIST,				NULL				}
};

/**
 * Look up a kernel algorithm name and its key size
 */
static char* lookup_algorithm(kernel_algorithm_t *list, int ikev2)
{
	while (list->ikev2 != END_OF_LIST)
	{
		if (list->ikev2 == ikev2)
		{
			return list->name;
		}
		list++;
	}
	return NULL;
}

typedef struct route_entry_t route_entry_t;

/**
 * installed routing entry
 */
struct route_entry_t {
	/** Name of the interface the route is bound to */
	char *if_name;

	/** Source ip of the route */
	host_t *src_ip;

	/** gateway for this route */
	host_t *gateway;

	/** Destination net */
	chunk_t dst_net;

	/** Destination net prefixlen */
	u_int8_t prefixlen;
};

/**
 * destroy an route_entry_t object
 */
static void route_entry_destroy(route_entry_t *this)
{
	free(this->if_name);
	this->src_ip->destroy(this->src_ip);
	DESTROY_IF(this->gateway);
	chunk_free(&this->dst_net);
	free(this);
}

typedef struct policy_entry_t policy_entry_t;

/**
 * installed kernel policy.
 */
struct policy_entry_t {

	/** direction of this policy: in, out, forward */
	u_int8_t direction;

	/** parameters of installed policy */
	struct xfrm_selector sel;

	/** optional mark */
	u_int32_t mark;

	/** associated route installed for this policy */
	route_entry_t *route;

	/** by how many CHILD_SA's this policy is used */
	u_int refcount;
};

/**
 * Hash function for policy_entry_t objects
 */
static u_int policy_hash(policy_entry_t *key)
{
	chunk_t chunk = chunk_create((void*)&key->sel,
							sizeof(struct xfrm_selector) + sizeof(u_int32_t));
	return chunk_hash(chunk);
}

/**
 * Equality function for policy_entry_t objects
 */
static bool policy_equals(policy_entry_t *key, policy_entry_t *other_key)
{
	return memeq(&key->sel, &other_key->sel,
				 sizeof(struct xfrm_selector) + sizeof(u_int32_t)) &&
		   key->direction == other_key->direction;
}

typedef struct private_kernel_netlink_ipsec_t private_kernel_netlink_ipsec_t;

/**
 * Private variables and functions of kernel_netlink class.
 */
struct private_kernel_netlink_ipsec_t {
	/**
	 * Public part of the kernel_netlink_t object.
	 */
	kernel_netlink_ipsec_t public;

	/**
	 * mutex to lock access to various lists
	 */
	mutex_t *mutex;

	/**
	 * Hash table of installed policies (policy_entry_t)
	 */
	hashtable_t *policies;

	/**
	 * job receiving netlink events
	 */
	callback_job_t *job;

	/**
	 * Netlink xfrm socket (IPsec)
	 */
	netlink_socket_t *socket_xfrm;

	/**
	 * netlink xfrm socket to receive acquire and expire events
	 */
	int socket_xfrm_events;

	/**
	 * whether to install routes along policies
	 */
	bool install_routes;

	/**
	 * Size of the replay window, in packets
	 */
	u_int32_t replay_window;

	/**
	 * Size of the replay window bitmap, in bytes
	 */
	u_int32_t replay_bmp;
};

/**
 * convert the general ipsec mode to the one defined in xfrm.h
 */
static u_int8_t mode2kernel(ipsec_mode_t mode)
{
	switch (mode)
	{
		case MODE_TRANSPORT:
			return XFRM_MODE_TRANSPORT;
		case MODE_TUNNEL:
			return XFRM_MODE_TUNNEL;
		case MODE_BEET:
			return XFRM_MODE_BEET;
		default:
			return mode;
	}
}

/**
 * convert a host_t to a struct xfrm_address
 */
static void host2xfrm(host_t *host, xfrm_address_t *xfrm)
{
	chunk_t chunk = host->get_address(host);
	memcpy(xfrm, chunk.ptr, min(chunk.len, sizeof(xfrm_address_t)));
}

/**
 * convert a struct xfrm_address to a host_t
 */
static host_t* xfrm2host(int family, xfrm_address_t *xfrm, u_int16_t port)
{
	chunk_t chunk;

	switch (family)
	{
		case AF_INET:
			chunk = chunk_create((u_char*)&xfrm->a4, sizeof(xfrm->a4));
			break;
		case AF_INET6:
			chunk = chunk_create((u_char*)&xfrm->a6, sizeof(xfrm->a6));
			break;
		default:
			return NULL;
	}
	return host_create_from_chunk(family, chunk, ntohs(port));
}

/**
 * convert a traffic selector address range to subnet and its mask.
 */
static void ts2subnet(traffic_selector_t* ts,
					  xfrm_address_t *net, u_int8_t *mask)
{
	host_t *net_host;
	chunk_t net_chunk;

	ts->to_subnet(ts, &net_host, mask);
	net_chunk = net_host->get_address(net_host);
	memcpy(net, net_chunk.ptr, net_chunk.len);
	net_host->destroy(net_host);
}

/**
 * convert a traffic selector port range to port/portmask
 */
static void ts2ports(traffic_selector_t* ts,
					 u_int16_t *port, u_int16_t *mask)
{
	/* linux does not seem to accept complex portmasks. Only
	 * any or a specific port is allowed. We set to any, if we have
	 * a port range, or to a specific, if we have one port only.
	 */
	u_int16_t from, to;

	from = ts->get_from_port(ts);
	to = ts->get_to_port(ts);

	if (from == to)
	{
		*port = htons(from);
		*mask = ~0;
	}
	else
	{
		*port = 0;
		*mask = 0;
	}
}

/**
 * convert a pair of traffic_selectors to a xfrm_selector
 */
static struct xfrm_selector ts2selector(traffic_selector_t *src,
										traffic_selector_t *dst)
{
	struct xfrm_selector sel;

	memset(&sel, 0, sizeof(sel));
	sel.family = (src->get_type(src) == TS_IPV4_ADDR_RANGE) ? AF_INET : AF_INET6;
	/* src or dest proto may be "any" (0), use more restrictive one */
	sel.proto = max(src->get_protocol(src), dst->get_protocol(dst));
	ts2subnet(dst, &sel.daddr, &sel.prefixlen_d);
	ts2subnet(src, &sel.saddr, &sel.prefixlen_s);
	ts2ports(dst, &sel.dport, &sel.dport_mask);
	ts2ports(src, &sel.sport, &sel.sport_mask);
	sel.ifindex = 0;
	sel.user = 0;

	return sel;
}

/**
 * convert a xfrm_selector to a src|dst traffic_selector
 */
static traffic_selector_t* selector2ts(struct xfrm_selector *sel, bool src)
{
	u_char *addr;
	u_int8_t prefixlen;
	u_int16_t port = 0;
	host_t *host = NULL;

	if (src)
	{
		addr = (u_char*)&sel->saddr;
		prefixlen = sel->prefixlen_s;
		if (sel->sport_mask)
		{
			port = htons(sel->sport);
		}
	}
	else
	{
		addr = (u_char*)&sel->daddr;
		prefixlen = sel->prefixlen_d;
		if (sel->dport_mask)
		{
			port = htons(sel->dport);
		}
	}

	/* The Linux 2.6 kernel does not set the selector's family field,
	 * so as a kludge we additionally test the prefix length.
	 */
	if (sel->family == AF_INET || sel->prefixlen_s == 32)
	{
		host = host_create_from_chunk(AF_INET, chunk_create(addr, 4), 0);
	}
	else if (sel->family == AF_INET6 || sel->prefixlen_s == 128)
	{
		host = host_create_from_chunk(AF_INET6, chunk_create(addr, 16), 0);
	}

	if (host)
	{
		return traffic_selector_create_from_subnet(host, prefixlen,
												   sel->proto, port);
	}
	return NULL;
}

/**
 * process a XFRM_MSG_ACQUIRE from kernel
 */
static void process_acquire(private_kernel_netlink_ipsec_t *this, struct nlmsghdr *hdr)
{
	u_int32_t reqid = 0;
	int proto = 0;
	traffic_selector_t *src_ts, *dst_ts;
	struct xfrm_user_acquire *acquire;
	struct rtattr *rta;
	size_t rtasize;

	acquire = (struct xfrm_user_acquire*)NLMSG_DATA(hdr);
	rta = XFRM_RTA(hdr, struct xfrm_user_acquire);
	rtasize = XFRM_PAYLOAD(hdr, struct xfrm_user_acquire);

	DBG2(DBG_KNL, "received a XFRM_MSG_ACQUIRE");

	while (RTA_OK(rta, rtasize))
	{
		DBG2(DBG_KNL, "  %N", xfrm_attr_type_names, rta->rta_type);

		if (rta->rta_type == XFRMA_TMPL)
		{
			struct xfrm_user_tmpl* tmpl;

			tmpl = (struct xfrm_user_tmpl*)RTA_DATA(rta);
			reqid = tmpl->reqid;
			proto = tmpl->id.proto;
		}
		rta = RTA_NEXT(rta, rtasize);
	}
	switch (proto)
	{
		case 0:
		case IPPROTO_ESP:
		case IPPROTO_AH:
			break;
		default:
			/* acquire for AH/ESP only, not for IPCOMP */
			return;
	}
	src_ts = selector2ts(&acquire->sel, TRUE);
	dst_ts = selector2ts(&acquire->sel, FALSE);

	hydra->kernel_interface->acquire(hydra->kernel_interface, reqid, src_ts,
									 dst_ts);
}

/**
 * process a XFRM_MSG_EXPIRE from kernel
 */
static void process_expire(private_kernel_netlink_ipsec_t *this, struct nlmsghdr *hdr)
{
	u_int8_t protocol;
	u_int32_t spi, reqid;
	struct xfrm_user_expire *expire;

	expire = (struct xfrm_user_expire*)NLMSG_DATA(hdr);
	protocol = expire->state.id.proto;
	spi = expire->state.id.spi;
	reqid = expire->state.reqid;

	DBG2(DBG_KNL, "received a XFRM_MSG_EXPIRE");

	if (protocol != IPPROTO_ESP && protocol != IPPROTO_AH)
	{
		DBG2(DBG_KNL, "ignoring XFRM_MSG_EXPIRE for SA with SPI %.8x and "
					  "reqid {%u} which is not a CHILD_SA", ntohl(spi), reqid);
		return;
	}

	hydra->kernel_interface->expire(hydra->kernel_interface, reqid, protocol,
									spi, expire->hard != 0);
}

/**
 * process a XFRM_MSG_MIGRATE from kernel
 */
static void process_migrate(private_kernel_netlink_ipsec_t *this, struct nlmsghdr *hdr)
{
	traffic_selector_t *src_ts, *dst_ts;
	host_t *local = NULL, *remote = NULL;
	host_t *old_src = NULL, *old_dst = NULL;
	host_t *new_src = NULL, *new_dst = NULL;
	struct xfrm_userpolicy_id *policy_id;
	struct rtattr *rta;
	size_t rtasize;
	u_int32_t reqid = 0;
	policy_dir_t dir;

	policy_id = (struct xfrm_userpolicy_id*)NLMSG_DATA(hdr);
	rta     = XFRM_RTA(hdr, struct xfrm_userpolicy_id);
	rtasize = XFRM_PAYLOAD(hdr, struct xfrm_userpolicy_id);

	DBG2(DBG_KNL, "received a XFRM_MSG_MIGRATE");

	src_ts = selector2ts(&policy_id->sel, TRUE);
	dst_ts = selector2ts(&policy_id->sel, FALSE);
	dir = (policy_dir_t)policy_id->dir;

	DBG2(DBG_KNL, "  policy: %R === %R %N", src_ts, dst_ts, policy_dir_names);

	while (RTA_OK(rta, rtasize))
	{
		DBG2(DBG_KNL, "  %N", xfrm_attr_type_names, rta->rta_type);
		if (rta->rta_type == XFRMA_KMADDRESS)
		{
			struct xfrm_user_kmaddress *kmaddress;

			kmaddress = (struct xfrm_user_kmaddress*)RTA_DATA(rta);
			local  = xfrm2host(kmaddress->family, &kmaddress->local, 0);
			remote = xfrm2host(kmaddress->family, &kmaddress->remote, 0);
			DBG2(DBG_KNL, "  kmaddress: %H...%H", local, remote);
		}
		else if (rta->rta_type == XFRMA_MIGRATE)
		{
			struct xfrm_user_migrate *migrate;

			migrate = (struct xfrm_user_migrate*)RTA_DATA(rta);
			old_src = xfrm2host(migrate->old_family, &migrate->old_saddr, 0);
			old_dst = xfrm2host(migrate->old_family, &migrate->old_daddr, 0);
			new_src = xfrm2host(migrate->new_family, &migrate->new_saddr, 0);
			new_dst = xfrm2host(migrate->new_family, &migrate->new_daddr, 0);
			reqid = migrate->reqid;
			DBG2(DBG_KNL, "  migrate %H...%H to %H...%H, reqid {%u}",
						  old_src, old_dst, new_src, new_dst, reqid);
			DESTROY_IF(old_src);
			DESTROY_IF(old_dst);
			DESTROY_IF(new_src);
			DESTROY_IF(new_dst);
		}
		rta = RTA_NEXT(rta, rtasize);
	}

	if (src_ts && dst_ts && local && remote)
	{
		hydra->kernel_interface->migrate(hydra->kernel_interface, reqid,
										 src_ts, dst_ts, dir, local, remote);
	}
	else
	{
		DESTROY_IF(src_ts);
		DESTROY_IF(dst_ts);
		DESTROY_IF(local);
		DESTROY_IF(remote);
	}
}

/**
 * process a XFRM_MSG_MAPPING from kernel
 */
static void process_mapping(private_kernel_netlink_ipsec_t *this,
							struct nlmsghdr *hdr)
{
	u_int32_t spi, reqid;
	struct xfrm_user_mapping *mapping;
	host_t *host;

	mapping = (struct xfrm_user_mapping*)NLMSG_DATA(hdr);
	spi = mapping->id.spi;
	reqid = mapping->reqid;

	DBG2(DBG_KNL, "received a XFRM_MSG_MAPPING");

	if (mapping->id.proto == IPPROTO_ESP)
	{
		host = xfrm2host(mapping->id.family, &mapping->new_saddr,
						 mapping->new_sport);
		if (host)
		{
			hydra->kernel_interface->mapping(hydra->kernel_interface, reqid,
											 spi, host);
		}
	}
}

/**
 * Receives events from kernel
 */
static job_requeue_t receive_events(private_kernel_netlink_ipsec_t *this)
{
	char response[1024];
	struct nlmsghdr *hdr = (struct nlmsghdr*)response;
	struct sockaddr_nl addr;
	socklen_t addr_len = sizeof(addr);
	int len;
	bool oldstate;

	oldstate = thread_cancelability(TRUE);
	len = recvfrom(this->socket_xfrm_events, response, sizeof(response), 0,
				   (struct sockaddr*)&addr, &addr_len);
	thread_cancelability(oldstate);

	if (len < 0)
	{
		switch (errno)
		{
			case EINTR:
				/* interrupted, try again */
				return JOB_REQUEUE_DIRECT;
			case EAGAIN:
				/* no data ready, select again */
				return JOB_REQUEUE_DIRECT;
			default:
				DBG1(DBG_KNL, "unable to receive from xfrm event socket");
				sleep(1);
				return JOB_REQUEUE_FAIR;
		}
	}

	if (addr.nl_pid != 0)
	{	/* not from kernel. not interested, try another one */
		return JOB_REQUEUE_DIRECT;
	}

	while (NLMSG_OK(hdr, len))
	{
		switch (hdr->nlmsg_type)
		{
			case XFRM_MSG_ACQUIRE:
				process_acquire(this, hdr);
				break;
			case XFRM_MSG_EXPIRE:
				process_expire(this, hdr);
				break;
			case XFRM_MSG_MIGRATE:
				process_migrate(this, hdr);
				break;
			case XFRM_MSG_MAPPING:
				process_mapping(this, hdr);
				break;
			default:
				DBG1(DBG_KNL, "received unknown event from xfrm event socket: %d", hdr->nlmsg_type);
				break;
		}
		hdr = NLMSG_NEXT(hdr, len);
	}
	return JOB_REQUEUE_DIRECT;
}

/**
 * Get an SPI for a specific protocol from the kernel.
 */
static status_t get_spi_internal(private_kernel_netlink_ipsec_t *this,
		host_t *src, host_t *dst, u_int8_t proto, u_int32_t min, u_int32_t max,
		u_int32_t reqid, u_int32_t *spi)
{
	netlink_buf_t request;
	struct nlmsghdr *hdr, *out;
	struct xfrm_userspi_info *userspi;
	u_int32_t received_spi = 0;
	size_t len;

	memset(&request, 0, sizeof(request));

	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST;
	hdr->nlmsg_type = XFRM_MSG_ALLOCSPI;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_userspi_info));

	userspi = (struct xfrm_userspi_info*)NLMSG_DATA(hdr);
	host2xfrm(src, &userspi->info.saddr);
	host2xfrm(dst, &userspi->info.id.daddr);
	userspi->info.id.proto = proto;
	userspi->info.mode = XFRM_MODE_TUNNEL;
	userspi->info.reqid = reqid;
	userspi->info.family = src->get_family(src);
	userspi->min = min;
	userspi->max = max;

	if (this->socket_xfrm->send(this->socket_xfrm, hdr, &out, &len) == SUCCESS)
	{
		hdr = out;
		while (NLMSG_OK(hdr, len))
		{
			switch (hdr->nlmsg_type)
			{
				case XFRM_MSG_NEWSA:
				{
					struct xfrm_usersa_info* usersa = NLMSG_DATA(hdr);
					received_spi = usersa->id.spi;
					break;
				}
				case NLMSG_ERROR:
				{
					struct nlmsgerr *err = NLMSG_DATA(hdr);

					DBG1(DBG_KNL, "allocating SPI failed: %s (%d)",
						 strerror(-err->error), -err->error);
					break;
				}
				default:
					hdr = NLMSG_NEXT(hdr, len);
					continue;
				case NLMSG_DONE:
					break;
			}
			break;
		}
		free(out);
	}

	if (received_spi == 0)
	{
		return FAILED;
	}

	*spi = received_spi;
	return SUCCESS;
}

METHOD(kernel_ipsec_t, get_spi, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	u_int8_t protocol, u_int32_t reqid, u_int32_t *spi)
{
	DBG2(DBG_KNL, "getting SPI for reqid {%u}", reqid);

	if (get_spi_internal(this, src, dst, protocol,
			0xc0000000, 0xcFFFFFFF, reqid, spi) != SUCCESS)
	{
		DBG1(DBG_KNL, "unable to get SPI for reqid {%u}", reqid);
		return FAILED;
	}

	DBG2(DBG_KNL, "got SPI %.8x for reqid {%u}", ntohl(*spi), reqid);

	return SUCCESS;
}

METHOD(kernel_ipsec_t, get_cpi, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	u_int32_t reqid, u_int16_t *cpi)
{
	u_int32_t received_spi = 0;

	DBG2(DBG_KNL, "getting CPI for reqid {%u}", reqid);

	if (get_spi_internal(this, src, dst,
			IPPROTO_COMP, 0x100, 0xEFFF, reqid, &received_spi) != SUCCESS)
	{
		DBG1(DBG_KNL, "unable to get CPI for reqid {%u}", reqid);
		return FAILED;
	}

	*cpi = htons((u_int16_t)ntohl(received_spi));

	DBG2(DBG_KNL, "got CPI %.4x for reqid {%u}", ntohs(*cpi), reqid);

	return SUCCESS;
}

METHOD(kernel_ipsec_t, add_sa, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	u_int32_t spi, u_int8_t protocol, u_int32_t reqid, mark_t mark,
	u_int32_t tfc, lifetime_cfg_t *lifetime, u_int16_t enc_alg, chunk_t enc_key,
	u_int16_t int_alg, chunk_t int_key, ipsec_mode_t mode, u_int16_t ipcomp,
	u_int16_t cpi, bool encap, bool esn, bool inbound,
	traffic_selector_t* src_ts, traffic_selector_t* dst_ts)
{
	netlink_buf_t request;
	char *alg_name;
	struct nlmsghdr *hdr;
	struct xfrm_usersa_info *sa;
	u_int16_t icv_size = 64;
	status_t status = FAILED;

	/* if IPComp is used, we install an additional IPComp SA. if the cpi is 0
	 * we are in the recursive call below */
	if (ipcomp != IPCOMP_NONE && cpi != 0)
	{
		lifetime_cfg_t lft = {{0,0,0},{0,0,0},{0,0,0}};
		add_sa(this, src, dst, htonl(ntohs(cpi)), IPPROTO_COMP, reqid, mark, tfc,
			   &lft, ENCR_UNDEFINED, chunk_empty, AUTH_UNDEFINED, chunk_empty,
			   mode, ipcomp, 0, FALSE, FALSE, inbound, NULL, NULL);
		ipcomp = IPCOMP_NONE;
		/* use transport mode ESP SA, IPComp uses tunnel mode */
		mode = MODE_TRANSPORT;
	}

	memset(&request, 0, sizeof(request));

	if (mark.value)
	{
		DBG2(DBG_KNL, "adding SAD entry with SPI %.8x and reqid {%u}  "
					  "(mark %u/0x%8x)", ntohl(spi), reqid, mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "adding SAD entry with SPI %.8x and reqid {%u}",
					   ntohl(spi), reqid);
	}
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_type = inbound ? XFRM_MSG_UPDSA : XFRM_MSG_NEWSA;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_info));

	sa = (struct xfrm_usersa_info*)NLMSG_DATA(hdr);
	host2xfrm(src, &sa->saddr);
	host2xfrm(dst, &sa->id.daddr);
	sa->id.spi = spi;
	sa->id.proto = protocol;
	sa->family = src->get_family(src);
	sa->mode = mode2kernel(mode);
	switch (mode)
	{
		case MODE_TUNNEL:
			sa->flags |= XFRM_STATE_AF_UNSPEC;
			break;
		case MODE_BEET:
		case MODE_TRANSPORT:
			if(src_ts && dst_ts)
			{
				sa->sel = ts2selector(src_ts, dst_ts);
			}
			break;
		default:
			break;
	}

	sa->reqid = reqid;
	sa->lft.soft_byte_limit = XFRM_LIMIT(lifetime->bytes.rekey);
	sa->lft.hard_byte_limit = XFRM_LIMIT(lifetime->bytes.life);
	sa->lft.soft_packet_limit = XFRM_LIMIT(lifetime->packets.rekey);
	sa->lft.hard_packet_limit = XFRM_LIMIT(lifetime->packets.life);
	/* we use lifetimes since added, not since used */
	sa->lft.soft_add_expires_seconds = lifetime->time.rekey;
	sa->lft.hard_add_expires_seconds = lifetime->time.life;
	sa->lft.soft_use_expires_seconds = 0;
	sa->lft.hard_use_expires_seconds = 0;

	struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_usersa_info);

	switch (enc_alg)
	{
		case ENCR_UNDEFINED:
			/* no encryption */
			break;
		case ENCR_AES_CCM_ICV16:
		case ENCR_AES_GCM_ICV16:
		case ENCR_NULL_AUTH_AES_GMAC:
		case ENCR_CAMELLIA_CCM_ICV16:
			icv_size += 32;
			/* FALL */
		case ENCR_AES_CCM_ICV12:
		case ENCR_AES_GCM_ICV12:
		case ENCR_CAMELLIA_CCM_ICV12:
			icv_size += 32;
			/* FALL */
		case ENCR_AES_CCM_ICV8:
		case ENCR_AES_GCM_ICV8:
		case ENCR_CAMELLIA_CCM_ICV8:
		{
			struct xfrm_algo_aead *algo;

			alg_name = lookup_algorithm(encryption_algs, enc_alg);
			if (alg_name == NULL)
			{
				DBG1(DBG_KNL, "algorithm %N not supported by kernel!",
					 encryption_algorithm_names, enc_alg);
				goto failed;
			}
			DBG2(DBG_KNL, "  using encryption algorithm %N with key size %d",
				 encryption_algorithm_names, enc_alg, enc_key.len * 8);

			rthdr->rta_type = XFRMA_ALG_AEAD;
			rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_algo_aead) + enc_key.len);
			hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
			if (hdr->nlmsg_len > sizeof(request))
			{
				goto failed;
			}

			algo = (struct xfrm_algo_aead*)RTA_DATA(rthdr);
			algo->alg_key_len = enc_key.len * 8;
			algo->alg_icv_len = icv_size;
			strcpy(algo->alg_name, alg_name);
			memcpy(algo->alg_key, enc_key.ptr, enc_key.len);

			rthdr = XFRM_RTA_NEXT(rthdr);
			break;
		}
		default:
		{
			struct xfrm_algo *algo;

			alg_name = lookup_algorithm(encryption_algs, enc_alg);
			if (alg_name == NULL)
			{
				DBG1(DBG_KNL, "algorithm %N not supported by kernel!",
					 encryption_algorithm_names, enc_alg);
				goto failed;
			}
			DBG2(DBG_KNL, "  using encryption algorithm %N with key size %d",
				 encryption_algorithm_names, enc_alg, enc_key.len * 8);

			rthdr->rta_type = XFRMA_ALG_CRYPT;
			rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_algo) + enc_key.len);
			hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
			if (hdr->nlmsg_len > sizeof(request))
			{
				goto failed;
			}

			algo = (struct xfrm_algo*)RTA_DATA(rthdr);
			algo->alg_key_len = enc_key.len * 8;
			strcpy(algo->alg_name, alg_name);
			memcpy(algo->alg_key, enc_key.ptr, enc_key.len);

			rthdr = XFRM_RTA_NEXT(rthdr);
		}
	}

	if (int_alg != AUTH_UNDEFINED)
	{
		alg_name = lookup_algorithm(integrity_algs, int_alg);
		if (alg_name == NULL)
		{
			DBG1(DBG_KNL, "algorithm %N not supported by kernel!",
				 integrity_algorithm_names, int_alg);
			goto failed;
		}
		DBG2(DBG_KNL, "  using integrity algorithm %N with key size %d",
			 integrity_algorithm_names, int_alg, int_key.len * 8);

		if (int_alg == AUTH_HMAC_SHA2_256_128)
		{
			struct xfrm_algo_auth* algo;

			/* the kernel uses SHA256 with 96 bit truncation by default,
			 * use specified truncation size supported by newer kernels */
			rthdr->rta_type = XFRMA_ALG_AUTH_TRUNC;
			rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_algo_auth) + int_key.len);

			hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
			if (hdr->nlmsg_len > sizeof(request))
			{
				goto failed;
			}

			algo = (struct xfrm_algo_auth*)RTA_DATA(rthdr);
			algo->alg_key_len = int_key.len * 8;
			algo->alg_trunc_len = 128;
			strcpy(algo->alg_name, alg_name);
			memcpy(algo->alg_key, int_key.ptr, int_key.len);
		}
		else
		{
			struct xfrm_algo* algo;

			rthdr->rta_type = XFRMA_ALG_AUTH;
			rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_algo) + int_key.len);

			hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
			if (hdr->nlmsg_len > sizeof(request))
			{
				goto failed;
			}

			algo = (struct xfrm_algo*)RTA_DATA(rthdr);
			algo->alg_key_len = int_key.len * 8;
			strcpy(algo->alg_name, alg_name);
			memcpy(algo->alg_key, int_key.ptr, int_key.len);
		}
		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (ipcomp != IPCOMP_NONE)
	{
		rthdr->rta_type = XFRMA_ALG_COMP;
		alg_name = lookup_algorithm(compression_algs, ipcomp);
		if (alg_name == NULL)
		{
			DBG1(DBG_KNL, "algorithm %N not supported by kernel!",
				 ipcomp_transform_names, ipcomp);
			goto failed;
		}
		DBG2(DBG_KNL, "  using compression algorithm %N",
			 ipcomp_transform_names, ipcomp);

		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_algo));
		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}

		struct xfrm_algo* algo = (struct xfrm_algo*)RTA_DATA(rthdr);
		algo->alg_key_len = 0;
		strcpy(algo->alg_name, alg_name);

		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (encap)
	{
		struct xfrm_encap_tmpl *tmpl;

		rthdr->rta_type = XFRMA_ENCAP;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_encap_tmpl));

		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}

		tmpl = (struct xfrm_encap_tmpl*)RTA_DATA(rthdr);
		tmpl->encap_type = UDP_ENCAP_ESPINUDP;
		tmpl->encap_sport = htons(src->get_port(src));
		tmpl->encap_dport = htons(dst->get_port(dst));
		memset(&tmpl->encap_oa, 0, sizeof (xfrm_address_t));
		/* encap_oa could probably be derived from the
		 * traffic selectors [rfc4306, p39]. In the netlink kernel implementation
		 * pluto does the same as we do here but it uses encap_oa in the
		 * pfkey implementation. BUT as /usr/src/linux/net/key/af_key.c indicates
		 * the kernel ignores it anyway
		 *   -> does that mean that NAT-T encap doesn't work in transport mode?
		 * No. The reason the kernel ignores NAT-OA is that it recomputes
		 * (or, rather, just ignores) the checksum. If packets pass
		 * the IPsec checks it marks them "checksum ok" so OA isn't needed. */
		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (mark.value)
	{
		struct xfrm_mark *mrk;

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));

		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (tfc)
	{
		u_int32_t *tfcpad;

		rthdr->rta_type = XFRMA_TFCPAD;
		rthdr->rta_len = RTA_LENGTH(sizeof(u_int32_t));

		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}

		tfcpad = (u_int32_t*)RTA_DATA(rthdr);
		*tfcpad = tfc;
		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (protocol != IPPROTO_COMP)
	{
		if (esn || this->replay_window > DEFAULT_REPLAY_WINDOW)
		{
			/* for ESN or larger replay windows we need the new
			 * XFRMA_REPLAY_ESN_VAL attribute to configure a bitmap */
			struct xfrm_replay_state_esn *replay;

			rthdr->rta_type = XFRMA_REPLAY_ESN_VAL;
			rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_replay_state_esn) +
										(this->replay_window + 7) / 8);

			hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
			if (hdr->nlmsg_len > sizeof(request))
			{
				goto failed;
			}

			replay = (struct xfrm_replay_state_esn*)RTA_DATA(rthdr);
			/* bmp_len contains number uf __u32's */
			replay->bmp_len = this->replay_bmp;
			replay->replay_window = this->replay_window;

			rthdr = XFRM_RTA_NEXT(rthdr);
			if (esn)
			{
				sa->flags |= XFRM_STATE_ESN;
			}
		}
		else
		{
			sa->replay_window = DEFAULT_REPLAY_WINDOW;
		}
	}

	if (this->socket_xfrm->send_ack(this->socket_xfrm, hdr) != SUCCESS)
	{
		if (mark.value)
		{
			DBG1(DBG_KNL, "unable to add SAD entry with SPI %.8x  "
						  "(mark %u/0x%8x)", ntohl(spi), mark.value, mark.mask);
		}
		else
		{
			DBG1(DBG_KNL, "unable to add SAD entry with SPI %.8x", ntohl(spi));
		}
		goto failed;
	}

	status = SUCCESS;

failed:
	memwipe(request, sizeof(request));
	return status;
}

/**
 * Get the ESN replay state (i.e. sequence numbers) of an SA.
 *
 * Allocates into one the replay state structure we get from the kernel.
 */
static void get_replay_state(private_kernel_netlink_ipsec_t *this,
							 u_int32_t spi, u_int8_t protocol, host_t *dst,
							 struct xfrm_replay_state_esn **replay_esn,
							 struct xfrm_replay_state **replay)
{
	netlink_buf_t request;
	struct nlmsghdr *hdr, *out = NULL;
	struct xfrm_aevent_id *out_aevent = NULL, *aevent_id;
	size_t len;
	struct rtattr *rta;
	size_t rtasize;

	memset(&request, 0, sizeof(request));

	DBG2(DBG_KNL, "querying replay state from SAD entry with SPI %.8x",
		 ntohl(spi));

	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST;
	hdr->nlmsg_type = XFRM_MSG_GETAE;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_aevent_id));

	aevent_id = (struct xfrm_aevent_id*)NLMSG_DATA(hdr);
	aevent_id->flags = XFRM_AE_RVAL;

	host2xfrm(dst, &aevent_id->sa_id.daddr);
	aevent_id->sa_id.spi = spi;
	aevent_id->sa_id.proto = protocol;
	aevent_id->sa_id.family = dst->get_family(dst);

	if (this->socket_xfrm->send(this->socket_xfrm, hdr, &out, &len) == SUCCESS)
	{
		hdr = out;
		while (NLMSG_OK(hdr, len))
		{
			switch (hdr->nlmsg_type)
			{
				case XFRM_MSG_NEWAE:
				{
					out_aevent = NLMSG_DATA(hdr);
					break;
				}
				case NLMSG_ERROR:
				{
					struct nlmsgerr *err = NLMSG_DATA(hdr);
					DBG1(DBG_KNL, "querying replay state from SAD entry failed: %s (%d)",
						 strerror(-err->error), -err->error);
					break;
				}
				default:
					hdr = NLMSG_NEXT(hdr, len);
					continue;
				case NLMSG_DONE:
					break;
			}
			break;
		}
	}

	if (out_aevent)
	{
		rta = XFRM_RTA(out, struct xfrm_aevent_id);
		rtasize = XFRM_PAYLOAD(out, struct xfrm_aevent_id);
		while (RTA_OK(rta, rtasize))
		{
			if (rta->rta_type == XFRMA_REPLAY_VAL &&
				RTA_PAYLOAD(rta) == sizeof(**replay))
			{
				*replay = malloc(RTA_PAYLOAD(rta));
				memcpy(*replay, RTA_DATA(rta), RTA_PAYLOAD(rta));
				break;
			}
			if (rta->rta_type == XFRMA_REPLAY_ESN_VAL &&
				RTA_PAYLOAD(rta) >= sizeof(**replay_esn) + this->replay_bmp)
			{
				*replay_esn = malloc(RTA_PAYLOAD(rta));
				memcpy(*replay_esn, RTA_DATA(rta), RTA_PAYLOAD(rta));
				break;
			}
			rta = RTA_NEXT(rta, rtasize);
		}
	}
	free(out);
}

METHOD(kernel_ipsec_t, query_sa, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	u_int32_t spi, u_int8_t protocol, mark_t mark, u_int64_t *bytes)
{
	netlink_buf_t request;
	struct nlmsghdr *out = NULL, *hdr;
	struct xfrm_usersa_id *sa_id;
	struct xfrm_usersa_info *sa = NULL;
	status_t status = FAILED;
	size_t len;

	memset(&request, 0, sizeof(request));

	if (mark.value)
	{
		DBG2(DBG_KNL, "querying SAD entry with SPI %.8x  (mark %u/0x%8x)",
					   ntohl(spi), mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "querying SAD entry with SPI %.8x", ntohl(spi));
	}
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST;
	hdr->nlmsg_type = XFRM_MSG_GETSA;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_id));

	sa_id = (struct xfrm_usersa_id*)NLMSG_DATA(hdr);
	host2xfrm(dst, &sa_id->daddr);
	sa_id->spi = spi;
	sa_id->proto = protocol;
	sa_id->family = dst->get_family(dst);

	if (mark.value)
	{
		struct xfrm_mark *mrk;
		struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_usersa_id);

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));
		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			return FAILED;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
	}

	if (this->socket_xfrm->send(this->socket_xfrm, hdr, &out, &len) == SUCCESS)
	{
		hdr = out;
		while (NLMSG_OK(hdr, len))
		{
			switch (hdr->nlmsg_type)
			{
				case XFRM_MSG_NEWSA:
				{
					sa = (struct xfrm_usersa_info*)NLMSG_DATA(hdr);
					break;
				}
				case NLMSG_ERROR:
				{
					struct nlmsgerr *err = NLMSG_DATA(hdr);

					if (mark.value)
					{
						DBG1(DBG_KNL, "querying SAD entry with SPI %.8x  "
									  "(mark %u/0x%8x) failed: %s (%d)",
									   ntohl(spi), mark.value, mark.mask,
									   strerror(-err->error), -err->error);
					}
					else
					{
						DBG1(DBG_KNL, "querying SAD entry with SPI %.8x "
									  "failed: %s (%d)", ntohl(spi),
									   strerror(-err->error), -err->error);
					}
					break;
				}
				default:
					hdr = NLMSG_NEXT(hdr, len);
					continue;
				case NLMSG_DONE:
					break;
			}
			break;
		}
	}

	if (sa == NULL)
	{
		DBG2(DBG_KNL, "unable to query SAD entry with SPI %.8x", ntohl(spi));
	}
	else
	{
		*bytes = sa->curlft.bytes;
		status = SUCCESS;
	}
	memwipe(out, len);
	free(out);
	return status;
}

METHOD(kernel_ipsec_t, del_sa, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	u_int32_t spi, u_int8_t protocol, u_int16_t cpi, mark_t mark)
{
	netlink_buf_t request;
	struct nlmsghdr *hdr;
	struct xfrm_usersa_id *sa_id;

	/* if IPComp was used, we first delete the additional IPComp SA */
	if (cpi)
	{
		del_sa(this, src, dst, htonl(ntohs(cpi)), IPPROTO_COMP, 0, mark);
	}

	memset(&request, 0, sizeof(request));

	if (mark.value)
	{
		DBG2(DBG_KNL, "deleting SAD entry with SPI %.8x  (mark %u/0x%8x)",
					   ntohl(spi), mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "deleting SAD entry with SPI %.8x", ntohl(spi));
	}
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_type = XFRM_MSG_DELSA;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_id));

	sa_id = (struct xfrm_usersa_id*)NLMSG_DATA(hdr);
	host2xfrm(dst, &sa_id->daddr);
	sa_id->spi = spi;
	sa_id->proto = protocol;
	sa_id->family = dst->get_family(dst);

	if (mark.value)
	{
		struct xfrm_mark *mrk;
		struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_usersa_id);

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));
		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			return FAILED;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
	}

	if (this->socket_xfrm->send_ack(this->socket_xfrm, hdr) != SUCCESS)
	{
		if (mark.value)
		{
			DBG1(DBG_KNL, "unable to delete SAD entry with SPI %.8x  "
						  "(mark %u/0x%8x)", ntohl(spi), mark.value, mark.mask);
		}
		else
		{
			DBG1(DBG_KNL, "unable to delete SAD entry with SPI %.8x", ntohl(spi));
		}
		return FAILED;
	}
	if (mark.value)
	{
		DBG2(DBG_KNL, "deleted SAD entry with SPI %.8x  (mark %u/0x%8x)",
					   ntohl(spi), mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "deleted SAD entry with SPI %.8x", ntohl(spi));
	}
	return SUCCESS;
}

METHOD(kernel_ipsec_t, update_sa, status_t,
	private_kernel_netlink_ipsec_t *this, u_int32_t spi, u_int8_t protocol,
	u_int16_t cpi, host_t *src, host_t *dst, host_t *new_src, host_t *new_dst,
	bool old_encap, bool new_encap, mark_t mark)
{
	netlink_buf_t request;
	u_char *pos;
	struct nlmsghdr *hdr, *out = NULL;
	struct xfrm_usersa_id *sa_id;
	struct xfrm_usersa_info *out_sa = NULL, *sa;
	size_t len;
	struct rtattr *rta;
	size_t rtasize;
	struct xfrm_encap_tmpl* tmpl = NULL;
	struct xfrm_replay_state *replay = NULL;
	struct xfrm_replay_state_esn *replay_esn = NULL;
	status_t status = FAILED;

	/* if IPComp is used, we first update the IPComp SA */
	if (cpi)
	{
		update_sa(this, htonl(ntohs(cpi)), IPPROTO_COMP, 0,
				  src, dst, new_src, new_dst, FALSE, FALSE, mark);
	}

	memset(&request, 0, sizeof(request));

	DBG2(DBG_KNL, "querying SAD entry with SPI %.8x for update", ntohl(spi));

	/* query the existing SA first */
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST;
	hdr->nlmsg_type = XFRM_MSG_GETSA;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_id));

	sa_id = (struct xfrm_usersa_id*)NLMSG_DATA(hdr);
	host2xfrm(dst, &sa_id->daddr);
	sa_id->spi = spi;
	sa_id->proto = protocol;
	sa_id->family = dst->get_family(dst);

	if (this->socket_xfrm->send(this->socket_xfrm, hdr, &out, &len) == SUCCESS)
	{
		hdr = out;
		while (NLMSG_OK(hdr, len))
		{
			switch (hdr->nlmsg_type)
			{
				case XFRM_MSG_NEWSA:
				{
					out_sa = NLMSG_DATA(hdr);
					break;
				}
				case NLMSG_ERROR:
				{
					struct nlmsgerr *err = NLMSG_DATA(hdr);
					DBG1(DBG_KNL, "querying SAD entry failed: %s (%d)",
						 strerror(-err->error), -err->error);
					break;
				}
				default:
					hdr = NLMSG_NEXT(hdr, len);
					continue;
				case NLMSG_DONE:
					break;
			}
			break;
		}
	}
	if (out_sa == NULL)
	{
		DBG1(DBG_KNL, "unable to update SAD entry with SPI %.8x", ntohl(spi));
		goto failed;
	}

	get_replay_state(this, spi, protocol, dst, &replay_esn, &replay);

	/* delete the old SA (without affecting the IPComp SA) */
	if (del_sa(this, src, dst, spi, protocol, 0, mark) != SUCCESS)
	{
		DBG1(DBG_KNL, "unable to delete old SAD entry with SPI %.8x", ntohl(spi));
		goto failed;
	}

	DBG2(DBG_KNL, "updating SAD entry with SPI %.8x from %#H..%#H to %#H..%#H",
		 ntohl(spi), src, dst, new_src, new_dst);
	/* copy over the SA from out to request */
	hdr = (struct nlmsghdr*)request;
	memcpy(hdr, out, min(out->nlmsg_len, sizeof(request)));
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_type = XFRM_MSG_NEWSA;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_usersa_info));
	sa = NLMSG_DATA(hdr);
	sa->family = new_dst->get_family(new_dst);

	if (!src->ip_equals(src, new_src))
	{
		host2xfrm(new_src, &sa->saddr);
	}
	if (!dst->ip_equals(dst, new_dst))
	{
		host2xfrm(new_dst, &sa->id.daddr);
	}

	rta = XFRM_RTA(out, struct xfrm_usersa_info);
	rtasize = XFRM_PAYLOAD(out, struct xfrm_usersa_info);
	pos = (u_char*)XFRM_RTA(hdr, struct xfrm_usersa_info);
	while(RTA_OK(rta, rtasize))
	{
		/* copy all attributes, but not XFRMA_ENCAP if we are disabling it */
		if (rta->rta_type != XFRMA_ENCAP || new_encap)
		{
			if (rta->rta_type == XFRMA_ENCAP)
			{	/* update encap tmpl */
				tmpl = (struct xfrm_encap_tmpl*)RTA_DATA(rta);
				tmpl->encap_sport = ntohs(new_src->get_port(new_src));
				tmpl->encap_dport = ntohs(new_dst->get_port(new_dst));
			}
			memcpy(pos, rta, rta->rta_len);
			pos += RTA_ALIGN(rta->rta_len);
			hdr->nlmsg_len += RTA_ALIGN(rta->rta_len);
		}
		rta = RTA_NEXT(rta, rtasize);
	}

	rta = (struct rtattr*)pos;
	if (tmpl == NULL && new_encap)
	{	/* add tmpl if we are enabling it */
		rta->rta_type = XFRMA_ENCAP;
		rta->rta_len = RTA_LENGTH(sizeof(struct xfrm_encap_tmpl));

		hdr->nlmsg_len += RTA_ALIGN(rta->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}

		tmpl = (struct xfrm_encap_tmpl*)RTA_DATA(rta);
		tmpl->encap_type = UDP_ENCAP_ESPINUDP;
		tmpl->encap_sport = ntohs(new_src->get_port(new_src));
		tmpl->encap_dport = ntohs(new_dst->get_port(new_dst));
		memset(&tmpl->encap_oa, 0, sizeof (xfrm_address_t));

		rta = XFRM_RTA_NEXT(rta);
	}

	if (replay_esn)
	{
		rta->rta_type = XFRMA_REPLAY_ESN_VAL;
		rta->rta_len = RTA_LENGTH(sizeof(struct xfrm_replay_state_esn) +
								  this->replay_bmp);

		hdr->nlmsg_len += RTA_ALIGN(rta->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}
		memcpy(RTA_DATA(rta), replay_esn,
			   sizeof(struct xfrm_replay_state_esn) + this->replay_bmp);

		rta = XFRM_RTA_NEXT(rta);
	}
	else if (replay)
	{
		rta->rta_type = XFRMA_REPLAY_VAL;
		rta->rta_len = RTA_LENGTH(sizeof(struct xfrm_replay_state));

		hdr->nlmsg_len += RTA_ALIGN(rta->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			goto failed;
		}
		memcpy(RTA_DATA(rta), replay, sizeof(replay));

		rta = XFRM_RTA_NEXT(rta);
	}
	else
	{
		DBG1(DBG_KNL, "unable to copy replay state from old SAD entry "
			 "with SPI %.8x", ntohl(spi));
	}

	if (this->socket_xfrm->send_ack(this->socket_xfrm, hdr) != SUCCESS)
	{
		DBG1(DBG_KNL, "unable to update SAD entry with SPI %.8x", ntohl(spi));
		goto failed;
	}

	status = SUCCESS;
failed:
	free(replay);
	free(replay_esn);
	memwipe(out, len);
	free(out);

	return status;
}

METHOD(kernel_ipsec_t, add_policy, status_t,
	private_kernel_netlink_ipsec_t *this, host_t *src, host_t *dst,
	traffic_selector_t *src_ts, traffic_selector_t *dst_ts,
	policy_dir_t direction, policy_type_t type, ipsec_sa_cfg_t *sa,
	mark_t mark, bool routed)
{
	policy_entry_t *current, *policy;
	bool found = FALSE;
	netlink_buf_t request;
	struct xfrm_userpolicy_info *policy_info;
	struct nlmsghdr *hdr;
	int i;

	/* create a policy */
	policy = malloc_thing(policy_entry_t);
	memset(policy, 0, sizeof(policy_entry_t));
	policy->sel = ts2selector(src_ts, dst_ts);
	policy->mark = mark.value & mark.mask;
	policy->direction = direction;

	/* find the policy, which matches EXACTLY */
	this->mutex->lock(this->mutex);
	current = this->policies->get(this->policies, policy);
	if (current)
	{
		/* use existing policy */
		current->refcount++;
		if (mark.value)
		{
			DBG2(DBG_KNL, "policy %R === %R %N  (mark %u/0x%8x) "
						  "already exists, increasing refcount",
						   src_ts, dst_ts, policy_dir_names, direction,
						   mark.value, mark.mask);
		}
		else
		{
			DBG2(DBG_KNL, "policy %R === %R %N "
						  "already exists, increasing refcount",
						   src_ts, dst_ts, policy_dir_names, direction);
		}
		free(policy);
		policy = current;
		found = TRUE;
	}
	else
	{	/* apply the new one, if we have no such policy */
		this->policies->put(this->policies, policy, policy);
		policy->refcount = 1;
	}

	if (mark.value)
	{
		DBG2(DBG_KNL, "adding policy %R === %R %N  (mark %u/0x%8x)",
					   src_ts, dst_ts, policy_dir_names, direction,
					   mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "adding policy %R === %R %N",
					   src_ts, dst_ts, policy_dir_names, direction);
	}

	memset(&request, 0, sizeof(request));
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_type = found ? XFRM_MSG_UPDPOLICY : XFRM_MSG_NEWPOLICY;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_userpolicy_info));

	policy_info = (struct xfrm_userpolicy_info*)NLMSG_DATA(hdr);
	policy_info->sel = policy->sel;
	policy_info->dir = policy->direction;

	/* calculate priority based on selector size, small size = high prio */
	policy_info->priority = routed ? PRIO_LOW : PRIO_HIGH;
	policy_info->priority -= policy->sel.prefixlen_s;
	policy_info->priority -= policy->sel.prefixlen_d;
	policy_info->priority <<= 2; /* make some room for the two flags */
	policy_info->priority += policy->sel.sport_mask ||
							 policy->sel.dport_mask ? 0 : 2;
	policy_info->priority += policy->sel.proto ? 0 : 1;

	policy_info->action = type != POLICY_DROP ? XFRM_POLICY_ALLOW
											  : XFRM_POLICY_BLOCK;
	policy_info->share = XFRM_SHARE_ANY;
	this->mutex->unlock(this->mutex);

	/* policies don't expire */
	policy_info->lft.soft_byte_limit = XFRM_INF;
	policy_info->lft.soft_packet_limit = XFRM_INF;
	policy_info->lft.hard_byte_limit = XFRM_INF;
	policy_info->lft.hard_packet_limit = XFRM_INF;
	policy_info->lft.soft_add_expires_seconds = 0;
	policy_info->lft.hard_add_expires_seconds = 0;
	policy_info->lft.soft_use_expires_seconds = 0;
	policy_info->lft.hard_use_expires_seconds = 0;

	struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_userpolicy_info);

	if (type == POLICY_IPSEC)
	{
		struct xfrm_user_tmpl *tmpl = (struct xfrm_user_tmpl*)RTA_DATA(rthdr);
		struct {
			u_int8_t proto;
			bool use;
		} protos[] = {
			{ IPPROTO_COMP, sa->ipcomp.transform != IPCOMP_NONE },
			{ IPPROTO_ESP, sa->esp.use },
			{ IPPROTO_AH, sa->ah.use },
		};
		ipsec_mode_t proto_mode = sa->mode;

		rthdr->rta_type = XFRMA_TMPL;
		rthdr->rta_len = 0; /* actual length is set below */

		for (i = 0; i < countof(protos); i++)
		{
			if (!protos[i].use)
			{
				continue;
			}

			rthdr->rta_len += RTA_LENGTH(sizeof(struct xfrm_user_tmpl));
			hdr->nlmsg_len += RTA_ALIGN(RTA_LENGTH(sizeof(struct xfrm_user_tmpl)));
			if (hdr->nlmsg_len > sizeof(request))
			{
				return FAILED;
			}

			tmpl->reqid = sa->reqid;
			tmpl->id.proto = protos[i].proto;
			tmpl->aalgos = tmpl->ealgos = tmpl->calgos = ~0;
			tmpl->mode = mode2kernel(proto_mode);
			tmpl->optional = protos[i].proto == IPPROTO_COMP &&
							 direction != POLICY_OUT;
			tmpl->family = src->get_family(src);

			if (proto_mode == MODE_TUNNEL)
			{	/* only for tunnel mode */
				host2xfrm(src, &tmpl->saddr);
				host2xfrm(dst, &tmpl->id.daddr);
			}

			tmpl++;

			/* use transport mode for other SAs */
			proto_mode = MODE_TRANSPORT;
		}

		rthdr = XFRM_RTA_NEXT(rthdr);
	}

	if (mark.value)
	{
		struct xfrm_mark *mrk;

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));

		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			return FAILED;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
	}

	if (this->socket_xfrm->send_ack(this->socket_xfrm, hdr) != SUCCESS)
	{
		DBG1(DBG_KNL, "unable to add policy %R === %R %N", src_ts, dst_ts,
					   policy_dir_names, direction);
		return FAILED;
	}

	/* install a route, if:
	 * - we are NOT updating a policy
	 * - this is a forward policy (to just get one for each child)
	 * - we are in tunnel/BEET mode
	 * - routing is not disabled via strongswan.conf
	 */
	if (policy->route == NULL && direction == POLICY_FWD &&
		sa->mode != MODE_TRANSPORT && this->install_routes)
	{
		route_entry_t *route = malloc_thing(route_entry_t);

		if (hydra->kernel_interface->get_address_by_ts(hydra->kernel_interface,
				dst_ts, &route->src_ip) == SUCCESS)
		{
			/* get the nexthop to src (src as we are in POLICY_FWD).*/
			route->gateway = hydra->kernel_interface->get_nexthop(
												hydra->kernel_interface, src);
			/* install route via outgoing interface */
			route->if_name = hydra->kernel_interface->get_interface(
												hydra->kernel_interface, dst);
			route->dst_net = chunk_alloc(policy->sel.family == AF_INET ? 4 : 16);
			memcpy(route->dst_net.ptr, &policy->sel.saddr, route->dst_net.len);
			route->prefixlen = policy->sel.prefixlen_s;

			if (route->if_name)
			{
				DBG2(DBG_KNL, "installing route: %R via %H src %H dev %s",
					 src_ts, route->gateway, route->src_ip, route->if_name);
				switch (hydra->kernel_interface->add_route(
									hydra->kernel_interface, route->dst_net,
									route->prefixlen, route->gateway,
									route->src_ip, route->if_name))
				{
					default:
						DBG1(DBG_KNL, "unable to install source route for %H",
							 route->src_ip);
						/* FALL */
					case ALREADY_DONE:
						/* route exists, do not uninstall */
						route_entry_destroy(route);
						break;
					case SUCCESS:
						/* cache the installed route */
						policy->route = route;
						break;
				}
			}
			else
			{
				route_entry_destroy(route);
			}
		}
		else
		{
			free(route);
		}
	}
	return SUCCESS;
}

METHOD(kernel_ipsec_t, query_policy, status_t,
	private_kernel_netlink_ipsec_t *this, traffic_selector_t *src_ts,
	traffic_selector_t *dst_ts, policy_dir_t direction, mark_t mark,
	u_int32_t *use_time)
{
	netlink_buf_t request;
	struct nlmsghdr *out = NULL, *hdr;
	struct xfrm_userpolicy_id *policy_id;
	struct xfrm_userpolicy_info *policy = NULL;
	size_t len;

	memset(&request, 0, sizeof(request));

	if (mark.value)
	{
		DBG2(DBG_KNL, "querying policy %R === %R %N  (mark %u/0x%8x)",
					   src_ts, dst_ts, policy_dir_names, direction,
					   mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "querying policy %R === %R %N", src_ts, dst_ts,
					   policy_dir_names, direction);
	}
	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST;
	hdr->nlmsg_type = XFRM_MSG_GETPOLICY;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_userpolicy_id));

	policy_id = (struct xfrm_userpolicy_id*)NLMSG_DATA(hdr);
	policy_id->sel = ts2selector(src_ts, dst_ts);
	policy_id->dir = direction;

	if (mark.value)
	{
		struct xfrm_mark *mrk;
		struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_userpolicy_id);

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));

		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			return FAILED;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
	}

	if (this->socket_xfrm->send(this->socket_xfrm, hdr, &out, &len) == SUCCESS)
	{
		hdr = out;
		while (NLMSG_OK(hdr, len))
		{
			switch (hdr->nlmsg_type)
			{
				case XFRM_MSG_NEWPOLICY:
				{
					policy = (struct xfrm_userpolicy_info*)NLMSG_DATA(hdr);
					break;
				}
				case NLMSG_ERROR:
				{
					struct nlmsgerr *err = NLMSG_DATA(hdr);
					DBG1(DBG_KNL, "querying policy failed: %s (%d)",
						 strerror(-err->error), -err->error);
					break;
				}
				default:
					hdr = NLMSG_NEXT(hdr, len);
					continue;
				case NLMSG_DONE:
					break;
			}
			break;
		}
	}

	if (policy == NULL)
	{
		DBG2(DBG_KNL, "unable to query policy %R === %R %N", src_ts, dst_ts,
					   policy_dir_names, direction);
		free(out);
		return FAILED;
	}

	if (policy->curlft.use_time)
	{
		/* we need the monotonic time, but the kernel returns system time. */
		*use_time = time_monotonic(NULL) - (time(NULL) - policy->curlft.use_time);
	}
	else
	{
		*use_time = 0;
	}

	free(out);
	return SUCCESS;
}

METHOD(kernel_ipsec_t, del_policy, status_t,
	private_kernel_netlink_ipsec_t *this, traffic_selector_t *src_ts,
	traffic_selector_t *dst_ts, policy_dir_t direction, mark_t mark,
	bool unrouted)
{
	policy_entry_t *current, policy, *to_delete = NULL;
	route_entry_t *route;
	netlink_buf_t request;
	struct nlmsghdr *hdr;
	struct xfrm_userpolicy_id *policy_id;

	if (mark.value)
	{
		DBG2(DBG_KNL, "deleting policy %R === %R %N  (mark %u/0x%8x)",
					   src_ts, dst_ts, policy_dir_names, direction,
					   mark.value, mark.mask);
	}
	else
	{
		DBG2(DBG_KNL, "deleting policy %R === %R %N",
					   src_ts, dst_ts, policy_dir_names, direction);
	}

	/* create a policy */
	memset(&policy, 0, sizeof(policy_entry_t));
	policy.sel = ts2selector(src_ts, dst_ts);
	policy.mark = mark.value & mark.mask;
	policy.direction = direction;

	/* find the policy */
	this->mutex->lock(this->mutex);
	current = this->policies->get(this->policies, &policy);
	if (current)
	{
		to_delete = current;
		if (--to_delete->refcount > 0)
		{
			/* is used by more SAs, keep in kernel */
			DBG2(DBG_KNL, "policy still used by another CHILD_SA, not removed");
			this->mutex->unlock(this->mutex);
			return SUCCESS;
		}
		/* remove if last reference */
		this->policies->remove(this->policies, to_delete);
	}
	this->mutex->unlock(this->mutex);
	if (!to_delete)
	{
		if (mark.value)
		{
			DBG1(DBG_KNL, "deleting policy %R === %R %N  (mark %u/0x%8x) "
						  "failed, not found", src_ts, dst_ts, policy_dir_names,
						   direction, mark.value, mark.mask);
		}
		else
		{
			DBG1(DBG_KNL, "deleting policy %R === %R %N failed, not found",
						   src_ts, dst_ts, policy_dir_names, direction);
		}
		return NOT_FOUND;
	}

	memset(&request, 0, sizeof(request));

	hdr = (struct nlmsghdr*)request;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	hdr->nlmsg_type = XFRM_MSG_DELPOLICY;
	hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct xfrm_userpolicy_id));

	policy_id = (struct xfrm_userpolicy_id*)NLMSG_DATA(hdr);
	policy_id->sel = to_delete->sel;
	policy_id->dir = direction;

	if (mark.value)
	{
		struct xfrm_mark *mrk;
		struct rtattr *rthdr = XFRM_RTA(hdr, struct xfrm_userpolicy_id);

		rthdr->rta_type = XFRMA_MARK;
		rthdr->rta_len = RTA_LENGTH(sizeof(struct xfrm_mark));
		hdr->nlmsg_len += RTA_ALIGN(rthdr->rta_len);
		if (hdr->nlmsg_len > sizeof(request))
		{
			return FAILED;
		}

		mrk = (struct xfrm_mark*)RTA_DATA(rthdr);
		mrk->v = mark.value;
		mrk->m = mark.mask;
	}

	route = to_delete->route;
	free(to_delete);

	if (this->socket_xfrm->send_ack(this->socket_xfrm, hdr) != SUCCESS)
	{
		if (mark.value)
		{
			DBG1(DBG_KNL, "unable to delete policy %R === %R %N  "
                          "(mark %u/0x%8x)", src_ts, dst_ts, policy_dir_names,
						   direction, mark.value, mark.mask);
		}
		else
		{
			DBG1(DBG_KNL, "unable to delete policy %R === %R %N",
						   src_ts, dst_ts, policy_dir_names, direction);
		}
		return FAILED;
	}

	if (route)
	{
		if (hydra->kernel_interface->del_route(hydra->kernel_interface,
				route->dst_net, route->prefixlen, route->gateway,
				route->src_ip, route->if_name) != SUCCESS)
		{
			DBG1(DBG_KNL, "error uninstalling route installed with "
						  "policy %R === %R %N", src_ts, dst_ts,
						   policy_dir_names, direction);
		}
		route_entry_destroy(route);
	}
	return SUCCESS;
}

METHOD(kernel_ipsec_t, bypass_socket, bool,
	private_kernel_netlink_ipsec_t *this, int fd, int family)
{
	struct xfrm_userpolicy_info policy;
	u_int sol, ipsec_policy;

	switch (family)
	{
		case AF_INET:
			sol = SOL_IP;
			ipsec_policy = IP_XFRM_POLICY;
			break;
		case AF_INET6:
			sol = SOL_IPV6;
			ipsec_policy = IPV6_XFRM_POLICY;
			break;
		default:
			return FALSE;
	}

	memset(&policy, 0, sizeof(policy));
	policy.action = XFRM_POLICY_ALLOW;
	policy.sel.family = family;

	policy.dir = XFRM_POLICY_OUT;
	if (setsockopt(fd, sol, ipsec_policy, &policy, sizeof(policy)) < 0)
	{
		DBG1(DBG_KNL, "unable to set IPSEC_POLICY on socket: %s",
			 strerror(errno));
		return FALSE;
	}
	policy.dir = XFRM_POLICY_IN;
	if (setsockopt(fd, sol, ipsec_policy, &policy, sizeof(policy)) < 0)
	{
		DBG1(DBG_KNL, "unable to set IPSEC_POLICY on socket: %s",
			 strerror(errno));
		return FALSE;
	}
	return TRUE;
}

METHOD(kernel_ipsec_t, destroy, void,
	private_kernel_netlink_ipsec_t *this)
{
	enumerator_t *enumerator;
	policy_entry_t *policy;

	if (this->job)
	{
		this->job->cancel(this->job);
	}
	if (this->socket_xfrm_events > 0)
	{
		close(this->socket_xfrm_events);
	}
	DESTROY_IF(this->socket_xfrm);
	enumerator = this->policies->create_enumerator(this->policies);
	while (enumerator->enumerate(enumerator, &policy, &policy))
	{
		free(policy);
	}
	enumerator->destroy(enumerator);
	this->policies->destroy(this->policies);
	this->mutex->destroy(this->mutex);
	free(this);
}

/*
 * Described in header.
 */
kernel_netlink_ipsec_t *kernel_netlink_ipsec_create()
{
	private_kernel_netlink_ipsec_t *this;
	struct sockaddr_nl addr;
	int fd;

	INIT(this,
		.public = {
			.interface = {
				.get_spi = _get_spi,
				.get_cpi = _get_cpi,
				.add_sa  = _add_sa,
				.update_sa = _update_sa,
				.query_sa = _query_sa,
				.del_sa = _del_sa,
				.add_policy = _add_policy,
				.query_policy = _query_policy,
				.del_policy = _del_policy,
				.bypass_socket = _bypass_socket,
				.destroy = _destroy,
			},
		},
		.policies = hashtable_create((hashtable_hash_t)policy_hash,
									 (hashtable_equals_t)policy_equals, 32),
		.mutex = mutex_create(MUTEX_TYPE_DEFAULT),
		.install_routes = lib->settings->get_bool(lib->settings,
					"%s.install_routes", TRUE, hydra->daemon),
		.replay_window = lib->settings->get_int(lib->settings,
					"%s.replay_window", DEFAULT_REPLAY_WINDOW, hydra->daemon),
	);

	this->replay_bmp = (this->replay_window + sizeof(u_int32_t) * 8 - 1) /
													(sizeof(u_int32_t) * 8);

	if (streq(hydra->daemon, "pluto"))
	{	/* no routes for pluto, they are installed via updown script */
		this->install_routes = FALSE;
	}

	/* disable lifetimes for allocated SPIs in kernel */
	fd = open("/proc/sys/net/core/xfrm_acq_expires", O_WRONLY);
	if (fd)
	{
		ignore_result(write(fd, "165", 3));
		close(fd);
	}

	this->socket_xfrm = netlink_socket_create(NETLINK_XFRM);
	if (!this->socket_xfrm)
	{
		destroy(this);
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;

	/* create and bind XFRM socket for ACQUIRE, EXPIRE, MIGRATE & MAPPING */
	this->socket_xfrm_events = socket(AF_NETLINK, SOCK_RAW, NETLINK_XFRM);
	if (this->socket_xfrm_events <= 0)
	{
		DBG1(DBG_KNL, "unable to create XFRM event socket");
		destroy(this);
		return NULL;
	}
	addr.nl_groups = XFRMNLGRP(ACQUIRE) | XFRMNLGRP(EXPIRE) |
					 XFRMNLGRP(MIGRATE) | XFRMNLGRP(MAPPING);
	if (bind(this->socket_xfrm_events, (struct sockaddr*)&addr, sizeof(addr)))
	{
		DBG1(DBG_KNL, "unable to bind XFRM event socket");
		destroy(this);
		return NULL;
	}
	this->job = callback_job_create((callback_job_cb_t)receive_events,
									this, NULL, NULL);
	lib->processor->queue_job(lib->processor, (job_t*)this->job);

	return &this->public;
}

