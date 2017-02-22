#include "include.h"
#include "port_knocking.h"

/* list of hidden local ports */
struct data_node *ports;

/* list of currently saved senders */
struct data_node *senders;

/* global netfilter hook */
struct nf_hook_ops netf_hook;

/* ports to knock before accepting target ports  */
static int knocking_ports[KNOCKING_LENGTH] = {2345, 3456, 4567};

struct data_node *insert_sender(u8 *ip_addr, int protocol) {

	struct sender_node *sender = kmalloc(sizeof(struct sender_node), GFP_KERNEL);

	sender->protocol = protocol;
	sender->knocking_counter = 0;

	if(protocol == ETH_P_IP) {

		debug("[ insert_sender ] INSERT SENDER %pI4", ip_addr);

		memcpy(sender->ipv4, ip_addr, IPV4_LENGTH);
	}else {

		debug("[ insert_sender ] INSERT SENDER %pI6", ip_addr);

		memcpy(sender->ipv6, ip_addr, IPV6_LENGTH);
	}

	return insert_data_node(&senders, (void *)sender);
}

int find_port(int port) {

	return find_data_node(&ports, (void *)&port, sizeof(int)) != NULL;
}

int is_knock_port(int port) {

	return (port == knocking_ports[0] || port == knocking_ports[1] || port == knocking_ports[2]);
}

int sender_accept(struct data_node *node, int protocol) {

	/* look for source address */
	if(node != NULL) {

		struct sender_node *sender = (struct sender_node *)node->data;

		/* accept packet */
		if(sender->protocol == protocol && sender->knocking_counter == KNOCKING_LENGTH) {

			debug("[ sender_accept ] ACCEPT SENDER");

			return 0;	
		}
	}

	debug("[ sender_accept ] REJECT SENDER");

	return 1;
}

int sender_knock(u8 *addr, int protocol, int port, int len) {

	struct data_node *node;

	struct sender_node *sender;

	int offset;

	if(len == IPV4_LENGTH) {

		offset = offsetof(struct sender_node, ipv4);
	}else if(len == IPV6_LENGTH) {

		offset = offsetof(struct sender_node, ipv6);
	}

	node = find_data_node_field(&senders, (void *)addr, offset, len);

	if(node == NULL) {

		debug("[ sender_knock ] SENDER NOT IN LIST, INSERT NEW SENDER");

		node = insert_sender(addr, protocol);
	}

	sender = (struct sender_node *)node->data;

	if(port == knocking_ports[sender->knocking_counter]) {

		debug("[ sender_knock ] PORT %d MATCHES, INCREASE COUNTER", port);

		sender->knocking_counter++;
	}else {

		debug("[ sender_knock ] PORT %d DOES NOT MATCH, DELETE SENDER", port);

		kfree(sender);

		delete_data_node(&senders, node);
	}

	return 1;
}

int sender_check(struct sk_buff *skb, int port) {

	struct iphdr *header_ipv4;
	struct ipv6hdr *header_ipv6;

	/* get ipv4 header */
	header_ipv4 = ip_hdr(skb);

	/* get ipv6 header */
	header_ipv6 = ipv6_hdr(skb);

	if(find_port(port)) {

		/* port is hidden */
		if(skb->protocol == htons(ETH_P_IP) && header_ipv4 != NULL) {

			struct data_node *node = find_data_node_field(&senders, (void *)&header_ipv4->saddr, offsetof(struct sender_node, ipv4), IPV4_LENGTH);

			debug("[ sender_check ] CHECK ACCEPT SENDER %pI4", &header_ipv4->saddr);

			return sender_accept(node, htons(ETH_P_IP));
		}

		if(skb->protocol == htons(ETH_P_IPV6) && header_ipv6 != NULL) {

			struct data_node *node = find_data_node_field(&senders, (void *)header_ipv6->saddr.s6_addr, offsetof(struct sender_node, ipv6), IPV6_LENGTH);

			debug("[ sender_check ] CHECK ACCEPT SENDER %pI6", header_ipv6->saddr.s6_addr);

			return sender_accept(node, htons(ETH_P_IPV6));
		}
	}else {

		/* port is not hidden */
		if(skb->protocol == htons(ETH_P_IP) && header_ipv4 != NULL) {

			debug("[ sender_check ] CHECK KNOCK SENDER %pI4", &header_ipv4->saddr);

			return sender_knock((u8 *)&header_ipv4->saddr, htons(ETH_P_IP), port, IPV4_LENGTH);
		}

		if(skb->protocol == htons(ETH_P_IPV6) && header_ipv6 != NULL) {

			debug("[ sender_check ] CHECK KNOCK SENDER %pI6", header_ipv6->saddr.s6_addr);

			return sender_knock(header_ipv6->saddr.s6_addr, htons(ETH_P_IPV6), port, IPV6_LENGTH);
		}
	}

	/* no ipv4 or ipv6 packet or not found in list */
	return NF_ACCEPT;
}

