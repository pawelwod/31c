/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_RX_QUEUES_PER_PORT 16
#define MAX_QUEUES_PER_LCORE 8
#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024

#define MAX_TIMER_PERIOD 86400 /* 1 day max */

static uint16_t g_nb_rxd = RX_DESC_DEFAULT;
static uint16_t g_nb_txd = TX_DESC_DEFAULT;
static struct rte_ether_addr g_port_dst_addr[2];
static unsigned g_queue_per_lcore = MAX_QUEUES_PER_LCORE;
static volatile bool g_force_quit;

/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t g_timer_period = 10; /* default period is 10 seconds */

struct fwd_stats {
	uint64_t rx_cnt;
	uint64_t tx_cnt;
	uint64_t drop_cnt;

	uint64_t rx_bytes;
	uint64_t tx_bytes;

	/* BPS are in TSC domain. */
	uint64_t rx_bps;
	uint64_t tx_bps;
};

struct port_conf {
	uint16_t num_q;
	struct rte_mempool *pktmbuf_pool;
	struct rte_eth_conf eth_conf;

	struct fwd_stats stats;
};

struct __rte_cache_aligned lcore_conf {
	unsigned rx_portid;
	uint16_t rx_first_q; /* First queue offset. */
	uint16_t rx_n_q; /* Number of TX/RX queues to poll. */

	unsigned tx_portid;
	uint16_t tx_first_q; /* First queue offset. */
	uint16_t tx_n_q; /* Number of TX/RX queues to poll. */

	const struct rte_ether_addr *dst_mac;

	volatile int give_stats;
};

static struct rte_eth_conf g_def_eth_conf = {
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
	},
};

static struct lcore_conf g_lcore_conf[RTE_MAX_LCORE];
static struct port_conf g_port_conf[2];

static uint16_t
l2fwd_rx(struct lcore_conf *conf, uint16_t *rx_q,
		struct rte_mbuf **pkts_burst, uint16_t burst_size)
{
	uint16_t nb_rx = rte_eth_rx_burst(conf->rx_portid, *rx_q, pkts_burst, burst_size);

	*rx_q += 1;
	if (*rx_q == conf->rx_first_q + conf->rx_n_q)
		*rx_q = conf->rx_first_q;

	return nb_rx;
}


/* Check if packet need to be dropped. For now only IP4 / IP6 packets
 * are allowed.
 */
static bool
l2fwd_is_ip_pkt(const struct rte_mbuf *m)
{
	const struct rte_ether_hdr *hdr;
	const struct rte_vlan_hdr *vlan;
	rte_be16_t ether_type;

	 hdr = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
	 ether_type = hdr->ether_type;

	if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN)) {
		vlan = rte_pktmbuf_mtod_offset(m,
				const struct rte_vlan_hdr *,
				sizeof(struct rte_ether_hdr));

		ether_type = vlan->eth_proto;
	} else if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_QINQ)) {
		vlan = rte_pktmbuf_mtod_offset(m,
				const struct rte_vlan_hdr *,
				sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr));

		ether_type = vlan->eth_proto;
	}

	if (ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
			ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6))
		return true;

	/* Drop non-IP packets. */
	return false;
}

/* Set destination address of the packet */
static void
l2fwd_set_dst(struct rte_mbuf *m, const struct rte_ether_addr *dst_mac)
{
	struct rte_ether_hdr *hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	rte_ether_addr_copy(dst_mac, &hdr->dst_addr);
}

/*
 * Transmit packets to the next port.
 * The function returns the number of packets sent.
 * The unsent packets are returned in pkts_burst. Function will updte tx_q for
 * next transmission.
 */
