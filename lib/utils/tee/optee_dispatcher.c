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
#include <sbi/sbi_scratch.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/tee/tee_dispatcher.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi_mbox.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>

/**
 * OP-TEE specific context
 *
 * This context holds OP-TEE domain reference and per-hart reqfwd channel IDs.
 * The domain is shared across all harts as OP-TEE has a single domain instance.
 * The reqfwd_channel_ids array is indexed by hart_index (0-based contiguous),
 * NOT hartid, to ensure correct indexing regardless of hartid assignment.
 *
 * The reqfwd_channel_ids pointer is dynamically allocated with exact size
 * needed (num_harts entries). A NULL pointer indicates the reqfwd channels
 * have not been initialized.
 */
struct optee_context {
	/** Pointer to OP-TEE domain */
	struct sbi_domain *domain;
	/** Per-hart reqfwd channel IDs, indexed by hart_index */
	u32 *reqfwd_channel_ids;
	/** Number of harts (size of reqfwd_channel_ids array) */
	u32 num_harts;
};

/**
 * Setup OP-TEE domain from device tree
 *
 * This function extracts the domain from the device tree and binds it to
 * the OP-TEE context. This MUST be called after sbi_domain_finalize() so
 * that all domains are available.
 *
 * Note: sbi_init.c guarantees that sbi_mpxy_init() runs AFTER
 * sbi_domain_finalize(), so the domain lookup should always succeed.
 * If it fails, it indicates a device tree configuration error.
 */
static int optee_domain_setup(const void *fdt, int nodeoff,
			      struct optee_context *ctx)
{
	struct sbi_domain *dom = NULL;
	const u32 *prop_instance;
	int len, offset;
	const char *domain_name;

	prop_instance = fdt_getprop(fdt, nodeoff, "opensbi-domain-instance",
				    &len);
	if (!prop_instance || len < 4)
		return SBI_EINVAL;

	offset = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*prop_instance));
	if (offset < 0)
		return SBI_EINVAL;

	domain_name = fdt_get_name(fdt, offset, NULL);
	if (!domain_name)
		return SBI_EINVAL;

	/* Find and bind the domain */
	sbi_domain_for_each(dom) {
		if (!sbi_strcmp(dom->name, domain_name)) {
			ctx->domain = dom;
			return SBI_OK;
		}
	}

	/*
	 * Domain not found - this is a configuration error.
	 * Either the domain name in DT is wrong, or sbi_mpxy_init() was
	 * called before sbi_domain_finalize() (which violates the expected
	 * initialization order in sbi_init.c).
	 */
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
 * OP-TEE response transformation callback
 *
 * OP-TEE returns 5 unsigned longs (a0-a4) where:
 *
 *   a0 = TEEABI_OPTEED_RETURN_* code (internal signal to dispatcher)
 *        This register contains an internal OP-TEE ABI code that signals
 *        what type of event completed. On RISC-V, only these are used:
 *
 *          - TEEABI_OPTEED_RETURN_ENTRY_DONE  (0xBE000001)
 *            OP-TEE primary hart initialization complete.
 *
 *          - TEEABI_OPTEED_RETURN_ON_DONE     (0xBE000002)
 *            OP-TEE secondary hart boot complete.
 *
 *          - TEEABI_OPTEED_RETURN_CALL_DONE   (0xBE000005)
 *            A standard SMC call to OP-TEE has completed.
 *            This is the most common code for normal TEE operations.
 *
 *          - TEEABI_OPTEED_RETURN_FIQ_DONE    (0xBE000006)
 *            OP-TEE has finished handling a forwarded FIQ.
 *
 *        Note: OFF_DONE, SUSPEND_DONE, RESUME_DONE, SYSTEM_OFF_DONE,
 *        SYSTEM_RESET_DONE are defined but NOT used on RISC-V because
 *        power management is handled directly by OpenSBI SBI extensions
 *        (HSM, SRST), not through OP-TEE.
 *
 *        These codes are internal signals between OP-TEE and OpenSBI.
 *        They are NOT part of the SMC calling convention that Linux uses.
 *
 *   a1 = SMC return value (e.g., OPTEE_SMC_RETURN_OK)
 *        This becomes the caller's a0.
 *
 *   a2-a4 = Additional return values
 *        These become the caller's a1-a3.
 *
 * This callback ALWAYS strips a0, regardless of which TEEABI_OPTEED_RETURN_*
 * code it contains, because all return codes are internal dispatcher signals.
 * Linux expects SMC results in a0-a3 per the SMC Calling Convention.
 */
static int optee_transform_response(void *tx, u32 tx_len,
				    void *rx, u32 rx_max_len,
				    unsigned long *rx_len)
{
	u32 copy_len;

	/* Must have at least one register (a0) to skip */
	if (tx_len < sizeof(ulong))
		return SBI_EINVAL;

	/* Calculate length after skipping a0 */
	copy_len = tx_len - sizeof(ulong);

	/* Check destination buffer size */
	if (copy_len > rx_max_len)
		return SBI_ENOMEM;

	/* Copy a1-a4 to rx, skipping a0 (TEEABI_OPTEED_RETURN_*) */
	sbi_memcpy(rx, &(((ulong *)tx)[1]), copy_len);
	*rx_len = copy_len;

	return SBI_OK;
}

