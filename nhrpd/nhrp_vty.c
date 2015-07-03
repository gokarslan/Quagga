/* NHRP vty handling
 * Copyright (c) 2014-2015 Timo Teräs
 *
 * This file is free software: you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "zebra.h"
#include "command.h"
#include "zclient.h"
#include "stream.h"

#include "nhrpd.h"
#include "netlink.h"

static struct cmd_node zebra_node = {
	.node   = ZEBRA_NODE,
	.prompt = "%s(config-router)# ",
	.vtysh  = 1,
};

static struct cmd_node nhrp_interface_node = {
	.node   = INTERFACE_NODE,
	.prompt = "%s(config-if)# ",
	.vtysh  = 1,
};

static const struct message debug_flags_desc[] = {
	{ NHRP_DEBUG_ALL, "all" },
	{ NHRP_DEBUG_COMMON, "common" },
	{ NHRP_DEBUG_IF, "interface" },
	{ NHRP_DEBUG_KERNEL, "kernel" },
	{ NHRP_DEBUG_ROUTE, "route" },
	{ NHRP_DEBUG_VICI, "vici" },
	{ 0, NULL },
};

static const struct message interface_flags_desc[] = {
	{ NHRP_IFF_SHORTCUT, "shortcut" },
	{ NHRP_IFF_REDIRECT, "redirect" },
	{ NHRP_IFF_REG_NO_UNIQUE, "registration no-unique" },
	{ 0, NULL },
};

static int nhrp_vty_return(struct vty *vty, int ret)
{
	static const char * const errmsgs[] = {
		[NHRP_ERR_FAIL]				= "Command failed",
		[NHRP_ERR_NO_MEMORY]			= "Out of memory",
		[NHRP_ERR_UNSUPPORTED_INTERFACE]	= "NHRP not supported on this interface",
		[NHRP_ERR_NHRP_NOT_ENABLED]		= "NHRP not enabled (set 'nhrp network-id' first)",
		[NHRP_ERR_ENTRY_EXISTS]			= "Entry exists already",
		[NHRP_ERR_ENTRY_NOT_FOUND]		= "Entry not found",
		[NHRP_ERR_PROTOCOL_ADDRESS_MISMATCH]	= "Protocol address family does not match command (ip/ipv6 mismatch)",
	};
	const char *str = NULL;
	char buf[256];

	if (ret == NHRP_OK)
		return CMD_SUCCESS;

	if (ret > 0 && ret <= (int)ZEBRA_NUM_OF(errmsgs))
		if (errmsgs[ret])
			str = errmsgs[ret];

	if (!str) {
		str = buf;
		snprintf(buf, sizeof(buf), "Unknown error %d", ret);
	}

	vty_out (vty, "%% %s%s", str, VTY_NEWLINE);

	return CMD_WARNING;
}

static int toggle_flag(
	struct vty *vty, const struct message *flag_desc,
	const char *name, int on_off, unsigned *flags)
{
	int i;

	for (i = 0; flag_desc[i].str != NULL; i++) {
		if (strcmp(flag_desc[i].str, name) != 0)
			continue;
		if (on_off)
			*flags |= flag_desc[i].key;
		else
			*flags &= ~flag_desc[i].key;
		return CMD_SUCCESS;
	}

	vty_out(vty, "%% Invalid value %s%s", name, VTY_NEWLINE);
	return CMD_WARNING;
}

#ifndef NO_DEBUG

DEFUN(show_debugging_nhrp, show_debugging_nhrp_cmd,
	"show debugging nhrp",
	SHOW_STR
	"Debugging information\n"
	"NHRP configuration\n")
{
	int i;

	vty_out(vty, "NHRP debugging status:%s", VTY_NEWLINE);

	for (i = 0; debug_flags_desc[i].str != NULL; i++) {
		if (debug_flags_desc[i].key == NHRP_DEBUG_ALL)
			continue;
		if (!(debug_flags_desc[i].key & debug_flags))
			continue;

		vty_out(vty, "  NHRP %s debugging is on%s",
			debug_flags_desc[i].str, VTY_NEWLINE);
	}

	return CMD_SUCCESS;
}

DEFUN(debug_nhrp, debug_nhrp_cmd,
	"debug nhrp (all|common|interface|kernel|route|vici)",
	"Enable debug messages for specific or all parts.\n"
	"NHRP information\n"
	"All messages\n"
	"Common messages (default)\n"
	"Interface messages\n"
	"Kernel messages\n"
	"Route messages\n"
	"VICI messages\n")
{
	return toggle_flag(vty, debug_flags_desc, argv[0], 1, &debug_flags);
}

DEFUN(no_debug_nhrp, no_debug_nhrp_cmd,
	"no debug nhrp (all|common|interface|kernel|route|vici)",
	NO_STR
	"Disable debug messages for specific or all parts.\n"
	"NHRP information\n"
	"All messages\n"
	"Common messages (default)\n"
	"Interface messages\n"
	"Kernel messages\n"
	"Route messages\n"
	"VICI messages\n")
{
	return toggle_flag(vty, debug_flags_desc, argv[0], 0, &debug_flags);
}

#endif /* NO_DEBUG */