static uint16_t
l2fwd_fwd(struct lcore_conf *conf, uint16_t *tx_q, struct rte_mbuf **pkts_burst,
		uint16_t nb_rx, struct fwd_stats *stats)
{
	struct rte_mbuf *m, *fwd_pkts[MAX_PKT_BURST];
	uint16_t pkt_len, fwd_cnt, unsent_cnt, i;
	uint16_t tx_q_start, nb_tx;

	if (nb_rx == 0)
		return 0;

	unsent_cnt = 0;
	fwd_cnt = 0;

	/* Forward only IP packets. */
	for (i = 0, unsent_cnt = 0; i < nb_rx; i++) {
		m = pkts_burst[i];
		pkt_len = rte_pktmbuf_pkt_len(m);
		stats->rx_bytes += pkt_len;

		if (!l2fwd_is_ip_pkt(pkts_burst[i])) {
			pkts_burst[unsent_cnt++] = m;
			continue;
		}

		if (conf->dst_mac) {
			rte_mb();
			l2fwd_set_dst(m, conf->dst_mac);
		}

		fwd_pkts[fwd_cnt++] = m;
		stats->tx_bytes += pkt_len;
	}

	tx_q_start = *tx_q;
	nb_tx = 0;

	/* Use each TX queue to transmit packets.*/
	do {
		nb_tx += rte_eth_tx_burst(conf->tx_portid, *tx_q, &fwd_pkts[nb_tx], fwd_cnt);

		*tx_q += 1;
		if (*tx_q == conf->tx_first_q + conf->tx_n_q)
			*tx_q = conf->tx_first_q;

	} while(nb_tx != fwd_cnt && *tx_q != tx_q_start);

	/* We tried our best. Return unsent buffer. */
	for ( i = nb_tx; i < fwd_cnt; i++) {
		m = fwd_pkts[i];
		pkts_burst[unsent_cnt++] = m;

		/* Roll back TX bytes. */
		stats->tx_bytes -= rte_pktmbuf_pkt_len(m);
	}

	stats->rx_cnt += nb_rx;
	stats->tx_cnt += nb_tx;
	stats->drop_cnt += nb_rx - nb_tx;
	return nb_tx;
}

/* main processing loop */
static void
l2fwd_fwd_loop(void)
{
	const unsigned lcore_id = rte_lcore_id();
	const uint64_t tsc_hz = rte_get_timer_hz();
	struct lcore_conf * const conf = &g_lcore_conf[lcore_id];
	struct rte_mbuf *fwd_pkts[MAX_PKT_BURST];
	uint16_t rx_q, tx_q;
	unsigned nb_rx, nb_tx;
	struct fwd_stats stats;
	uint64_t last_tsc, cur_tsc, delta_tsc;


	if (conf->rx_n_q == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u: nothing to do\n", lcore_id);
		return;
	}

	rx_q = conf->rx_first_q;
	tx_q = conf->tx_first_q;

	RTE_LOG(INFO, L2FWD, "lcore %u: entering main loop\n"
					"		-- RX portid=%u qid=%u-%u, TX portid=%u qid=%u-%u\n",
			lcore_id,
			conf->rx_portid, conf->rx_first_q, conf->rx_first_q + conf->rx_n_q - 1,
			conf->tx_portid, conf->tx_first_q, conf->tx_first_q + conf->tx_n_q - 1);

	last_tsc = rte_rdtsc();
	while (!g_force_quit) {
		nb_rx = l2fwd_rx(conf, &rx_q, fwd_pkts, MAX_PKT_BURST);
		nb_tx = l2fwd_fwd(conf, &tx_q, fwd_pkts, nb_rx, &stats);
		rte_pktmbuf_free_bulk(fwd_pkts, nb_rx - nb_tx);

		if (conf->give_stats) {
			/* Calc RX and TX bps. */

			cur_tsc = rte_rdtsc();
			delta_tsc = cur_tsc - last_tsc;
			last_tsc = cur_tsc;

			stats.rx_bps = stats.rx_bytes * tsc_hz / delta_tsc;
			stats.tx_bps = stats.tx_bytes * tsc_hz / delta_tsc;


			/* Update stats in port_conf */
			struct port_conf * const rx_port = &g_port_conf[conf->rx_portid];
			rx_port->stats.rx_cnt += stats.rx_cnt;
			rx_port->stats.rx_bytes += stats.rx_bytes;
			rx_port->stats.drop_cnt += stats.drop_cnt;
			rx_port->stats.rx_bps += stats.rx_bps;

			struct port_conf * const tx_port = &g_port_conf[conf->tx_portid];
			tx_port->stats.tx_cnt += stats.tx_cnt;
			tx_port->stats.tx_bytes += stats.tx_bytes;
			rx_port->stats.tx_bps += stats.tx_bps;


			rte_wmb();
			conf->give_stats = 0;

			memset(&stats, 0, sizeof(stats));
		}
	}
}

