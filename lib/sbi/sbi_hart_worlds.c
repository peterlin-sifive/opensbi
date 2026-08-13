/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 SiFive Inc.
 */

#include <sbi/riscv_encoding.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hart_protection.h>
#include <sbi/sbi_hart_worlds.h>
#include <sbi/sbi_scratch.h>

static int sbi_hart_worlds_configure(struct sbi_scratch *scratch,
				     struct sbi_domain *dom)
{
	struct sbi_hart_features *hf = sbi_hart_features_ptr(scratch);
	bool wid_found = true;
	u32 wid_val = 0;

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMLWID))
		return 0;

	if (dom->has_wid) {
		wid_val = dom->wid;
	} else if (sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWID)) {
		wid_val = csr_read(CSR_MWID) & ~MWID_LOCK;
	} else if (hf->has_pmwid) {
		wid_val = hf->pmwid;
	} else {
		/*
		 * Smlwid present but no WID source. Without writing
		 * mlwid, the hardware reset value is used which may
		 * cause a software-check exception on lower-privilege
		 * mode entry.
		 */
		wid_found = false;
	}

	if (wid_found) {
		if (hf->has_pmlwidlist &&
		    !(hf->pmlwidlist & BIT_ULL(wid_val))) {
			sbi_printf("%s: domain wid %u not in pmlwidlist\n",
				   __func__, wid_val);
			return SBI_EINVAL;
		}
		csr_write(CSR_MLWID, wid_val);
	}

	if (sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWIDDELEG))
		csr_write(CSR_MWIDDELEG, dom->widdeleg);

	return 0;
}

static struct sbi_hart_protection wid_protection = {
	.name		= "wid",
	.rating		= 100,
	.type		= SBI_HART_PROTECTION_TYPE_ID,
	.configure	= sbi_hart_worlds_configure,
	.unconfigure	= NULL
};

int sbi_hart_worlds_init(struct sbi_scratch *scratch)
{
	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMLWID))
		return 0;

	return sbi_hart_protection_register(&wid_protection);
}
