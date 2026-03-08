/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SiFive
 *
 * TEE Dispatcher Registry
 */

#include <libfdt.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/tee/tee_dispatcher.h>

/** List of registered TEE dispatchers */
static SBI_LIST_HEAD(tee_dispatcher_list);

/**
 * Register a TEE dispatcher
 */
int tee_dispatcher_register(struct tee_dispatcher *dispatcher)
{
	if (!dispatcher || !dispatcher->ops)
		return SBI_EINVAL;

	/* Check for duplicate registration */
	struct tee_dispatcher *d;
	sbi_list_for_each_entry(d, &tee_dispatcher_list, head) {
		if (d->impl_id == dispatcher->impl_id)
			return SBI_EALREADY;
	}

	SBI_INIT_LIST_HEAD(&dispatcher->head);
	sbi_list_add_tail(&dispatcher->head, &tee_dispatcher_list);

	return SBI_OK;
}

/**
 * Unregister a TEE dispatcher
 */
void tee_dispatcher_unregister(struct tee_dispatcher *dispatcher)
{
	if (!dispatcher)
		return;

	sbi_list_del(&dispatcher->head);
}

/**
 * Find dispatcher by implementation ID
 */
struct tee_dispatcher *tee_dispatcher_find(u32 impl_id)
{
	struct tee_dispatcher *d;

	sbi_list_for_each_entry(d, &tee_dispatcher_list, head) {
		if (d->impl_id == impl_id)
			return d;
	}

	return NULL;
}

/**
 * Setup TEE dispatcher from device tree
 */
int tee_dispatcher_setup(const void *fdt, int nodeoff,
			 struct tee_dispatcher **out_dispatcher)
{
	struct tee_dispatcher *dispatcher;
	const fdt32_t *val;
	u32 impl_id;
	int rc, len;

	if (!out_dispatcher)
		return SBI_EINVAL;

	/* Get TEE implementation ID from DT */
	val = fdt_getprop(fdt, nodeoff, "tee-impl-id", &len);
	if (!val || len < 4)
		return SBI_EINVAL;

	impl_id = fdt32_to_cpu(*val);

	/* Check if dispatcher is already registered */
	dispatcher = tee_dispatcher_find(impl_id);
	if (dispatcher) {
		*out_dispatcher = dispatcher;
		return SBI_OK;
	}

	/* Allocate new dispatcher */
	dispatcher = sbi_zalloc(sizeof(*dispatcher));
	if (!dispatcher)
		return SBI_ENOMEM;

	/* Setup dispatcher based on implementation ID */
	switch (impl_id) {
	case RPMI_TEE_IMPL_ID_OPTEE:
		rc = optee_dispatcher_setup(fdt, nodeoff, dispatcher);
		break;
	default:
		rc = SBI_ENODEV;
		break;
	}

	if (rc) {
		sbi_free(dispatcher);
		return rc;
	}

	/* Register dispatcher */
	rc = tee_dispatcher_register(dispatcher);
	if (rc) {
		sbi_free(dispatcher);
		return rc;
	}

	*out_dispatcher = dispatcher;
	return SBI_OK;
}
