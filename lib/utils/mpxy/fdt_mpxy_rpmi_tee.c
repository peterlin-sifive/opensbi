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
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi_mbox.h>
#include <sbi_utils/mailbox/rpmi_mailbox.h>
#include <sbi_utils/tee/tee_dispatcher.h>

/** TEE group context */
struct tee_group_context {
	struct tee_dispatcher *dispatcher;
	struct tee_attributes cached_attrs;
};

/** Cached response for TEE_GET_ATTRIBUTES */
static struct tee_group_context tee_ctx;

/** TEE service data for validation */
static struct mpxy_rpmi_service_data tee_srvcdata[] = {
	[0] = {
		.id = RPMI_TEE_SRV_GET_ATTRIBUTES,
		.min_tx_len = 0,
		.max_tx_len = 0,
		.min_rx_len = sizeof(s32) + sizeof(u32), /* status + impl_id */
		.max_rx_len = sizeof(s32) + sizeof(u32),
	},
	[1] = {
		.id = RPMI_TEE_SRV_COMMUNICATE,
		/*
		 * TEE_COMMUNICATE data format is implementation-specific.
		 * The min/max lengths here are just basic sanity checks.
		 * Actual validation is done by the dispatcher.
		 */
		.min_tx_len = sizeof(u32),
		.max_tx_len = 256,  /* Max MPXY message size */
		.min_rx_len = sizeof(s32),
		.max_rx_len = 256,
	},
};

/**
 * Setup TEE service group
 */
static int mpxy_rpmi_tee_setup(void **context, struct mbox_chan *chan,
			       const struct mpxy_rpmi_mbox_data *data)
{
	int rc;

	if (!tee_ctx.dispatcher)
		return SBI_ENODEV;

	/* Get and cache TEE attributes */
	if (tee_ctx.dispatcher->ops->get_attributes) {
		rc = tee_ctx.dispatcher->ops->get_attributes(
			tee_ctx.dispatcher, &tee_ctx.cached_attrs);
		if (rc)
			return rc;
	}

	*context = &tee_ctx;
	return SBI_OK;
}

/**
 * Handle TEE service group message transfer
 */
static int mpxy_rpmi_tee_xfer(void *context, struct mbox_chan *chan,
			      struct mbox_xfer *xfer)
{
	struct tee_group_context *ctx = context;
	struct rpmi_message_args *args = xfer->args;
	int rc = SBI_OK;

	if (!ctx || !ctx->dispatcher)
		return SBI_ENODEV;

	if (!xfer->rx || (args->type != RPMI_MSG_NORMAL_REQUEST))
		return SBI_OK;

	switch (args->service_id) {
	case RPMI_TEE_SRV_GET_ATTRIBUTES:
		/* Return cached attributes */
		((u32 *)xfer->rx)[0] = cpu_to_le32(RPMI_SUCCESS);
		((u32 *)xfer->rx)[1] = cpu_to_le32(ctx->cached_attrs.tee_impl_id);
		args->rx_data_len = 2 * sizeof(u32);
		break;

	case RPMI_TEE_SRV_COMMUNICATE:
		/*
		 * TEE_COMMUNICATE uses implementation-specific data format.
		 * Forward the entire request to the dispatcher.
		 */
		if (ctx->dispatcher->ops->communicate) {
			unsigned long rx_len = 0;

			rc = ctx->dispatcher->ops->communicate(
				ctx->dispatcher,
				xfer->tx, xfer->tx_len,
				xfer->rx, xfer->rx_len,
				&rx_len);

			args->rx_data_len = rx_len;

			/* Enter TEE domain if supported */
			if (!rc && ctx->dispatcher->ops->domain_enter) {
				rc = ctx->dispatcher->ops->domain_enter(
					ctx->dispatcher);
			}
		} else {
			((u32 *)xfer->rx)[0] = cpu_to_le32(RPMI_ERR_NOTSUPP);
			args->rx_data_len = sizeof(u32);
		}
		break;

	default:
		((u32 *)xfer->rx)[0] = cpu_to_le32(RPMI_ERR_NOTSUPP);
		args->rx_data_len = sizeof(u32);
		break;
	}

	return rc;
}

/** TEE service group mbox data */
static const struct mpxy_rpmi_mbox_data tee_data = {
	.servicegrp_id = RPMI_SRVGRP_TEE,
	.num_services = RPMI_TEE_SRV_MAX_COUNT,
	.service_data = tee_srvcdata,
	.setup_group = mpxy_rpmi_tee_setup,
	.xfer_group = mpxy_rpmi_tee_xfer,
};

/**
 * Initialize TEE MPXY channel
 */
static int mpxy_tee_init(const void *fdt, int nodeoff,
			 const struct fdt_match *match)
{
	int rc;

	/* Setup TEE dispatcher from device tree */
	rc = tee_dispatcher_setup_from_fdt(fdt, nodeoff, &tee_ctx.dispatcher);
	if (rc)
		return rc;

	/* Continue with standard RPMI mbox init */
	return mpxy_rpmi_mbox_init(fdt, nodeoff, match);
}

/** Device tree match table */
static const struct fdt_match tee_match[] = {
	{
		.compatible = "riscv,rpmi-mpxy-tee",
		.data = &tee_data,
	},
	{},
};

/** TEE MPXY driver */
const struct fdt_driver fdt_mpxy_rpmi_tee = {
	.experimental = true,
	.match_table = tee_match,
	.init = mpxy_tee_init,
};