unsigned int knock_port(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {

	struct iphdr *header_ipv4;
	struct ipv6hdr *header_ipv6;
	struct tcphdr *header_tcp;

	header_ipv4 = ip_hdr(skb);
	header_ipv6 = ipv6_hdr(skb);

	if(is_empty_data_node(&ports)) {

		/* no hidden ports, accept */
		return NF_ACCEPT;
	}

	debug("[ knock_port ] KNOCK KNOCK");

	if(skb->protocol == htons(ETH_P_IP)) {

		if(header_ipv4->protocol != IPPROTO_TCP) {

			debug("[ knock_port ] IPV4 PACKET NOT UDP, ACCEPT");

			return NF_ACCEPT;
		}
	}

	if(skb->protocol == htons(ETH_P_IPV6)) {

		if(header_ipv6->nexthdr != IPPROTO_TCP) {

			debug("[ knock_port ] IPV6 PACKET NOT UDP, ACCEPT");

			return NF_ACCEPT;
		}
	}

	/* get tcp header */
	header_tcp = tcp_hdr(skb);

	/* fix for host -> guest scp file transfer when module loaded */
	if(!is_knock_port(ntohs(header_tcp->dest)) && !find_port(ntohs(header_tcp->dest))) {

		/* not a knocking port and hot a hidden port */
		return NF_ACCEPT;
	}

	if(sender_check(skb, ntohs(header_tcp->dest))) {

		if(skb->protocol == htons(ETH_P_IP)) {

			debug("[ knock_port ] IPV4 PACKET REJECTED, SEND REJECT MESSAGE");

			/* reject for ipv4 */
			nf_send_reset(state->net, skb, state->hook);

		}else if(skb->protocol == htons(ETH_P_IPV6)) {

			debug("[ knock_port ] IPV6 PACKET REJECTED, SEND REJECT MESSAGE");

			/* reject for ipv6 */
			nf_send_reset6(state->net, skb, state->hook);
		}

		debug("[ knock_port ] UNKNOWN PROTOCOL, DROP");

		return NF_DROP;
	}

	debug("[ knock_port ] PACKET ACCEPTED");

	return NF_ACCEPT;
}

void port_unhide(int port) {

	struct data_node *node = find_data_node(&ports, (void *)&port, sizeof(int));

	if(node != NULL) {

		debug("[ port_unhide ] PORT %d FOUND, DELETE FROM LIST", port);

		/* delete entry */
		delete_data_node(&ports, node);
	}

	debug("[ port_unhide ] PORT %d NOT FOUND", port);
}

void port_hide(int port) {

	int *new_port = kmalloc(sizeof(int), GFP_KERNEL);

	/* look for port in list */
	if(find_data_node(&ports, (void *)&port, sizeof(port)) != NULL) {

		debug("[ port_hide ] PORT %d ALREADY IN LIST", port);

		return;
	}

	/* insert new port */
	*new_port = port;

	debug("[ port_hide ] INSERT PORT %d IN LIST", port);

	insert_data_node(&ports, (void *)new_port);
}

int port_knocking_init(void) {

	int ret;

	debug("[ port_knocking_init ] INITIALIZING PORT KNOCKING");

	/* set flags and function for netfilter */
	netf_hook.hook = knock_port;
	netf_hook.hooknum = NF_INET_LOCAL_IN;
	netf_hook.pf = PF_INET;
	netf_hook.priority = NF_IP_PRI_FIRST;

	/* register our netfilter hook */
	ret = nf_register_hook(&netf_hook);

	if(ret < 0) {

		/* some error occurred */
		return 1;
	}

	return 0;
}

void port_knocking_exit(void) {

	debug("[ port_knocking_exit ] EXIT PORT KNOCKING");

	/* clear sender list */
	free_data_node_list(&senders);

	/* clear port list */
	free_data_node_list(&ports);

	nf_unregister_hook(&netf_hook);
}

MODULE_LICENSE("GPL");