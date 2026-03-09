/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SiFive Inc.
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

static int tee_dispatcher_register(struct tee_dispatcher *dispatcher)
{
	struct tee_dispatcher *d;

	if (!dispatcher || !dispatcher->ops)
		return SBI_EINVAL;

	/* Validate mandatory operations */
	if (!dispatcher->ops->get_attributes ||
	    !dispatcher->ops->communicate)
		return SBI_EINVAL;

	sbi_list_for_each_entry(d, &tee_dispatcher_list, node) {
		if (d->impl_id == dispatcher->impl_id)
			return SBI_EALREADY;
	}

	sbi_list_add_tail(&dispatcher->node, &tee_dispatcher_list);

	return SBI_OK;
}

static struct tee_dispatcher *tee_dispatcher_find(u32 impl_id)
{
	struct tee_dispatcher *d;

	sbi_list_for_each_entry(d, &tee_dispatcher_list, node) {
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
	/* Placeholder - TEE implementation will be added here */
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
