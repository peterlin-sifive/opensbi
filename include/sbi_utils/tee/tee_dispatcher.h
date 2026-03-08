/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SiFive
 *
 * TEE Dispatcher Interface
 *
 * This interface provides an abstraction layer for different TEE
 * implementations. Each TEE implementation (e.g., OP-TEE) registers
 * a dispatcher that handles TEE-specific operations.
 */

#ifndef __TEE_DISPATCHER_H__
#define __TEE_DISPATCHER_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_list.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>

/**
 * TEE Attributes
 * Returned by TEE_GET_ATTRIBUTES service
 */
struct tee_attributes {
	/** TEE implementation identifier */
	u32 tee_impl_id;
	/** Number of XLEN-bit values in TEE_COMMUNICATE request */
	u32 comm_req_regs;
	/** Number of XLEN-bit values in TEE_COMMUNICATE response */
	u32 comm_resp_regs;
};

struct tee_dispatcher;

/**
 * TEE Dispatcher Operations
 * Each TEE implementation must provide these operations
 */
struct tee_dispatcher_ops {
	/**
	 * Get TEE attributes
	 * @param dispatcher: TEE dispatcher instance
	 * @param attr: Output attributes structure
	 * @return 0 on success, negative error code on failure
	 */
	int (*get_attributes)(const struct tee_dispatcher *dispatcher,
			      struct tee_attributes *attr);

	/**
	 * Invoke TEE service (TEE_COMMUNICATE)
	 * Data format is implementation-specific.
	 * @param dispatcher: TEE dispatcher instance
	 * @param tx_data: Request data buffer (implementation-specific format)
	 * @param tx_len: Size of request data
	 * @param rx_data: Response data buffer (implementation-specific format)
	 * @param rx_max_len: Maximum size of response buffer
	 * @param rx_len: Actual size of response data written
	 * @return 0 on success, negative error code on failure
	 */
	int (*communicate)(const struct tee_dispatcher *dispatcher,
			   void *tx_data, u32 tx_len,
			   void *rx_data, u32 rx_max_len,
			   unsigned long *rx_len);

	/**
	 * Enter TEE domain (optional, for domain-based TEEs)
	 * @param dispatcher: TEE dispatcher instance
	 * @return 0 on success, negative error code on failure
	 */
	int (*domain_enter)(const struct tee_dispatcher *dispatcher);
};

/**
 * TEE Dispatcher
 * Represents a registered TEE implementation
 */
struct tee_dispatcher {
	/** List head for dispatcher registry */
	struct sbi_dlist head;
	/** TEE implementation ID (TEE_IMPL_ID_xxx) */
	u32 impl_id;
	/** Name of the TEE */
	const char *name;
	/** Dispatcher operations */
	const struct tee_dispatcher_ops *ops;
	/** Dispatcher-specific context */
	void *context;
};

/**
 * Register a TEE dispatcher
 * @param dispatcher: Dispatcher to register
 * @return 0 on success, negative error code on failure
 */
int tee_dispatcher_register(struct tee_dispatcher *dispatcher);

/**
 * Unregister a TEE dispatcher
 * @param dispatcher: Dispatcher to unregister
 */
void tee_dispatcher_unregister(struct tee_dispatcher *dispatcher);

/**
 * Find dispatcher by implementation ID
 * @param impl_id: TEE implementation ID
 * @return Dispatcher pointer or NULL if not found
 */
struct tee_dispatcher *tee_dispatcher_find(u32 impl_id);

/**
 * Setup TEE dispatcher from device tree
 * @param fdt: Flattened device tree pointer
 * @param nodeoff: Node offset in device tree
 * @param dispatcher: Output dispatcher pointer
 * @return 0 on success, negative error code on failure
 */
int tee_dispatcher_setup_from_fdt(const void *fdt, int nodeoff,
				  struct tee_dispatcher **dispatcher);

#endif /* __TEE_DISPATCHER_H__ */