/* Ask each lcore to update port stats. */
static void
update_stats(void)
{
	unsigned lcore_id;

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		struct lcore_conf * const conf = &g_lcore_conf[lcore_id];

		if (conf->rx_n_q == 0)
			continue;

		conf->give_stats = 1;

		/* Wait for the stats to be updated. */
		do
			rte_pause();
		while (conf->give_stats);
	}
}

/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	struct port_conf *port;
	struct fwd_stats total = {0};
	unsigned portid;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_DIM(g_port_conf); portid++) {
		port = &g_port_conf[portid];

		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent:     %20"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped:  %20"PRIu64
			   "\nBytes received:   %20"PRIu64
			   "\nBytes sent:       %20"PRIu64
			   "\nTotal RX bps:     %20"PRIu64
			   "\nTotal TX bps:     %20"PRIu64,
			   portid,
			   port->stats.tx_cnt,
			   port->stats.rx_cnt,
			   port->stats.drop_cnt,
			   port->stats.rx_bytes,
			   port->stats.tx_bytes,
			   port->stats.rx_bps,
			   port->stats.tx_bps);

		total.rx_cnt += port->stats.rx_cnt;
		total.tx_cnt += port->stats.tx_cnt;
		total.drop_cnt += port->stats.drop_cnt;

		total.rx_bytes += port->stats.rx_bytes;
		total.tx_bytes += port->stats.tx_bytes;

		total.rx_bps += port->stats.rx_bps;
		total.tx_bps += port->stats.tx_bps;

		port->stats.rx_bps = 0;
		port->stats.tx_bps = 0;

	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped:  %14"PRIu64
		   "\nTotal bytes received:   %14"PRIu64
		   "\nTotal bytes sent:       %14"PRIu64
		   "\nTotal RX bps:           %14"PRIu64
		   "\nTotal TX bps:           %14"PRIu64,
		   total.tx_cnt,
		   total.rx_cnt,
		   total.drop_cnt,
		   total.rx_bytes,
		   total.tx_bytes,
		   total.rx_bps,
		   total.tx_bps);
	printf("\n====================================================\n");

	fflush(stdout);
}

/* Update and print stats every timer period. */
static void
l2fwd_main_loop(void)
{
	uint64_t next_print_tsc, cur_tsc;

	if (g_timer_period == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u stats printing disabled\n", rte_lcore_id());
		return;
	}

	RTE_LOG(INFO, L2FWD, "lcore %u: entering stats loop on master core\n", rte_lcore_id());

	next_print_tsc = rte_rdtsc() + g_timer_period;
	while (!g_force_quit) {
		cur_tsc = rte_rdtsc();

		if (next_print_tsc <= cur_tsc) {
			update_stats();
			print_stats();
			next_print_tsc += g_timer_period;
		}

		rte_delay_us(100);
	}
}

static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
	if (rte_lcore_id() != rte_get_main_lcore())
		l2fwd_fwd_loop();
	else
		l2fwd_main_loop();

	return 0;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUES_PER_PORT)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
	"h"
	"q:" /* queue-per-lcore */
	"T:"  /* timer period */
	;