static int nhrp_config_write(struct vty *vty)
{
#ifndef NO_DEBUG
	if (debug_flags == NHRP_DEBUG_ALL) {
		vty_out(vty, "debug nhrp all%s", VTY_NEWLINE);
	} else {
		int i;

		for (i = 0; debug_flags_desc[i].str != NULL; i++) {
			if (debug_flags_desc[i].key == NHRP_DEBUG_ALL)
				continue;
			if (!(debug_flags & debug_flags_desc[i].key))
				continue;
			vty_out(vty, "debug nhrp %s%s", debug_flags_desc[i].str, VTY_NEWLINE);
		}
	}
	vty_out(vty, "!%s", VTY_NEWLINE);
#endif /* NO_DEBUG */

	if (nhrp_event_socket_path) {
		vty_out(vty, "nhrp event socket %s%s",
			nhrp_event_socket_path, VTY_NEWLINE);
	}
	if (netlink_nflog_group) {
		vty_out(vty, "nhrp nflog-group %d%s",
			netlink_nflog_group, VTY_NEWLINE);
	}

	return 0;
}

#define IP_STR		"IP information\n"
#define IPV6_STR	"IPv6 information\n"
#define AFI_CMD		"(ip|ipv6)"
#define AFI_STR		IP_STR IPV6_STR
#define NHRP_STR	"Configure Next Hop Resolution Protocol\n"

static afi_t cmd_to_afi(const char *cmd)
{
	return strncmp(cmd, "ipv6", 4) == 0 ? AFI_IP6 : AFI_IP;
}

static const char *afi_to_cmd(afi_t afi)
{
	if (afi == AFI_IP6) return "ipv6";
	return "ip";
}

DEFUN(nhrp_event_socket, nhrp_event_socket_cmd,
	"nhrp event socket SOCKET",
	NHRP_STR
	"Event Manager commands\n"
	"Event Manager unix socket path\n"
	"Unix path for the socket")
{
	evmgr_set_socket(argv[0]);
	return CMD_SUCCESS;
}

DEFUN(no_nhrp_event_socket, no_nhrp_event_socket_cmd,
	"no nhrp event socket [SOCKET]",
	NO_STR
	NHRP_STR
	"Event Manager commands\n"
	"Event Manager unix socket path\n"
	"Unix path for the socket")
{
	evmgr_set_socket(NULL);
	return CMD_SUCCESS;
}

DEFUN(nhrp_nflog_group, nhrp_nflog_group_cmd,
	"nhrp nflog-group <1-65535>",
	NHRP_STR
	"Specify NFLOG group number\n"
	"NFLOG group number\n")
{
	uint32_t nfgroup;

	VTY_GET_INTEGER_RANGE("nflog-group", nfgroup, argv[0], 1, 65535);
	netlink_set_nflog_group(nfgroup);

	return CMD_SUCCESS;
}

