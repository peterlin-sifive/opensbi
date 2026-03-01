# OP-TEE Dispatcher Device Tree Configuration

This document describes the device tree configuration for the OP-TEE dispatcher
in OpenSBI, which implements the RPMI TEE Service Group.

## Architecture Overview

The numbers (1)-(6) in the diagram below denote the boot flow sequence.

```
         (1)-----------+
          | U-Boot SPL |
          +------------+
                |
                v
         (2)-------------------------------------------------------------+
          | OpenSBI                                                      |
          |                (4)------------------------+                  |
          |                 | optee dispatcher        |                  |
          +-----------------+-------^---------|-------+------------------+
              |                     |         |                       ^
M-mode        |                     |         |                       |
--------------+--[trusted domain]---+----.----+--[untrusted domain]---+---
S-mode        |  (coldboot domain)  |    |    |                       |
        REQFWD_RETRIEVE   REQFWD_COMPLETE|    |                 TEE_COMMUNICATE
              v                     |    |    v                       |
         (3)---------------------------+ |(5)----------------------------+
          | OP-TEE OS                  | | | U-Boot                      |
          +----------------------------+ | +-----------------------------+
                                         |    |
                                         |    v
                                         |(6)----------------------------+
                                         | | Linux                       |
                                         | +-----------------------------+
```

**Runtime Communication**: Linux (6) → TEE SrvGrp → optee dispatcher (4) →
ReqFwd SrvGrp → OP-TEE (3) → response back

## Device Tree Example

```
/ {
    cpus {
        cpu0: cpu@0 {
            device_type = "cpu";
            reg = <0>;
            opensbi-domain = <&tdomain>; /* coldboot domain */

            rpmi_tee_0: rpmi-tee {
                compatible = "riscv,rpmi-mpxy-tee";
                riscv,sbi-mpxy-channel-id = <0x00>;
                tee-impl-id = <0>;  /* TEE Impl ID for OP-TEE */
                opensbi-domain-instance = <&tdomain>;
            };

            rpmi_reqfwd_0: rpmi-reqfwd {
                compatible = "riscv,sbi-mpxy-reqfwd";
                riscv,sbi-mpxy-channel-id = <0x10>;
            };
        };

        cpu1: cpu@1 {
            device_type = "cpu";
            reg = <1>;
            opensbi-domain = <&tdomain>; /* coldboot domain */

            rpmi_tee_1: rpmi-tee {
                compatible = "riscv,rpmi-mpxy-tee";
                riscv,sbi-mpxy-channel-id = <0x01>;
                tee-impl-id = <0>;  /* TEE Impl ID for OP-TEE */
                opensbi-domain-instance = <&tdomain>;
            };

            rpmi_reqfwd_1: rpmi-reqfwd {
                compatible = "riscv,sbi-mpxy-reqfwd";
                riscv,sbi-mpxy-channel-id = <0x11>;
            };
        };
    };

    opensbi-domains {
        compatible = "opensbi,domain,config";

        tmem: tmem {
            compatible = "opensbi,domain,memregion";
            base = <0x0 0xF1000000>;
            order = <24>;  /* 16 MiB */
        };

        allmem: allmem {
            compatible = "opensbi,domain,memregion";
            base = <0x0 0x0>;
            order = <64>;
        };

        tdomain: trusted-domain {
            compatible = "opensbi,domain,instance";
            regions = <&allmem 0x3f>;
            possible-harts = <&cpu0 &cpu1>;
            next-addr = <0x0 0xF1000000>;  /* OP-TEE: CFG_TDDRAM_START */
            next-mode = <0x1>;
        };

        udomain: untrusted-domain {
            compatible = "opensbi,domain,instance";
            regions = <&tmem 0x0>, <&allmem 0x3f>;
            possible-harts = <&cpu0 &cpu1>;
            boot-hart = <&cpu0>;
            next-addr = <0x0 0x81200000>;  /* U-Boot: CONFIG_TEXT_BASE */
            next-mode = <0x1>;
        };
    };

    mpxy_mbox: sbi-mpxy-mbox {
        compatible = "riscv,sbi-mpxy-mbox";
        #mbox-cells = <2>;
    };

    firmware {
        optee {
            compatible = "linaro,optee-tz";
            method = "mpxy";
            mboxes = <&mpxy_mbox 0x00 0x00>,
                     <&mpxy_mbox 0x01 0x00>;
        };
    };
};
```

## Notes

- Each hart requires its own `rpmi-tee` and `rpmi-reqfwd` nodes
- The `rpmi-tee` node searches for a sibling `rpmi-reqfwd` node
- Both nodes must be children of the same CPU node