enum {
	/* long options mapped to a short option */
	CMD_LINE_OPT_HELP_NUM = 'h',
	CMD_LINE_OPT_TIMER_PERIOD_NUM = 'T',
	CMD_LINE_OPT_QUEUE_PER_LCORE_NUM = 'q',

	CMD_LAST_SHORT_OPTION = 255,
	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_PORT0_DST_MAC_NUM,
	CMD_LINE_OPT_PORT1_DST_MAC_NUM,

};

static const struct option lgopts[] = {
	{"help", no_argument, NULL, 'h'},
	{"port0-dst-mac", required_argument, NULL, CMD_LINE_OPT_PORT0_DST_MAC_NUM},
	{"port1-dst-mac", required_argument, NULL, CMD_LINE_OPT_PORT1_DST_MAC_NUM},
	{"timer-period", required_argument, NULL, CMD_LINE_OPT_TIMER_PERIOD_NUM},
	{"queue-per-lcore", required_argument, NULL, CMD_LINE_OPT_QUEUE_PER_LCORE_NUM},
	{NULL, 0, 0, 0}
};

static void
l2fwd_show_usage(const char *prgname)
{
    const char *help[][2] = {
    	{"-p PORTMASK",       "hexadecimal bitmask of ports to configure"},
    	{"-P",                "enable promiscuous mode on all ports"},
		{"-q NQ",             "number of queues (=ports) per lcore (default is 1)"},
    	{"-T|--timer-period", "statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)"},
    	{"--port0-dst-mac",   "first packet port destination MAC addresses"},
    	{"--port1-dst-mac",   "second packet port destination MAC addresses"},
    };

	printf("\nUsage: %s [EAL options] -- -p PORTMASK [-P] [-q NQ]\n", prgname);

	for(unsigned int i = 0; i < RTE_DIM(help); i++)
		printf("  %-19s %s\n", help[i][0], help[i][1]);

	printf("\n");
}

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, short_options,
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		case 'h':
			l2fwd_show_usage(prgname);
			return 1;
		case 'q':
			g_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (g_queue_per_lcore == 0 || g_queue_per_lcore > MAX_RX_QUEUES_PER_PORT) {
				printf("invalid queue number\n");
				l2fwd_show_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_show_usage(prgname);
				return -1;
			}
			g_timer_period = timer_secs;
			break;
	case CMD_LINE_OPT_PORT0_DST_MAC_NUM:
	case CMD_LINE_OPT_PORT1_DST_MAC_NUM:
			ret = rte_ether_unformat_addr(optarg,
					&g_port_dst_addr[opt - CMD_LINE_OPT_PORT0_DST_MAC_NUM]);
			if (ret) {
				fprintf(stderr, "invalid MAC address\n");
				l2fwd_show_usage(prgname);
				return -1;
			}
			break;

		default:
			l2fwd_show_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (g_force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (g_force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
					sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid,
				       link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		g_force_quit = true;
	}
}

static void
l2fwd_init_port(uint16_t portid)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_conf eth_conf;
	struct port_conf *port;
	int ret;

	port = &g_port_conf[portid];
	eth_conf = g_def_eth_conf;

	ret = rte_eth_dev_info_get(portid, &dev_info);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
			0, strerror(-ret));

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	eth_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;

	if (dev_info.max_rx_queues == 1)
		eth_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

	port->num_q = RTE_MIN(dev_info.max_rx_queues, MAX_RX_QUEUES_PER_PORT);

	ret = rte_eth_dev_configure(portid, port->num_q, port->num_q, &eth_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
			  ret, portid);

	port->eth_conf = eth_conf;

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &g_nb_rxd,
		   &g_nb_txd);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			 "Cannot adjust number of descriptors: err=%d, port=%u\n",
			 ret, portid);

	fflush(stdout);
}