DEFUN(no_nhrp_nflog_group, no_nhrp_nflog_group_cmd,
	"no nhrp nflog-group [<1-65535>]",
	NO_STR
	NHRP_STR
	"Specify NFLOG group number\n"
	"NFLOG group number\n")
{
	netlink_set_nflog_group(0);
	return CMD_SUCCESS;
}

DEFUN(tunnel_protection, tunnel_protection_cmd,
	"tunnel protection vici profile PROFILE {fallback-profile FALLBACK}",
	"NHRP/GRE integration\n"
	"IPsec protection\n"
	"VICI (StrongSwan)\n"
	"IPsec profile\n"
	"IPsec profile name\n"
	"Fallback IPsec profile\n"
	"Fallback IPsec profile name\n")
{
	struct interface *ifp = vty->index;

	nhrp_interface_set_protection(ifp, argv[0], argv[1]);
	return CMD_SUCCESS;
}

DEFUN(no_tunnel_protection, no_tunnel_protection_cmd,
	"tunnel protection",
	"NHRP/GRE integration\n"
	"IPsec protection\n")
{
	struct interface *ifp = vty->index;

	nhrp_interface_set_protection(ifp, NULL, NULL);
	return CMD_SUCCESS;
}

DEFUN(if_nhrp_network_id, if_nhrp_network_id_cmd,
	AFI_CMD " nhrp network-id <1-4294967295>",
	AFI_STR
	NHRP_STR
	"Enable NHRP and specify network-id\n"
	"System local ID to specify interface group\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	VTY_GET_INTEGER_RANGE("network-id", nifp->afi[afi].network_id, argv[1], 1, 4294967295);
	nhrp_interface_update(ifp);

	return CMD_SUCCESS;
}

DEFUN(if_no_nhrp_network_id, if_no_nhrp_network_id_cmd,
	"no " AFI_CMD " nhrp network-id [<1-4294967295>]",
	NO_STR
	AFI_STR
	NHRP_STR
	"Enable NHRP and specify network-id\n"
	"System local ID to specify interface group\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	nifp->afi[afi].network_id = 0;
	nhrp_interface_update(ifp);

	return CMD_SUCCESS;
}

DEFUN(if_nhrp_flags, if_nhrp_flags_cmd,
	AFI_CMD " nhrp (shortcut|redirect)",
	AFI_STR
	NHRP_STR
	"Allow shortcut establishment\n"
	"Send redirect notifications\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	return toggle_flag(vty, interface_flags_desc, argv[1], 1, &nifp->afi[afi].flags);
}

DEFUN(if_no_nhrp_flags, if_no_nhrp_flags_cmd,
	"no " AFI_CMD " nhrp (shortcut|redirect)",
	NO_STR
	AFI_STR
	NHRP_STR
	"Allow shortcut establishment\n"
	"Send redirect notifications\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	return toggle_flag(vty, interface_flags_desc, argv[1], 0, &nifp->afi[afi].flags);
}

DEFUN(if_nhrp_reg_flags, if_nhrp_reg_flags_cmd,
	AFI_CMD " nhrp registration (no-unique)",
	AFI_STR
	NHRP_STR
	"Registration configuration\n"
	"Don't set unique flag\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);
	char name[256];
	snprintf(name, sizeof(name), "registration %s", argv[1]);
	return toggle_flag(vty, interface_flags_desc, name, 1, &nifp->afi[afi].flags);
}

DEFUN(if_no_nhrp_reg_flags, if_no_nhrp_reg_flags_cmd,
	"no " AFI_CMD " nhrp registration (no-unique)",
	NO_STR
	AFI_STR
	NHRP_STR
	"Registration configuration\n"
	"Don't set unique flag\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);
	char name[256];
	snprintf(name, sizeof(name), "registration %s", argv[1]);
	return toggle_flag(vty, interface_flags_desc, name, 0, &nifp->afi[afi].flags);
}

DEFUN(if_nhrp_holdtime, if_nhrp_holdtime_cmd,
	AFI_CMD " nhrp holdtime <1-65000>",
	AFI_STR
	NHRP_STR
	"Specify NBMA address validity time\n"
	"Time in seconds that NBMA addresses are advertised valid\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	VTY_GET_INTEGER_RANGE("holdtime", nifp->afi[afi].holdtime, argv[1], 1, 65000);
	nhrp_interface_update(ifp);

	return CMD_SUCCESS;
}

