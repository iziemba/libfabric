/*
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <ofi_mem.h>
#include <ofi_iov.h>
#include "rxd.h"

static int rxd_match_unexp(struct dlist_entry *item, const void *arg)
{
	struct rxd_match_attr *attr = (struct rxd_match_attr *) arg;
	struct rxd_unexp_msg *unexp_msg;

	unexp_msg = container_of(item, struct rxd_unexp_msg, entry);

	if (!rxd_match_addr(attr->peer, unexp_msg->base_hdr->peer))
		return 0;

	if (!unexp_msg->tag_hdr)
		return 1;

	return rxd_match_tag(attr->tag, attr->ignore,
			     unexp_msg->tag_hdr->tag);
}

static struct rxd_unexp_msg *rxd_ep_check_unexp_list(struct dlist_entry *list,
				fi_addr_t addr, uint64_t tag, uint64_t ignore)
{
	struct rxd_match_attr attr;
	struct dlist_entry *match;

	attr.peer = addr;
	attr.tag = tag;
	attr.ignore = ignore;

	match = dlist_find_first_match(list, &rxd_match_unexp, &attr);
	if (!match)
		return NULL;

	FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Matched to unexp msg entry\n");

	return container_of(match, struct rxd_unexp_msg, entry);
}

static void rxd_progress_unexp_msg(struct rxd_ep *ep, struct rxd_x_entry *rx_entry,
				   struct rxd_unexp_msg *unexp_msg)
{
	struct rxd_pkt_entry *pkt_entry;

	rxd_progress_op(ep, rx_entry, unexp_msg->pkt_entry, unexp_msg->base_hdr,
			unexp_msg->sar_hdr, unexp_msg->tag_hdr,
			unexp_msg->data_hdr, NULL, NULL, &unexp_msg->msg,
			unexp_msg->msg_size);

	while (!dlist_empty(&unexp_msg->pkt_list)) {
		dlist_pop_front(&unexp_msg->pkt_list, struct rxd_pkt_entry,
				pkt_entry, d_entry);
		rxd_ep_recv_data(ep, rx_entry, (struct rxd_data_pkt *)
				 (pkt_entry->pkt), pkt_entry->pkt_size);
		rxd_release_repost_rx(ep, pkt_entry);
	}
	rxd_release_repost_rx(ep, unexp_msg->pkt_entry);
	dlist_remove(&unexp_msg->entry);
	free(unexp_msg);
}

static int rxd_progress_unexp_list(struct rxd_ep *ep,
				   struct dlist_entry *unexp_list,
				   struct dlist_entry *rx_list,
				   struct rxd_x_entry *rx_entry)
{
	struct rxd_x_entry *progress_entry, *dup_entry = NULL;
	struct rxd_unexp_msg *unexp_msg;
	size_t total_size;

	while (!dlist_empty(unexp_list)) {
		unexp_msg = rxd_ep_check_unexp_list(unexp_list, rx_entry->peer,
					rx_entry->cq_entry.tag, rx_entry->ignore);
		if (!unexp_msg)
			return 0;

		total_size = unexp_msg->sar_hdr ? unexp_msg->sar_hdr->size :
			     unexp_msg->msg_size;

		if (rx_entry->flags & RXD_MULTI_RECV)
			dup_entry = rxd_progress_multi_recv(ep, rx_entry, total_size);

		progress_entry = dup_entry ? dup_entry : rx_entry;
		progress_entry->cq_entry.len = MIN(rx_entry->cq_entry.len, total_size);
		rxd_progress_unexp_msg(ep, progress_entry, unexp_msg);
		if (!dup_entry)
			return 1;
	}

	return 0;
}

static int rxd_ep_discard_recv(struct rxd_ep *rxd_ep, void *context,
			       struct rxd_unexp_msg *unexp_msg)
{
	struct rxd_pkt_entry *pkt_entry;
	uint64_t seq = unexp_msg->base_hdr->seq_no;
	int ret;

	assert(unexp_msg->tag_hdr);
	seq += unexp_msg->sar_hdr ? unexp_msg->sar_hdr->num_segs : 1;

	rxd_ep->peers[unexp_msg->base_hdr->peer].rx_seq_no =
			MAX(seq, rxd_ep->peers[unexp_msg->base_hdr->peer].rx_seq_no);
	rxd_ep_send_ack(rxd_ep, unexp_msg->base_hdr->peer);

	ret = ofi_cq_write(rxd_ep->util_ep.rx_cq, context, FI_TAGGED | FI_RECV,
			   0, NULL, unexp_msg->data_hdr ?
			   unexp_msg->data_hdr->cq_data : 0,
			   unexp_msg->tag_hdr->tag);

	while (!dlist_empty(&unexp_msg->pkt_list)) {
		dlist_pop_front(&unexp_msg->pkt_list, struct rxd_pkt_entry,
				pkt_entry, d_entry);
		rxd_release_repost_rx(rxd_ep, pkt_entry);
	}

	rxd_release_repost_rx(rxd_ep, unexp_msg->pkt_entry);

	dlist_remove(&unexp_msg->entry);
	free(unexp_msg);

	return ret;
}

static int rxd_peek_recv(struct rxd_ep *rxd_ep, fi_addr_t addr, uint64_t tag,
			 uint64_t ignore, void *context, uint64_t flags,
			 struct dlist_entry *unexp_list)
{
	struct rxd_unexp_msg *unexp_msg;

	fastlock_release(&rxd_ep->util_ep.lock);
	rxd_ep_progress(&rxd_ep->util_ep);
	fastlock_acquire(&rxd_ep->util_ep.lock);

	unexp_msg = rxd_ep_check_unexp_list(unexp_list, addr, tag, ignore);
	if (!unexp_msg) {
		FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Message not found\n");
		return ofi_cq_write_error_peek(rxd_ep->util_ep.rx_cq, tag,
					       context);
	}
	FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Message found\n");

	assert(unexp_msg->tag_hdr);
	if (flags & FI_DISCARD)
		return rxd_ep_discard_recv(rxd_ep, context, unexp_msg);

	if (flags & FI_CLAIM) {
		FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Marking message for CLAIM\n");
		((struct fi_context *)context)->internal[0] = unexp_msg;
		dlist_remove(&unexp_msg->entry);
	}

	return ofi_cq_write(rxd_ep->util_ep.rx_cq, context, FI_TAGGED | FI_RECV,
			    unexp_msg->sar_hdr ? unexp_msg->sar_hdr->size :
			    unexp_msg->msg_size, NULL, unexp_msg->data_hdr ?
			    unexp_msg->data_hdr->cq_data : 0,
			    unexp_msg->tag_hdr->tag);
}

ssize_t rxd_ep_generic_recvmsg(struct rxd_ep *rxd_ep, const struct iovec *iov,
			       size_t iov_count, fi_addr_t addr, uint64_t tag,
			       uint64_t ignore, void *context, uint32_t op,
			       uint32_t rxd_flags, uint64_t flags)
{
	ssize_t ret = 0;
	struct rxd_x_entry *rx_entry;
	struct dlist_entry *unexp_list, *rx_list;
	struct rxd_unexp_msg *unexp_msg;

	assert(iov_count <= RXD_IOV_LIMIT);
	assert(!(rxd_flags & RXD_MULTI_RECV) || iov_count == 1);
	assert(!(flags & FI_PEEK) || op == ofi_op_tagged);

	fastlock_acquire(&rxd_ep->util_ep.lock);

	if (ofi_cirque_isfull(rxd_ep->util_ep.rx_cq->cirq)) {
		ret = -FI_EAGAIN;
		goto out;
	}

	if (op == ofi_op_tagged) {
		unexp_list = &rxd_ep->unexp_tag_list;
		rx_list = &rxd_ep->rx_tag_list;
	} else {
		unexp_list = &rxd_ep->unexp_list;
		rx_list = &rxd_ep->rx_list;
	}

	if (flags & FI_PEEK) {
		ret = rxd_peek_recv(rxd_ep, addr, tag, ignore, context, flags,
				    unexp_list);
		goto out;
	}

	if (!(flags & FI_DISCARD)) {
		rx_entry = rxd_rx_entry_init(rxd_ep, iov, iov_count, tag, ignore, context,
					(rxd_ep->util_ep.caps & FI_DIRECTED_RECV &&
					addr != FI_ADDR_UNSPEC) ?
					rxd_ep_av(rxd_ep)->fi_addr_table[addr] :
					FI_ADDR_UNSPEC, op, rxd_flags);
		if (!rx_entry) {
			ret = -FI_EAGAIN;
		} else if (flags & FI_CLAIM) {
			FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Claiming message\n");
			unexp_msg = (struct rxd_unexp_msg *)
				(((struct fi_context *) context)->internal[0]);
			rxd_progress_unexp_msg(rxd_ep, rx_entry, unexp_msg);
		} else if (!rxd_progress_unexp_list(rxd_ep, unexp_list,
			   rx_list, rx_entry)) {
			dlist_insert_tail(&rx_entry->entry, rx_list);
		}
		goto out;
	}

	assert(flags & FI_CLAIM);
	FI_DBG(&rxd_prov, FI_LOG_EP_CTRL, "Discarding message\n");
	unexp_msg = (struct rxd_unexp_msg *)
			(((struct fi_context *) context)->internal[0]);
	ret = rxd_ep_discard_recv(rxd_ep, context, unexp_msg);

out:
	fastlock_release(&rxd_ep->util_ep.lock);
	return ret;
}

static ssize_t rxd_ep_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			      uint64_t flags)
{
	struct rxd_ep *ep;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	return rxd_ep_generic_recvmsg(ep, msg->msg_iov, msg->iov_count,
				      msg->addr, 0, ~0, msg->context, ofi_op_msg,
				      rxd_rx_flags(flags | ep->util_ep.rx_msg_flags),
				      flags);
}

static ssize_t rxd_ep_recv(struct fid_ep *ep_fid, void *buf, size_t len, void *desc,
			   fi_addr_t src_addr, void *context)
{
	struct rxd_ep *ep;
	struct iovec msg_iov;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	return rxd_ep_generic_recvmsg(ep, &msg_iov, 1, src_addr, 0, ~0, context,
				      ofi_op_msg, ep->rx_flags, 0);
}

static ssize_t rxd_ep_recvv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
			    size_t count, fi_addr_t src_addr, void *context)
{
	struct rxd_ep *ep;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	return rxd_ep_generic_recvmsg(ep, iov, count, src_addr,
				      0, ~0, context, ofi_op_msg, ep->rx_flags, 0);
}

ssize_t rxd_ep_generic_inject(struct rxd_ep *rxd_ep, const struct iovec *iov,
			      size_t iov_count, fi_addr_t addr, uint64_t tag,
			      uint64_t data, uint32_t op, uint32_t rxd_flags)
{
	struct rxd_x_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;
	fi_addr_t rxd_addr;

	assert(iov_count <= RXD_IOV_LIMIT);
	assert(ofi_total_iov_len(iov, iov_count) <=
	       rxd_ep_domain(rxd_ep)->max_inline_msg);

	fastlock_acquire(&rxd_ep->util_ep.lock);

	if (ofi_cirque_isfull(rxd_ep->util_ep.tx_cq->cirq))
		goto out;

	rxd_addr = rxd_ep_av(rxd_ep)->fi_addr_table[addr];
	ret = rxd_send_rts_if_needed(rxd_ep, rxd_addr);
	if (ret)
		goto out;

	tx_entry = rxd_tx_entry_init(rxd_ep, iov, iov_count, NULL, 0, 0, data,
				     tag, NULL, rxd_addr, op, rxd_flags);
	if (!tx_entry) {
		ret = -FI_EAGAIN;
		goto out;
	}

	ret = rxd_ep_send_op(rxd_ep, tx_entry, NULL, 0, NULL, 0, 0, 0);
	if (ret)
		rxd_tx_entry_free(rxd_ep, tx_entry);

out:
	fastlock_release(&rxd_ep->util_ep.lock);
	return ret;
}

ssize_t rxd_ep_generic_sendmsg(struct rxd_ep *rxd_ep, const struct iovec *iov,
			       size_t iov_count, fi_addr_t addr, uint64_t tag,
			       uint64_t data, void *context, uint32_t op,
			       uint32_t rxd_flags)
{
	struct rxd_x_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;
	fi_addr_t rxd_addr;

	assert(iov_count <= RXD_IOV_LIMIT);

	if (rxd_flags & RXD_INJECT)
		return rxd_ep_generic_inject(rxd_ep, iov, iov_count, addr, tag, 0,
					     op, rxd_flags);

	fastlock_acquire(&rxd_ep->util_ep.lock);

	if (ofi_cirque_isfull(rxd_ep->util_ep.tx_cq->cirq))
		goto out;

	rxd_addr = rxd_ep_av(rxd_ep)->fi_addr_table[addr];
	ret = rxd_send_rts_if_needed(rxd_ep, rxd_addr);
	if (ret)
		goto out;

	tx_entry = rxd_tx_entry_init(rxd_ep, iov, iov_count, NULL, 0, 0,
				     data, tag, context, rxd_addr, op, rxd_flags);
	if (!tx_entry)
		goto out;

	ret = rxd_ep_send_op(rxd_ep, tx_entry, NULL, 0, NULL, 0, 0, 0);
	if (ret)
		rxd_tx_entry_free(rxd_ep, tx_entry);

out:
	fastlock_release(&rxd_ep->util_ep.lock);
	return ret;
}

static ssize_t rxd_ep_sendmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			      uint64_t flags)
{
	struct rxd_ep *ep;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	return rxd_ep_generic_sendmsg(ep, msg->msg_iov, msg->iov_count,
				   msg->addr, 0, msg->data, msg->context,
				   ofi_op_msg, rxd_tx_flags(flags |
				   ep->util_ep.tx_msg_flags));

}

static ssize_t rxd_ep_sendv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
			    size_t count, fi_addr_t dest_addr, void *context)
{
	struct rxd_ep *ep;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	return rxd_ep_generic_sendmsg(ep, iov, count, dest_addr, 0,
				      0, context, ofi_op_msg,
				      ep->tx_flags);
}

static ssize_t rxd_ep_send(struct fid_ep *ep_fid, const void *buf, size_t len,
			   void *desc, fi_addr_t dest_addr, void *context)
{
	struct rxd_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxd_ep_generic_sendmsg(ep, &iov, 1, dest_addr, 0,
				      0, context, ofi_op_msg,
				      ep->tx_flags);
}

static ssize_t rxd_ep_inject(struct fid_ep *ep_fid, const void *buf, size_t len,
			     fi_addr_t dest_addr)
{
	struct rxd_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxd_ep_generic_inject(ep, &iov, 1, dest_addr, 0, 0, ofi_op_msg,
				     RXD_NO_TX_COMP | RXD_INJECT);
}

static ssize_t rxd_ep_senddata(struct fid_ep *ep_fid, const void *buf, size_t len,
			       void *desc, uint64_t data, fi_addr_t dest_addr,
			       void *context)
{
	struct rxd_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxd_ep_generic_sendmsg(ep, &iov, 1, dest_addr, 0, data, context,
				      ofi_op_msg, ep->tx_flags |
				      RXD_REMOTE_CQ_DATA);
}

static ssize_t rxd_ep_injectdata(struct fid_ep *ep_fid, const void *buf, size_t len,
				 uint64_t data, fi_addr_t dest_addr)
{
	struct rxd_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct rxd_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxd_ep_generic_inject(ep, &iov, 1, dest_addr, 0, data, ofi_op_msg,
				     RXD_NO_TX_COMP | RXD_INJECT |
				     RXD_REMOTE_CQ_DATA);
}

struct fi_ops_msg rxd_ops_msg = {
	.size = sizeof(struct fi_ops_msg),
	.recv = rxd_ep_recv,
	.recvv = rxd_ep_recvv,
	.recvmsg = rxd_ep_recvmsg,
	.send = rxd_ep_send,
	.sendv = rxd_ep_sendv,
	.sendmsg = rxd_ep_sendmsg,
	.inject = rxd_ep_inject,
	.senddata = rxd_ep_senddata,
	.injectdata = rxd_ep_injectdata,
};