static void
l2fwd_alloc_mempools(void)
{
	const char *pool_name[2] = {"mbuf_pool_0", "mbuf_pool_1"};
	struct rte_eth_dev_info dev_info[2];
	unsigned dev_socket[2];
	unsigned int nb_mbufs, nb_queues, nb_lcores;
	uint16_t nb_ports;
	uint16_t portid;
	int ret;

	/* Count number of cores needed. */
	ret = rte_eth_dev_info_get(0, &dev_info[0]);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
			0, strerror(-ret));

	ret = rte_eth_dev_info_get(1, &dev_info[1]);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
			1, strerror(-ret));

	dev_socket[0] = rte_eth_dev_socket_id(0);
	dev_socket[1] = rte_eth_dev_socket_id(1);

	portid = 1;

	nb_queues = RTE_MIN(dev_info[portid].max_rx_queues, 8);
	nb_lcores = RTE_MAX(dev_info[portid].max_rx_queues / g_queue_per_lcore, 1U);

	if (dev_socket[0] != dev_socket[1]) {
		nb_ports = 1;
		nb_mbufs = 2 * nb_queues * (g_nb_rxd + g_nb_txd) + MAX_PKT_BURST +
				nb_lcores * MEMPOOL_CACHE_SIZE;
		nb_mbufs = RTE_MAX(nb_mbufs, 8192U);

		g_port_conf[portid].pktmbuf_pool = rte_pktmbuf_pool_create(pool_name[portid],
				nb_mbufs, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
				dev_socket[portid]);

		if (g_port_conf[portid].pktmbuf_pool == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

		nb_lcores = 0;

	} else
		nb_ports = 2;

	portid = 0;

	nb_queues += RTE_MIN(dev_info[portid].max_rx_queues, 8);
	nb_lcores += RTE_MAX(dev_info[portid].max_rx_queues / g_queue_per_lcore, 1U);

	nb_mbufs = nb_ports * (2 * nb_queues * (g_nb_rxd + g_nb_txd) + MAX_PKT_BURST) +
			nb_lcores * MEMPOOL_CACHE_SIZE;
	nb_mbufs = RTE_MAX(nb_mbufs, 8192U);

	g_port_conf[portid].pktmbuf_pool = rte_pktmbuf_pool_create(pool_name[portid],
			nb_mbufs, MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			dev_socket[portid]);

	if (g_port_conf[portid].pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	if (dev_socket[0] == dev_socket[1])
		g_port_conf[1].pktmbuf_pool = g_port_conf[0].pktmbuf_pool;
}

static void
l2fwd_init_port_queues(uint16_t portid)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;
	struct port_conf *port = &g_port_conf[portid];
	uint16_t qid;
	int ret;

	ret = rte_eth_dev_info_get(portid, &dev_info);
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Error during getting device (port %u) info: %s\n",
			0, strerror(-ret));

	fflush(stdout);
	rxq_conf = dev_info.default_rxconf;
	rxq_conf.offloads = port->eth_conf.rxmode.offloads;

	for (qid = 0; qid < port->num_q; qid++) {
		ret = rte_eth_rx_queue_setup(portid, qid, g_nb_rxd,
						 rte_eth_dev_socket_id(portid),
						 &rxq_conf,
						 port->pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u, qid=%u\n",
				  ret, portid, qid);

		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = port->eth_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, qid, g_nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u, qid=%u\n",
				ret, portid, qid);
	}
}

unsigned
find_free_lcore(uint16_t socketid)
{
	unsigned free_lcore_id = RTE_MAX_LCORE;
	unsigned lcore_id;
	uint16_t lcore_socketid;

	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		lcore_socketid = rte_lcore_to_socket_id(lcore_id);

		if (g_lcore_conf[lcore_id].rx_n_q)
			continue;

		if (free_lcore_id != RTE_MAX_LCORE)
			free_lcore_id = lcore_id;

		if (lcore_socketid != socketid)
			continue;

		return lcore_id;
	}

	if (free_lcore_id == RTE_MAX_LCORE)
		rte_exit(EXIT_FAILURE, "Not enough lcores\n");

	printf("Not enough lcores on socket %u, using lcore %u on socket %u\n",
		socketid, lcore_id, free_lcore_id);

	return lcore_id;
}