DEFUN(if_no_nhrp_holdtime, if_no_nhrp_holdtime_cmd,
	"no " AFI_CMD " nhrp holdtime [1-65000]",
	NO_STR
	AFI_STR
	NHRP_STR
	"Specify NBMA address validity time\n"
	"Time in seconds that NBMA addresses are advertised valid\n")
{
	struct interface *ifp = vty->index;
	struct nhrp_interface *nifp = ifp->info;
	afi_t afi = cmd_to_afi(argv[0]);

	nifp->afi[afi].holdtime = NHRPD_DEFAULT_HOLDTIME;
	nhrp_interface_update(ifp);

	return CMD_SUCCESS;
}

DEFUN(if_nhrp_map, if_nhrp_map_cmd,
	AFI_CMD " nhrp map (A.B.C.D|X:X::X:X) (A.B.C.D|local)",
	AFI_STR
	NHRP_STR
	"Nexthop Server configuration\n"
	"IPv4 protocol address\n"
	"IPv6 protocol address\n"
	"IPv4 NBMA address\n"
	"Handle protocol address locally\n")
{
	struct interface *ifp = vty->index;
	afi_t afi = cmd_to_afi(argv[0]);
	union sockunion proto_addr, nbma_addr;
	struct nhrp_cache *c;

	if (str2sockunion(argv[1], &proto_addr) < 0 ||
	    afi2family(afi) != sockunion_family(&proto_addr))
		return nhrp_vty_return(vty, NHRP_ERR_PROTOCOL_ADDRESS_MISMATCH);

	c = nhrp_cache_get(ifp, &proto_addr, 1);
	if (!c)
		return nhrp_vty_return(vty, NHRP_ERR_FAIL);

	c->map = 1;
	if (strcmp(argv[2], "local") == 0) {
		nhrp_cache_update_binding(c, NHRP_CACHE_LOCAL, 0, NULL, 0, NULL);
	} else{
		if (str2sockunion(argv[2], &nbma_addr) < 0)
			return nhrp_vty_return(vty, NHRP_ERR_FAIL);
		nhrp_cache_update_binding(c, NHRP_CACHE_STATIC, 0,
			nhrp_peer_get(ifp, &nbma_addr), 0, NULL);
	}

	return CMD_SUCCESS;
}

DEFUN(if_nhrp_nhs, if_nhrp_nhs_cmd,
	AFI_CMD " nhrp nhs (A.B.C.D|X:X::X:X|dynamic) nbma (A.B.C.D|FQDN)",
	AFI_STR
	NHRP_STR
	"Nexthop Server configuration\n"
	"IPv4 protocol address\n"
	"IPv6 protocol address\n"
	"Automatic detection of protocol address\n"
	"IPv4 NBMA address\n"
	"Fully qualified domain name for NBMA address(es)\n")
{
	struct interface *ifp = vty->index;
	afi_t afi = cmd_to_afi(argv[0]);
	union sockunion proto_addr;
	int ret;

	if (str2sockunion(argv[1], &proto_addr) < 0)
		sockunion_family(&proto_addr) = AF_UNSPEC;

	ret = nhrp_nhs_add(ifp, afi, &proto_addr, argv[2]);
	return nhrp_vty_return(vty, ret);
}

DEFUN(if_no_nhrp_nhs, if_no_nhrp_nhs_cmd,
	"no " AFI_CMD " nhrp nhs (A.B.C.D|X:X::X:X|dynamic) nbma (A.B.C.D|FQDN)",
	NO_STR
	AFI_STR
	NHRP_STR
	"Nexthop Server configuration\n"
	"IPv4 protocol address\n"
	"IPv6 protocol address\n"
	"Automatic detection of protocol address\n"
	"IPv4 NBMA address\n"
	"Fully qualified domain name for NBMA address(es)\n")
{
	struct interface *ifp = vty->index;
	afi_t afi = cmd_to_afi(argv[0]);
	union sockunion proto_addr;
	int ret;

	if (str2sockunion(argv[1], &proto_addr) < 0)
		sockunion_family(&proto_addr) = AF_UNSPEC;

	ret = nhrp_nhs_del(ifp, afi, &proto_addr, argv[2]);
	return nhrp_vty_return(vty, ret);
}

