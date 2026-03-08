/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Andes Technology Corporation.
 * Copyright (c) 2026 SiFive
 *
 * Generic TEE Service Group for MPXY RPMI
 *
 * This implementation follows the RPMI TEE Service Group specification
 * and delegates TEE-specific operations to registered dispatchers.
 */

#include <libfdt.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_mpxy.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi_mbox.h>
#include <sbi_utils/mailbox/rpmi_mailbox.h>
#include <sbi_utils/tee/tee_dispatcher.h>

/** TEE MPXY channel context */
struct mpxy_tee {
	/** TEE dispatcher */
	struct tee_dispatcher *dispatcher;
	/** TEE attributes */
	struct tee_attributes attrs;
	/** RPMI channel attributes */
	struct mpxy_rpmi_channel_attrs msgprot_attrs;
	/** MPXY channel */
	struct sbi_mpxy_channel channel;
	/** Owner hart ID */
	u32 hartid;
};

/**
 * Read RPMI message protocol attributes
 */
static int mpxy_tee_read_attributes(struct sbi_mpxy_channel *channel,
				    u32 *outmem, u32 base_attr_id,
				    u32 attr_count)
{
	struct mpxy_tee *tee =
		container_of(channel, struct mpxy_tee, channel);
	u32 end_id = base_attr_id + attr_count - 1;

	if (end_id >= MPXY_MSGPROT_RPMI_ATTR_MAX_ID ||
	    base_attr_id < MPXY_MSGPROT_RPMI_ATTR_SERVICEGROUP_ID)
		return SBI_EBAD_RANGE;

	sbi_memcpy(outmem,
		   (void *)&tee->msgprot_attrs + attr_id2index(base_attr_id) *
		   sizeof(u32), attr_count * sizeof(u32));
	return SBI_OK;
}

/**
 * Send TEE message with response
 */
static int mpxy_tee_send_message_with_response(struct sbi_mpxy_channel *channel,
					       u32 msg_id, void *msgbuf,
					       u32 msg_len, void *respbuf,
					       u32 resp_max_len,
					       unsigned long *resp_len)
{
	struct mpxy_tee *tee =
		container_of(channel, struct mpxy_tee, channel);
	struct rpmi_tee_get_attributes_resp *attr_resp;
	int rc = SBI_OK;

	if (!tee->dispatcher)
		return SBI_ENODEV;

	switch (msg_id) {
	case RPMI_TEE_SRV_GET_ATTRIBUTES:
		if (resp_max_len < sizeof(*attr_resp))
			return SBI_ENOMEM;
		attr_resp = respbuf;
		attr_resp->status = cpu_to_le32(RPMI_SUCCESS);
		attr_resp->tee_impl_id = cpu_to_le32(tee->attrs.tee_impl_id);
		attr_resp->comm_req_regs = cpu_to_le32(tee->attrs.comm_req_regs);
		attr_resp->comm_resp_regs = cpu_to_le32(tee->attrs.comm_resp_regs);
		*resp_len = sizeof(*attr_resp);
		break;

	case RPMI_TEE_SRV_COMMUNICATE:
		/*
		 * TEE_COMMUNICATE uses implementation-specific data format.
		 * Forward the entire request to the dispatcher.
		 */
		if (tee->dispatcher->ops->communicate) {
			rc = tee->dispatcher->ops->communicate(
				tee->dispatcher,
				msgbuf, msg_len,
				respbuf, resp_max_len,
				resp_len);

			/* Enter TEE domain if supported */
			if (!rc && tee->dispatcher->ops->domain_enter) {
				rc = tee->dispatcher->ops->domain_enter(
					tee->dispatcher);
			}
		} else {
			((u32 *)respbuf)[0] = cpu_to_le32(RPMI_ERR_NOTSUPP);
			*resp_len = sizeof(u32);
		}
		break;

	default:
		((u32 *)respbuf)[0] = cpu_to_le32(RPMI_ERR_NOTSUPP);
		*resp_len = sizeof(u32);
		break;
	}

	return rc;
}

/**
 * Initialize TEE MPXY channel
 */
static int mpxy_tee_init(const void *fdt, int nodeoff,
			 const struct fdt_match *match)
{
	struct mpxy_tee *tee;
	const fdt32_t *val;
	u32 channel_id, hartid;
	int rc, len, cpu_offset;

	/* Allocate context for TEE MPXY */
	tee = sbi_zalloc(sizeof(*tee));
	if (!tee)
		return SBI_ENOMEM;

	/* Get channel ID from DT */
	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-channel-id", &len);
	if (len > 0 && val)
		channel_id = fdt32_to_cpu(*val);
	else {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	/* Get parent CPU node to extract hartid */
	cpu_offset = fdt_parent_offset(fdt, nodeoff);
	if (cpu_offset < 0) {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	rc = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
	if (rc)
		goto fail_free;

	tee->hartid = hartid;

	/* Setup TEE dispatcher from device tree */
	rc = tee_dispatcher_setup_from_fdt(fdt, nodeoff, &tee->dispatcher);
	if (rc)
		goto fail_free;

	/* Get TEE attributes */
	if (tee->dispatcher->ops->get_attributes) {
		rc = tee->dispatcher->ops->get_attributes(
			tee->dispatcher, &tee->attrs);
		if (rc)
			goto fail_free;
	}

	/* Setup MPXY channel */
	tee->channel.channel_id = channel_id;
	tee->channel.attrs.msg_proto_id = SBI_MPXY_MSGPROTO_RPMI_ID;
	tee->channel.attrs.msg_proto_version = 1;
	tee->channel.attrs.msg_data_maxlen = PAGE_SIZE;
	tee->channel.attrs.msg_send_timeout = 0;
	tee->channel.attrs.msg_completion_timeout = 0;
	tee->channel.read_attributes = mpxy_tee_read_attributes;
	tee->channel.send_message_with_response =
		mpxy_tee_send_message_with_response;

	/* Setup RPMI service group attributes */
	tee->msgprot_attrs.servicegrp_id = RPMI_SRVGRP_TEE;
	tee->msgprot_attrs.servicegrp_ver = 1;

	/* Register MPXY channel */
	rc = sbi_mpxy_register_channel(&tee->channel);
	if (rc)
		goto fail_free;

	return SBI_OK;

fail_free:
	sbi_free(tee);
	return rc;
}

/** Device tree match table */
static const struct fdt_match tee_match[] = {
	{
		.compatible = "riscv,rpmi-mpxy-tee",
		.data = NULL,
	},
	{},
};

/** TEE MPXY driver */
const struct fdt_driver fdt_mpxy_rpmi_tee = {
	.experimental = true,
	.match_table = tee_match,
	.init = mpxy_tee_init,
};
