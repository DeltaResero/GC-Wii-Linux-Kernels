/*
 * Binary Increase Congestion control for TCP
 *
 * This is from the implementation of BICTCP in
 * Lison-Xu, Kahaled Harfoush, and Injong Rhee.
 *  "Binary Increase Congestion Control for Fast, Long Distance
 *  Networks" in InfoComm 2004
 * Available from:
 *  http://www.csc.ncsu.edu/faculty/rhee/export/bitcp.pdf
 *
 * Unless BIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <net/tcp.h>


#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define BICTCP_B		4	 /*
					  * In binary search,
					  * go to point (max+min)/N
					  */

static int fast_convergence = 1;
static int max_increment = 32;
static int low_window = 14;
static int beta = 819;		/* = 819/1024 (BICTCP_BETA_SCALE) */
static int low_utilization_threshold = 153;
static int low_utilization_period = 2;
static int initial_ssthresh = 100;
static int smooth_part = 20;

module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(max_increment, int, 0644);
MODULE_PARM_DESC(max_increment, "Limit on increment allowed during binary search");
module_param(low_window, int, 0644);
MODULE_PARM_DESC(low_window, "lower bound on congestion window (for TCP friendliness)");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(low_utilization_threshold, int, 0644);
MODULE_PARM_DESC(low_utilization_threshold, "percent (scaled by 1024) for low utilization mode");
module_param(low_utilization_period, int, 0644);
MODULE_PARM_DESC(low_utilization_period, "if average delay exceeds then goto to low utilization mode (seconds)");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(smooth_part, int, 0644);
MODULE_PARM_DESC(smooth_part, "log(B/(B*Smin))/log(B/(B-1))+B, # of RTT from Wmax-B to Wmax");


/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32 	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	loss_cwnd;	/* congestion window at last loss */
	u32	last_cwnd;	/* the last snd_cwnd */
	u32	last_time;	/* time when updated last_cwnd */
	u32	delay_min;	/* min delay */
	u32	delay_max;	/* max delay */
	u32	last_delay;
	u8	low_utilization;/* 0: high; 1: low */
	u32	low_utilization_start;	/* starting time of low utilization detection*/
	u32	epoch_start;	/* beginning of an epoch */
#define ACK_RATIO_SHIFT	4
	u32	delayed_ack;	/* estimate the ratio of Packets/ACKs << 4 */
};

static inline void bictcp_reset(struct bictcp *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->loss_cwnd = 0;
	ca->last_cwnd = 0;
	ca->last_time = 0;
	ca->delay_min = 0;
	ca->delay_max = 0;
	ca->last_delay = 0;
	ca->low_utilization = 0;
	ca->low_utilization_start = 0;
	ca->epoch_start = 0;
	ca->delayed_ack = 2 << ACK_RATIO_SHIFT;
}

static void bictcp_init(struct tcp_sock *tp)
{
	bictcp_reset(tcp_ca(tp));
	if (initial_ssthresh)
		tp->snd_ssthresh = initial_ssthresh;
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct bictcp *ca, u32 cwnd)
{
	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_time_stamp - ca->last_time) <= HZ / 32)
		return;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_time_stamp;

	if (ca->epoch_start == 0) /* record the beginning of an epoch */
		ca->epoch_start = tcp_time_stamp;

	/* start off normal */
	if (cwnd <= low_window) {
		ca->cnt = cwnd;
		return;
	}

	/* binary increase */
	if (cwnd < ca->last_max_cwnd) {
		__u32 	dist = (ca->last_max_cwnd - cwnd)
			/ BICTCP_B;

		if (dist > max_increment)
			/* linear increase */
			ca->cnt = cwnd / max_increment;
		else if (dist <= 1U)
			/* binary search increase */
			ca->cnt = (cwnd * smooth_part) / BICTCP_B;
		else
			/* binary search increase */
			ca->cnt = cwnd / dist;
	} else {
		/* slow start AMD linear increase */
		if (cwnd < ca->last_max_cwnd + BICTCP_B)
			/* slow start */
			ca->cnt = (cwnd * smooth_part) / BICTCP_B;
		else if (cwnd < ca->last_max_cwnd + max_increment*(BICTCP_B-1))
			/* slow start */
			ca->cnt = (cwnd * (BICTCP_B-1))
				/ (cwnd - ca->last_max_cwnd);
		else
			/* linear increase */
			ca->cnt = cwnd / max_increment;
	}

	/* if in slow start or link utilization is very low */
	if ( ca->loss_cwnd == 0 ||
	     (cwnd > ca->loss_cwnd && ca->low_utilization)) {
		if (ca->cnt > 20) /* increase cwnd 5% per RTT */
			ca->cnt = 20;
	}

	ca->cnt = (ca->cnt << ACK_RATIO_SHIFT) / ca->delayed_ack;
	if (ca->cnt == 0)			/* cannot be zero */
		ca->cnt = 1;
}