struct show_info_ctx {
	struct vty *vty;
	afi_t afi;
	int first;
};

static void show_ip_nhrp_cache(struct nhrp_cache *c, void *pctx)
{
	struct show_info_ctx *ctx = pctx;
	struct vty *vty = ctx->vty;
	char buf[2][SU_ADDRSTRLEN];

	if (ctx->afi != family2afi(sockunion_family(&c->remote_addr)))
		return;

	if (ctx->first) {
		vty_out(vty, "%-8s %-8s %-24s %-24s %-6s %s%s",
			"Iface",
			"Type",
			"Protocol",
			"NBMA",
			"Flags",
			"Identity",
			VTY_NEWLINE);
		ctx->first = 0;
	}

	vty_out(ctx->vty, "%-8s %-8s %-24s %-24s %c      %s%s",
		c->ifp->name,
		nhrp_cache_type_str[c->cur.type],
		sockunion2str(&c->remote_addr, buf[0], sizeof buf[0]),
		c->cur.peer ? sockunion2str(&c->cur.peer->vc->remote.nbma, buf[1], sizeof buf[1]) : "-",
		c->used ? 'U' : ' ',
		c->cur.peer ? c->cur.peer->vc->remote.id : "-",
		VTY_NEWLINE);
}

static void show_ip_opennhrp_cache(struct nhrp_cache *c, void *pctx)
{
	struct show_info_ctx *ctx = pctx;
	struct vty *vty = ctx->vty;
	char buf[SU_ADDRSTRLEN];

	if (ctx->afi != family2afi(sockunion_family(&c->remote_addr)))
		return;

	vty_out(ctx->vty,
		"Type: %s%s"
		"Flags:%s%s%s"
		"Protocol-Address: %s/%zu%s",
		nhrp_cache_type_str[c->cur.type],
		VTY_NEWLINE,
		(c->cur.peer && c->cur.peer->online) ? " up": "",
		c->used ? " used": "",
		VTY_NEWLINE,
		sockunion2str(&c->remote_addr, buf, sizeof buf),
		8 * family2addrsize(sockunion_family(&c->remote_addr)),
		VTY_NEWLINE);

	if (c->cur.peer) {
		vty_out(ctx->vty,
			"NBMA-Address: %s%s",
			sockunion2str(&c->cur.peer->vc->remote.nbma, buf, sizeof buf),
			VTY_NEWLINE);
	}

	if (sockunion_family(&c->cur.remote_nbma_natoa) != AF_UNSPEC) {
		vty_out(ctx->vty,
			"NBMA-NAT-OA-Address: %s%s",
			sockunion2str(&c->cur.remote_nbma_natoa, buf, sizeof buf),
			VTY_NEWLINE);
	}

	vty_out(ctx->vty, "%s", VTY_NEWLINE);
}

static void show_ip_nhrp_shortcut(struct nhrp_shortcut *s, void *pctx)
{
	struct show_info_ctx *ctx = pctx;
	struct nhrp_cache *c;
	struct vty *vty = ctx->vty;
	char buf1[PREFIX_STRLEN], buf2[SU_ADDRSTRLEN];

	if (ctx->first) {
		vty_out(vty, "%-8s %-24s %-24s %s%s",
			"Type",
			"Prefix",
			"Via",
			"Identity",
			VTY_NEWLINE);
		ctx->first = 0;
	}

	c = s->cache;
	vty_out(ctx->vty, "%-8s %-24s %-24s %s%s",
		nhrp_cache_type_str[s->type],
		prefix2str(s->p, buf1, sizeof buf1),
		c ? sockunion2str(&c->remote_addr, buf2, sizeof buf2) : "",
		(c && c->cur.peer) ? c->cur.peer->vc->remote.id : "",
		VTY_NEWLINE);
}