static void
l2fwd_init_lcore_conf(uint16_t rx_portid, uint16_t tx_portid)
{
	const struct port_conf *rx_port =  &g_port_conf[rx_portid];
	const struct port_conf *tx_port = &g_port_conf[tx_portid];
	struct lcore_conf *conf;
	unsigned lcore_id;

	uint16_t rx_qid, tx_qid, rx_num_q, tx_num_q;
	uint16_t num_rx_cores = RTE_MAX((rx_port->num_q + g_queue_per_lcore - 1) / g_queue_per_lcore, 1U);
	uint16_t num_rx_q_per_core = rx_port->num_q / num_rx_cores;
	uint16_t num_tx_q_per_core = tx_port->num_q / num_rx_cores;

	if (num_tx_q_per_core == 0)
		rte_exit(EXIT_FAILURE, "Not enough TX queues in port %u\n", tx_portid);

	rx_qid = 0;
	tx_qid = 0;
	rx_num_q = rx_port->num_q;
	tx_num_q = tx_port->num_q;

	while (rx_qid < rx_port->num_q) {
		lcore_id = find_free_lcore(rte_eth_dev_socket_id(rx_portid));

		conf = &g_lcore_conf[lcore_id];

		conf->rx_portid = rx_portid;
		conf->rx_first_q = rx_qid;
		conf->rx_n_q = RTE_MIN(num_rx_q_per_core, rx_num_q);

		conf->tx_portid = tx_portid;
		conf->tx_first_q = tx_qid;
		conf->tx_n_q = RTE_MIN(num_tx_q_per_core, tx_num_q);

		if (!rte_is_zero_ether_addr(&g_port_dst_addr[tx_portid]))
			conf->dst_mac = &g_port_dst_addr[tx_portid];

		rx_qid += conf->rx_n_q;
		tx_qid += conf->tx_n_q;
		rx_num_q -= conf->rx_n_q;
		tx_num_q -= conf->tx_n_q;
	}
}

static void
l2fwd_start_port(uint16_t portid)
{
	int ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL, 0);
	if (ret < 0)
		printf("Port %u, Failed to disable Ptype parsing\n", portid);

	/* Start device */
	ret = rte_eth_dev_start(portid);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
			  ret, portid);

	ret = rte_eth_promiscuous_enable(portid);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"rte_eth_promiscuous_enable:err=%s, port=%u\n",
			rte_strerror(-ret), portid);

	struct rte_ether_addr mac_addr;
	ret = rte_eth_macaddr_get(portid, &mac_addr);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			 "Cannot get MAC address: err=%d, port=%u\n",
			 ret, portid);

	printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
		portid, RTE_ETHER_ADDR_BYTES(&mac_addr));
}
int
main(int argc, char **argv)
{
	int ret;
	uint16_t nb_ports, portid;
	unsigned lcore_id;

	rte_set_application_usage_hook(l2fwd_show_usage);
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	g_force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	g_timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports != 2)
		rte_exit(EXIT_FAILURE, "Exactly two ethernet ports required but have %" PRIu16" - bye\n",
				nb_ports);

	l2fwd_init_port(0);
	l2fwd_init_port(1);
	l2fwd_alloc_mempools();

	l2fwd_init_port_queues(0);
	l2fwd_init_port_queues(1);

	l2fwd_init_lcore_conf(0, 1);
	l2fwd_init_lcore_conf(1, 0);

	l2fwd_start_port(0);
	l2fwd_start_port(1);

	check_all_ports_link_status(0x3);

	ret = 0;

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MAIN);
	RTE_LCORE_FOREACH(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%d, port=%d\n", ret, portid);

		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	/* clean up the EAL */
	rte_eal_cleanup();
	printf("Bye...\n");

	return ret;
}