/**
 * OP-TEE communicate - forward message and enter domain
 *
 * OP-TEE uses SMC-style parameters (a0-a7) for communication.
 * The request data contains 8 unsigned long values representing
 * the SMC parameters, and the response contains 4 return values.
 *
 * The per-hart reqfwd channel is looked up using current_hartindex(),
 * which returns the 0-based contiguous hart index.
 *
 * @param dispatcher: TEE dispatcher instance
 * @param tx_data: Request data buffer
 * @param tx_len: Size of request data
 * @param rx_data: Response data buffer
 * @param rx_max_len: Maximum size of response buffer
 * @param rx_len: Actual size of response data written
 * @return 0 on success, negative error code on failure
 */
static int optee_communicate(const struct tee_dispatcher *dispatcher,
			     void *tx_data, u32 tx_len,
			     void *rx_data, u32 rx_max_len,
			     unsigned long *rx_len)
{
	struct optee_context *ctx = dispatcher->context;
	struct sbi_mpxy_channel *recv_channel;
	struct rpmi_message_header header;
	u32 hart_index;
	u32 reqfwd_channel_id;
	int rc;

	if (!ctx)
		return SBI_EINVAL;

	/* Check if reqfwd channels have been initialized */
	if (!ctx->reqfwd_channel_ids)
		return SBI_EINVAL;

	/* Get current hart's index and look up its reqfwd channel ID */
	hart_index = current_hartindex();
	if (hart_index >= ctx->num_harts)
		return SBI_EINVAL;

	reqfwd_channel_id = ctx->reqfwd_channel_ids[hart_index];
	if (reqfwd_channel_id == 0)
		return SBI_ENODEV;

	/* Find per-hart request forward channel */
	recv_channel = sbi_mpxy_find_channel(reqfwd_channel_id);
	if (!recv_channel)
		return SBI_ENODEV;

	/* Prepare the header for forwarding to OP-TEE domain */
	header.servicegroup_id = cpu_to_le16(RPMI_SRVGRP_TEE);
	header.service_id = RPMI_TEE_SRV_COMMUNICATE;
	header.flags = RPMI_MSG_NORMAL_REQUEST;
	header.datalen = cpu_to_le16(tx_len);
	header.token = cpu_to_le16(0);

	/*
	 * Forward message to request forward channel.
	 * Pass OP-TEE specific response transformation callback to skip a0
	 * (internal TEEABI return code) when copying response to caller.
	 */
	rc = mpxy_reqfwd_forward_message(recv_channel, &header,
					 tx_data, tx_len,
					 rx_data, rx_max_len,
					 rx_len,
					 optee_transform_response);

	return rc;
}

/**
 * Enter OP-TEE domain
 *
 * This function switches execution context to the OP-TEE domain.
 * It blocks until OP-TEE processing completes and domain_exit is called.
 */
static int optee_domain_enter(const struct tee_dispatcher *dispatcher)
{
	struct optee_context *ctx = dispatcher->context;

	if (!ctx)
		return SBI_EINVAL;

	if (!ctx->domain)
		return SBI_ENOENT;

	return sbi_domain_context_enter(ctx->domain);
}

/** OP-TEE dispatcher operations */
static const struct tee_dispatcher_ops optee_ops = {
	.get_attributes = optee_get_attributes,
	.communicate = optee_communicate,
	.domain_enter = optee_domain_enter,
};

/**
 * Find reqfwd channel ID for a CPU node
 *
 * Searches for a sibling "riscv,sbi-mpxy-reqfwd" node under the same CPU node
 * and returns its channel ID.
 *
 * @param fdt: Device tree blob
 * @param cpu_offset: CPU node offset
 * @param channel_id: Output parameter for the reqfwd channel ID
 * @return true if found, false otherwise
 */
static bool optee_find_reqfwd_channel(const void *fdt, int cpu_offset,
				      u32 *channel_id)
{
	int child_offset, sibling_offset;
	const fdt32_t *val;
	int len;

	/* Find TEE node under this CPU */
	fdt_for_each_subnode(child_offset, fdt, cpu_offset) {
		if (fdt_node_check_compatible(fdt, child_offset,
					      "riscv,rpmi-mpxy-tee"))
			continue;

		/* Found TEE node, now find sibling reqfwd node */
		fdt_for_each_subnode(sibling_offset, fdt, cpu_offset) {
			if (fdt_node_check_compatible(fdt, sibling_offset,
						      "riscv,sbi-mpxy-reqfwd"))
				continue;

			val = fdt_getprop(fdt, sibling_offset,
					  "riscv,sbi-mpxy-channel-id", &len);
			if (len > 0 && val) {
				*channel_id = fdt32_to_cpu(*val);
				return true;
			}
		}
		break;
	}

	return false;
}

