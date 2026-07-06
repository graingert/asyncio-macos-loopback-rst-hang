/*
 * Catch the RFC 5961 RST that leaves an ESTABLISHED connection half-open on
 * FreeBSD, and print the receiver's TCP control block at that instant -- the
 * ground truth the packet capture can only infer.
 *
 * tcp_do_segment(struct tcpcb *tp, struct mbuf *m, struct tcphdr *th, ...):
 *   arg0 = tp (receiver's tcpcb), arg2 = th (already host-order here, because
 *   tcp_input() calls tcp_fields_to_host() before tcp_do_segment()).
 *
 * With the default net.inet.tcp.insecure_rst=0, the RFC 5961 check resets the
 * connection iff last_ack_sent == th_seq (that equality always satisfies an
 * accept clause). So a RST in ESTABLISHED with last_ack_sent != th_seq does NOT
 * reset -- that is precisely the hang. The printed deltas answer the open
 * question:
 *
 *   seq - rcv_nxt   ==  0  => RST sits exactly at rcv_nxt: the final data was
 *                            already absorbed and only the ACK is delayed
 *                            (last_ack_sent lags). The proposed fix helps.
 *                   >   0  => RST is ahead of rcv_nxt: the final data segment
 *                            was not yet processed when the RST arrived.
 *
 *   (last_ack_sent + rcv_wnd) - seq  < 0  => the current check's right edge is
 *                                            below the RST seq -> rejected (bug)
 *   (rcv_nxt       + rcv_wnd) - seq  > 0  => the proposed right edge would
 *                                            accept the RST
 */
#pragma D option quiet

fbt::tcp_do_segment:entry
/(args[2]->th_flags & 0x04) != 0 &&		/* TH_RST */
 args[0]->t_state == 4 &&			/* TCPS_ESTABLISHED */
 args[0]->last_ack_sent != args[2]->th_seq/	/* != => does NOT reset => hang */
{
	this->seq  = (uint32_t)args[2]->th_seq;
	this->las  = (uint32_t)args[0]->last_ack_sent;
	this->rnxt = (uint32_t)args[0]->rcv_nxt;
	this->rwnd = (uint32_t)args[0]->rcv_wnd;

	printf("HANG-RST th_seq=%u rcv_nxt=%u last_ack_sent=%u rcv_wnd=%u | seq-rcv_nxt=%d seq-last_ack_sent=%d cur_edge(las+wnd)-seq=%d fix_edge(rcvnxt+wnd)-seq=%d\n",
	    this->seq, this->rnxt, this->las, this->rwnd,
	    (int)(this->seq - this->rnxt),
	    (int)(this->seq - this->las),
	    (int)((this->las + this->rwnd) - this->seq),
	    (int)((this->rnxt + this->rwnd) - this->seq));
}
