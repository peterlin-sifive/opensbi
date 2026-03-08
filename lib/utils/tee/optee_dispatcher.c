/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SiFive
 *
 * OP-TEE Dispatcher Implementation
 */

#include <libfdt.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_mpxy.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/tee/tee_dispatcher.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi_mbox.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>

/**
 * OP-TEE specific context
 */
struct optee_context {
	/** Domain name from device tree */
	char domain_name[64];
	/** Pointer to OP-TEE domain */
	struct sbi_domain *domain;
	/** Request forward channel ID */
	u32 reqfwd_channel_id;
	/** Owner hart ID */
	u32 hartid;
};

/**
 * Setup OP-TEE domain from device tree
 */
static int optee_domain_setup(const void *fdt, int nodeoff,
			      struct optee_context *ctx)
{
	struct sbi_domain *dom = NULL;
	const u32 *prop_instance;
	int len, offset;

	prop_instance = fdt_getprop(fdt, nodeoff, "opensbi-domain-instance",
				    &len);
	if (!prop_instance || len < 4)
		return SBI_EINVAL;

	offset = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*prop_instance));
	if (offset < 0)
		return SBI_EINVAL;

	sbi_strncpy(ctx->domain_name, fdt_get_name(fdt, offset, NULL),
		    sizeof(ctx->domain_name));
	ctx->domain_name[sizeof(ctx->domain_name) - 1] = '\0';

	sbi_domain_for_each(dom) {
		if (!sbi_strcmp(dom->name, ctx->domain_name)) {
			ctx->domain = dom;
			break;
		}
	}

	return SBI_OK;
}

/**
 * Deferred domain setup (called when domain is used)
 */
static int optee_domain_setup_deferred(struct optee_context *ctx)
{
	struct sbi_domain *dom = NULL;

	if (ctx->domain)
		return SBI_OK;

	sbi_domain_for_each(dom) {
		if (!sbi_strcmp(dom->name, ctx->domain_name)) {
			ctx->domain = dom;
			return SBI_OK;
		}
	}

	return SBI_ENOENT;
}

/**
 * Get OP-TEE attributes
 *
 * OP-TEE uses SMC-style communication with 8 input registers (a0-a7)
 * and 4 output registers (a0-a3).
 */
static int optee_get_attributes(const struct tee_dispatcher *dispatcher,
				struct tee_attributes *attr)
{
	if (!attr)
		return SBI_EINVAL;

	/* TEE implementation ID */
	attr->tee_impl_id = RPMI_TEE_IMPL_ID_OPTEE;

	/* OP-TEE uses 8 registers for request (a0-a7) */
	attr->comm_req_regs = RPMI_TEE_OPTEE_COMM_REQ_REGS;

	/* OP-TEE uses 4 registers for response (a0-a3) */
	attr->comm_resp_regs = RPMI_TEE_OPTEE_COMM_RESP_REGS;

	return SBI_OK;
}

/**
 * OP-TEE communicate - forward message and enter domain
 *
 * OP-TEE uses SMC-style parameters (a0-a7) for communication.
 * The request data contains 8 unsigned long values representing
 * the SMC parameters, and the response contains 4 return values.
 */
static int optee_communicate(const struct tee_dispatcher *dispatcher,
			     void *tx_data, u32 tx_len,
			     void *rx_data, u32 rx_max_len,
			     unsigned long *rx_len)
{
	struct optee_context *ctx = dispatcher->context;
	struct sbi_mpxy_channel *recv_channel;
	struct rpmi_message_header header;
	int rc;

	if (!ctx)
		return SBI_EINVAL;

	/* Find request forward channel */
	recv_channel = sbi_mpxy_find_channel(ctx->reqfwd_channel_id);
	if (!recv_channel)
		return SBI_ENODEV;

	/* Prepare the header for forwarding to OP-TEE domain */
	header.servicegroup_id = cpu_to_le16(RPMI_SRVGRP_TEE);
	header.service_id = RPMI_TEE_SRV_COMMUNICATE;
	header.flags = RPMI_MSG_NORMAL_REQUEST;
	header.datalen = cpu_to_le16(tx_len);
	header.token = cpu_to_le16(0);

	/* Forward message to request forward channel */
	rc = mpxy_reqfwd_forward_message(recv_channel, &header,
					 tx_data, tx_len,
					 rx_data, rx_max_len,
					 rx_len);

	return rc;
}

/**
 * Enter OP-TEE domain
 */
static int optee_domain_enter(const struct tee_dispatcher *dispatcher)
{
	struct optee_context *ctx = dispatcher->context;
	int rc;

	if (!ctx)
		return SBI_EINVAL;

	/* Try deferred domain setup if not done yet */
	rc = optee_domain_setup_deferred(ctx);
	if (rc)
		return rc;

	return sbi_domain_context_enter(ctx->domain);
}

/** OP-TEE dispatcher operations */
static const struct tee_dispatcher_ops optee_ops = {
	.get_attributes = optee_get_attributes,
	.communicate = optee_communicate,
	.domain_enter = optee_domain_enter,
};

/**
 * Setup OP-TEE dispatcher from device tree
 */
int optee_dispatcher_setup(const void *fdt, int nodeoff,
			   struct tee_dispatcher *dispatcher)
{
	struct optee_context *ctx;
	const fdt32_t *val;
	int rc, len, cpu_offset, sibling_offset;
	bool found_reqfwd = false;

	/* Allocate OP-TEE context */
	ctx = sbi_zalloc(sizeof(*ctx));
	if (!ctx)
		return SBI_ENOMEM;

	/* Get parent CPU node to extract hartid */
	cpu_offset = fdt_parent_offset(fdt, nodeoff);
	if (cpu_offset < 0) {
		rc = SBI_EINVAL;
		goto fail_free_ctx;
	}

	rc = fdt_parse_hart_id(fdt, cpu_offset, &ctx->hartid);
	if (rc)
		goto fail_free_ctx;

	/* Find sibling reqfwd node to get reqfwd channel id */
	fdt_for_each_subnode(sibling_offset, fdt, cpu_offset) {
		if (!fdt_node_check_compatible(fdt, sibling_offset,
					       "riscv,sbi-mpxy-reqfwd")) {
			val = fdt_getprop(fdt, sibling_offset,
					  "riscv,sbi-mpxy-channel-id", &len);
			if (len > 0 && val) {
				ctx->reqfwd_channel_id = fdt32_to_cpu(*val);
				found_reqfwd = true;
				break;
			}
		}
	}

	if (!found_reqfwd) {
		rc = SBI_ENOENT;
		goto fail_free_ctx;
	}

	/* Setup OP-TEE domain */
	rc = optee_domain_setup(fdt, nodeoff, ctx);
	if (rc)
		goto fail_free_ctx;

	/* Setup dispatcher */
	dispatcher->impl_id = RPMI_TEE_IMPL_ID_OPTEE;
	dispatcher->name = "OP-TEE";
	dispatcher->ops = &optee_ops;
	dispatcher->context = ctx;

	return SBI_OK;

fail_free_ctx:
	sbi_free(ctx);
	return rc;
}