/**
 * Count harts with reqfwd channels and find max hart_index
 *
 * Iterates all CPU nodes to determine how many harts have reqfwd channels
 * and what the maximum hart_index is (to size the allocation).
 *
 * @param fdt: Device tree blob
 * @param cpus_offset: /cpus node offset
 * @param max_hart_index: Output parameter for maximum hart_index found
 * @return Number of harts with reqfwd channels, 0 if none found
 */
static u32 optee_count_reqfwd_harts(const void *fdt, int cpus_offset,
				    u32 *max_hart_index)
{
	int cpu_offset;
	u32 hartid, hart_index;
	u32 channel_id;
	u32 count = 0;
	int rc;

	*max_hart_index = 0;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		if (fdt_node_check_compatible(fdt, cpu_offset, "riscv") != 0)
			continue;

		rc = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
		if (rc)
			continue;

		hart_index = sbi_hartid_to_hartindex(hartid);
		if (hart_index >= SBI_HARTMASK_MAX_BITS)
			continue;

		if (optee_find_reqfwd_channel(fdt, cpu_offset, &channel_id)) {
			if (hart_index > *max_hart_index)
				*max_hart_index = hart_index;
			count++;
		}
	}

	return count;
}

/**
 * Populate reqfwd channel IDs array
 *
 * Iterates all CPU nodes and fills in the reqfwd channel ID for each hart.
 *
 * @param fdt: Device tree blob
 * @param cpus_offset: /cpus node offset
 * @param channel_ids: Array to populate (indexed by hart_index)
 * @param num_harts: Size of channel_ids array
 */
static void optee_populate_reqfwd_channels(const void *fdt, int cpus_offset,
					   u32 *channel_ids, u32 num_harts)
{
	int cpu_offset;
	u32 hartid, hart_index;
	u32 channel_id;
	int rc;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		if (fdt_node_check_compatible(fdt, cpu_offset, "riscv") != 0)
			continue;

		rc = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
		if (rc)
			continue;

		hart_index = sbi_hartid_to_hartindex(hartid);
		if (hart_index >= num_harts)
			continue;

		if (optee_find_reqfwd_channel(fdt, cpu_offset, &channel_id))
			channel_ids[hart_index] = channel_id;
	}
}

/**
 * Setup per-hart reqfwd channels from device tree
 *
 * This function iterates ALL CPU nodes in the device tree and sets up
 * reqfwd channel IDs for each hart. Uses a two-pass approach:
 *   1. First pass: count harts and find max hart_index to determine array size
 *   2. Second pass: fill in the channel IDs
 *
 * @param fdt: Device tree blob
 * @param ctx: OP-TEE context to populate
 * @return 0 on success, negative error code on failure
 */
static int optee_reqfwd_channels_setup(const void *fdt,
				       struct optee_context *ctx)
{
	int cpus_offset;
	u32 max_hart_index = 0;
	u32 num_harts, count;

	/* Find /cpus node */
	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return SBI_ENOENT;

	count = optee_count_reqfwd_harts(fdt, cpus_offset, &max_hart_index);
	if (count == 0)
		return SBI_ENOENT;

	num_harts = max_hart_index + 1;
	ctx->reqfwd_channel_ids = sbi_zalloc(num_harts * sizeof(u32));
	if (!ctx->reqfwd_channel_ids)
		return SBI_ENOMEM;

	ctx->num_harts = num_harts;

	optee_populate_reqfwd_channels(fdt, cpus_offset,
				       ctx->reqfwd_channel_ids, num_harts);

	return SBI_OK;
}

/**
 * Setup OP-TEE dispatcher from device tree
 *
 * This function creates a shared OP-TEE dispatcher context and sets up
 * per-hart reqfwd channels. The dispatcher is shared across all harts,
 * while per-hart reqfwd_channel_ids are stored in the optee_context.
 *
 * All OP-TEE setup is done here in one place:
 *   1. Allocate OP-TEE context
 *   2. Setup OP-TEE domain
 *   3. Setup per-hart reqfwd channels (iterates ALL CPU nodes)
 *   4. Initialize dispatcher structure
 */
int optee_dispatcher_setup(const void *fdt, int nodeoff,
			   struct tee_dispatcher *dispatcher)
{
	struct optee_context *ctx;
	int rc;

	/* Allocate OP-TEE context */
	ctx = sbi_zalloc(sizeof(*ctx));
	if (!ctx)
		return SBI_ENOMEM;

	/* Setup OP-TEE domain */
	rc = optee_domain_setup(fdt, nodeoff, ctx);
	if (rc)
		goto fail_free_ctx;

	/* Setup per-hart reqfwd channels from device tree */
	rc = optee_reqfwd_channels_setup(fdt, ctx);
	if (rc)
		goto fail_free_ctx;

	/* Setup dispatcher */
	dispatcher->impl_id = RPMI_TEE_IMPL_ID_OPTEE;
	dispatcher->name = "OP-TEE";
	dispatcher->ops = &optee_ops;
	dispatcher->context = ctx;

	return SBI_OK;

fail_free_ctx:
	if (ctx->reqfwd_channel_ids)
		sbi_free(ctx->reqfwd_channel_ids);
	sbi_free(ctx);
	return rc;
}