/* Detect low utilization in congestion avoidance */
static inline void bictcp_low_utilization(struct tcp_sock *tp, int flag)
{
	struct bictcp *ca = tcp_ca(tp);
	u32 dist, delay;

	/* No time stamp */
	if (!(tp->rx_opt.saw_tstamp && tp->rx_opt.rcv_tsecr) ||
	     /* Discard delay samples right after fast recovery */
	     tcp_time_stamp < ca->epoch_start + HZ ||
	     /* this delay samples may not be accurate */
	     flag == 0) {
		ca->last_delay = 0;
		goto notlow;
	}

	delay = ca->last_delay<<3;	/* use the same scale as tp->srtt*/
	ca->last_delay = tcp_time_stamp - tp->rx_opt.rcv_tsecr;
	if (delay == 0) 		/* no previous delay sample */
		goto notlow;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || ca->delay_min > delay) {
		ca->delay_min = ca->delay_max = delay;
		goto notlow;
	}

	if (ca->delay_max < delay)
		ca->delay_max = delay;

	/* utilization is low, if avg delay < dist*threshold
	   for checking_period time */
	dist = ca->delay_max - ca->delay_min;
	if (dist <= ca->delay_min>>6 ||
	    tp->srtt - ca->delay_min >=  (dist*low_utilization_threshold)>>10)
		goto notlow;

	if (ca->low_utilization_start == 0) {
		ca->low_utilization = 0;
		ca->low_utilization_start = tcp_time_stamp;
	} else if ((s32)(tcp_time_stamp - ca->low_utilization_start)
			> low_utilization_period*HZ) {
		ca->low_utilization = 1;
	}

	return;

 notlow:
	ca->low_utilization = 0;
	ca->low_utilization_start = 0;

}

static void bictcp_cong_avoid(struct tcp_sock *tp, u32 ack,
			      u32 seq_rtt, u32 in_flight, int data_acked)
{
	struct bictcp *ca = tcp_ca(tp);

	bictcp_low_utilization(tp, data_acked);

	if (in_flight < tp->snd_cwnd)
		return;

	if (tp->snd_cwnd <= tp->snd_ssthresh) {
		/* In "safe" area, increase. */
		if (tp->snd_cwnd < tp->snd_cwnd_clamp)
			tp->snd_cwnd++;
	} else {
		bictcp_update(ca, tp->snd_cwnd);

                /* In dangerous area, increase slowly.
		 * In theory this is tp->snd_cwnd += 1 / tp->snd_cwnd
		 */
		if (tp->snd_cwnd_cnt >= ca->cnt) {
			if (tp->snd_cwnd < tp->snd_cwnd_clamp)
				tp->snd_cwnd++;
			tp->snd_cwnd_cnt = 0;
		} else
			tp->snd_cwnd_cnt++;
	}

}

/*
 *	behave like Reno until low_window is reached,
 *	then increase congestion window slowly
 */
static u32 bictcp_recalc_ssthresh(struct tcp_sock *tp)
{
	struct bictcp *ca = tcp_ca(tp);

	ca->epoch_start = 0;	/* end of epoch */

	/* in case of wrong delay_max*/
	if (ca->delay_min > 0 && ca->delay_max > ca->delay_min)
		ca->delay_max = ca->delay_min
			+ ((ca->delay_max - ca->delay_min)* 90) / 100;

	/* Wmax and fast convergence */
	if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;

	ca->loss_cwnd = tp->snd_cwnd;


	if (tp->snd_cwnd <= low_window)
		return max(tp->snd_cwnd >> 1U, 2U);
	else
		return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static u32 bictcp_undo_cwnd(struct tcp_sock *tp)
{
	struct bictcp *ca = tcp_ca(tp);

	return max(tp->snd_cwnd, ca->last_max_cwnd);
}

static u32 bictcp_min_cwnd(struct tcp_sock *tp)
{
	return tp->snd_ssthresh;
}

static void bictcp_state(struct tcp_sock *tp, u8 new_state)
{
	if (new_state == TCP_CA_Loss)
		bictcp_reset(tcp_ca(tp));
}

/* Track delayed acknowledgement ratio using sliding window
 * ratio = (15*ratio + sample) / 16
 */
static void bictcp_acked(struct tcp_sock *tp, u32 cnt)
{
	if (cnt > 0 && 	tp->ca_state == TCP_CA_Open) {
		struct bictcp *ca = tcp_ca(tp);
		cnt -= ca->delayed_ack >> ACK_RATIO_SHIFT;
		ca->delayed_ack += cnt;
	}
}


static struct tcp_congestion_ops bictcp = {
	.init		= bictcp_init,
	.ssthresh	= bictcp_recalc_ssthresh,
	.cong_avoid	= bictcp_cong_avoid,
	.set_state	= bictcp_state,
	.undo_cwnd	= bictcp_undo_cwnd,
	.min_cwnd	= bictcp_min_cwnd,
	.pkts_acked     = bictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "bic",
};

static int __init bictcp_register(void)
{
	BUG_ON(sizeof(struct bictcp) > TCP_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&bictcp);
}

static void __exit bictcp_unregister(void)
{
	tcp_unregister_congestion_control(&bictcp);
}

module_init(bictcp_register);
module_exit(bictcp_unregister);

MODULE_AUTHOR("Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BIC TCP");
