#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 SiFive Inc.
#

libsbiutils-objs-$(CONFIG_FDT_TEE) += tee/tee_dispatcher.o
libsbiutils-objs-$(CONFIG_FDT_TEE_OPTEE) += tee/optee_dispatcher.o