DEFUN(show_ip_nhrp, show_ip_nhrp_cmd,
	"show " AFI_CMD " nhrp (cache|shortcut|opennhrp|)",
	SHOW_STR
	AFI_STR
	"NHRP information\n"
	"Forwarding cache information\n"
	"Shortcut information\n"
	"opennhrpctl style cache dump\n")
{
	struct listnode *node;
	struct interface *ifp;
	struct show_info_ctx ctx = {
		.vty = vty,
		.afi = cmd_to_afi(argv[0]),
		.first = 1,
	};

	if (!argv[1] || argv[1][0] == 'c') {
		for (ALL_LIST_ELEMENTS_RO(iflist, node, ifp))
			nhrp_cache_foreach(ifp, show_ip_nhrp_cache, &ctx);
	} else if (argv[1][0] == 'o') {
		vty_out(vty, "Status: ok%s%s", VTY_NEWLINE, VTY_NEWLINE);
		ctx.first = 0;
		for (ALL_LIST_ELEMENTS_RO(iflist, node, ifp))
			nhrp_cache_foreach(ifp, show_ip_opennhrp_cache, &ctx);
	} else {
		nhrp_shortcut_foreach(ctx.afi, show_ip_nhrp_shortcut, &ctx);
	}

	if (ctx.first) {
		vty_out(vty, "%% No entries%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

static void show_dmvpn_entry(struct nhrp_vc *vc, void *ctx)
{
	struct vty *vty = ctx;
	char buf[2][SU_ADDRSTRLEN];

	vty_out(vty, "%-24s %-24s %c      %-4d %-24s%s",
		sockunion2str(&vc->local.nbma, buf[0], sizeof buf[0]),
		sockunion2str(&vc->remote.nbma, buf[1], sizeof buf[1]),
		notifier_active(&vc->notifier_list) ? 'n' : ' ',
		vc->ipsec,
		vc->remote.id,
		VTY_NEWLINE);
}

DEFUN(show_dmvpn, show_dmvpn_cmd,
	"show dmvpn",
	SHOW_STR
	"DMVPN information\n")
{
	vty_out(vty, "%-24s %-24s %-6s %-4s %-24s%s",
		"Src",
		"Dst",
		"Flags",
		"SAs",
		"Identity",
		VTY_NEWLINE);

	nhrp_vc_foreach(show_dmvpn_entry, vty);

	return CMD_SUCCESS;
}

struct write_map_ctx {
	struct vty *vty;
	int family;
	const char *aficmd;
};

static void interface_config_write_nhrp_map(struct nhrp_cache *c, void *data)
{
	struct write_map_ctx *ctx = data;
	struct vty *vty = ctx->vty;
	char buf[2][SU_ADDRSTRLEN];

	if (!c->map) return;
	if (sockunion_family(&c->remote_addr) != ctx->family) return;

	vty_out(vty, " %s nhrp map %s %s%s",
		ctx->aficmd,
		sockunion2str(&c->remote_addr, buf[0], sizeof buf[0]),
		c->cur.type == NHRP_CACHE_LOCAL ? "local" :
		sockunion2str(&c->cur.peer->vc->remote.nbma, buf[1], sizeof buf[1]),
		VTY_NEWLINE);
}

static int interface_config_write(struct vty *vty)
{
	struct write_map_ctx mapctx;
	struct listnode *node;
	struct interface *ifp;
	struct nhrp_interface *nifp;
	struct nhrp_nhs *nhs;
	const char *aficmd;
	afi_t afi;
	char buf[SU_ADDRSTRLEN];
	int i;

	for (ALL_LIST_ELEMENTS_RO(iflist, node, ifp)) {
		vty_out(vty, "interface %s%s", ifp->name, VTY_NEWLINE);
		if (ifp->desc)
			vty_out(vty, " description %s%s", ifp->desc, VTY_NEWLINE);

		nifp = ifp->info;
		if (nifp->ipsec_profile) {
			vty_out(vty, " tunnel protection vici profile %s",
				nifp->ipsec_profile);
			if (nifp->ipsec_fallback_profile)
				vty_out(vty, " fallback-profile %s",
					nifp->ipsec_fallback_profile);
			vty_out(vty, "%s", VTY_NEWLINE);
		}

		for (afi = 0; afi < AFI_MAX; afi++) {
			struct nhrp_afi_data *ad = &nifp->afi[afi];

			aficmd = afi_to_cmd(afi);

			if (ad->network_id)
				vty_out(vty, " %s nhrp network-id %u%s",
					aficmd, ad->network_id,
					VTY_NEWLINE);

			if (ad->holdtime != NHRPD_DEFAULT_HOLDTIME)
				vty_out(vty, " %s nhrp holdtime %u%s",
					aficmd, ad->holdtime,
					VTY_NEWLINE);

			for (i = 0; interface_flags_desc[i].str != NULL; i++) {
				if (!(ad->flags & interface_flags_desc[i].key))
					continue;
				vty_out(vty, " %s nhrp %s%s",
					aficmd, interface_flags_desc[i].str, VTY_NEWLINE);
			}

			mapctx = (struct write_map_ctx) {
				.vty = vty,
				.family = afi2family(afi),
				.aficmd = aficmd,
			};
			nhrp_cache_foreach(ifp, interface_config_write_nhrp_map, &mapctx);

			list_for_each_entry(nhs, &ad->nhslist_head, nhslist_entry) {
				vty_out(vty, " %s nhrp nhs %s nbma %s%s",
					aficmd,
					sockunion_family(&nhs->proto_addr) == AF_UNSPEC ? "dynamic" : sockunion2str(&nhs->proto_addr, buf, sizeof buf),
					nhs->nbma_fqdn,
					VTY_NEWLINE);
			}
		}

		vty_out (vty, "!%s", VTY_NEWLINE);
	}

	return 0;
}

void nhrp_config_init(void)
{
	install_node(&zebra_node, nhrp_config_write);
	install_default(ZEBRA_NODE);

	/* global commands */
	install_element(VIEW_NODE, &show_debugging_nhrp_cmd);
	install_element(VIEW_NODE, &show_ip_nhrp_cmd);
	install_element(VIEW_NODE, &show_dmvpn_cmd);
	install_element(ENABLE_NODE, &show_debugging_nhrp_cmd);
	install_element(ENABLE_NODE, &show_ip_nhrp_cmd);
	install_element(ENABLE_NODE, &show_dmvpn_cmd);

	install_element(ENABLE_NODE, &debug_nhrp_cmd);
	install_element(ENABLE_NODE, &no_debug_nhrp_cmd);

	install_element(CONFIG_NODE, &debug_nhrp_cmd);
	install_element(CONFIG_NODE, &no_debug_nhrp_cmd);

	install_element(CONFIG_NODE, &nhrp_event_socket_cmd);
	install_element(CONFIG_NODE, &no_nhrp_event_socket_cmd);
	install_element(CONFIG_NODE, &nhrp_nflog_group_cmd);
	install_element(CONFIG_NODE, &no_nhrp_nflog_group_cmd);

	/* interface specific commands */
	install_node(&nhrp_interface_node, interface_config_write);
	install_default(INTERFACE_NODE);

	install_element(CONFIG_NODE, &interface_cmd);
	install_element(CONFIG_NODE, &no_interface_cmd);
	install_element(INTERFACE_NODE, &interface_cmd);
	install_element(INTERFACE_NODE, &no_interface_cmd);
	install_element(INTERFACE_NODE, &tunnel_protection_cmd);
	install_element(INTERFACE_NODE, &no_tunnel_protection_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_network_id_cmd);
	install_element(INTERFACE_NODE, &if_no_nhrp_network_id_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_holdtime_cmd);
	install_element(INTERFACE_NODE, &if_no_nhrp_holdtime_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_flags_cmd);
	install_element(INTERFACE_NODE, &if_no_nhrp_flags_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_reg_flags_cmd);
	install_element(INTERFACE_NODE, &if_no_nhrp_reg_flags_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_map_cmd);
	install_element(INTERFACE_NODE, &if_nhrp_nhs_cmd);
	install_element(INTERFACE_NODE, &if_no_nhrp_nhs_cmd);
}