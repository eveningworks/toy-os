#ifndef AQ_REGS_H
#define AQ_REGS_H

// The Aquantia AQtion register map, as far as aq.c uses it. BAR0, 32-bit
// registers. Addresses and bits are OpenBSD's if_aq_pci.c (see aq.c and
// LICENSE); names keep its spelling so the two can be read side by side.
// A2-only registers carry AQ2_.

#define AQUANTIA_VENDOR 0x1D6A

// --- global / firmware ---------------------------------------------------
#define AQ_FW_SOFTRESET_REG        0x0000
#define  AQ_FW_SOFTRESET_DIS       (1u << 14)
#define  AQ_FW_SOFTRESET_RESET     (1u << 15)
#define AQ_FW_VERSION_REG          0x0018
#define AQ_HW_REVISION_REG         0x001C
#define AQ2_HW_FPGA_VERSION_REG    0x00F4
#define AQ_GLB_NVR_INTERFACE1_REG  0x0100
#define AQ_FW_MBOX_CMD_REG         0x0200
#define  AQ_FW_MBOX_CMD_EXECUTE    0x00008000u
#define  AQ_FW_MBOX_CMD_BUSY       0x00000100u
#define AQ_FW_MBOX_ADDR_REG        0x0208
#define AQ_FW_MBOX_VAL_REG         0x020C
#define FW_MPI_MBOX_ADDR_REG       0x0360
#define FW1X_MPI_INIT1_REG         0x0364
#define FW2X_MPI_EFUSEADDR_REG     0x0364
#define FW1X_MPI_CONTROL_REG       0x0368
#define FW2X_MPI_CONTROL_REG       0x0368   // 64-bit
#define FW1X_MPI_STATE_REG         0x036C
#define FW1X_MPI_INIT2_REG         0x0370
#define FW2X_MPI_STATE_REG         0x0370   // 64-bit
#define FW1X_MPI_EFUSEADDR_REG     0x0374
#define FW_BOOT_EXIT_CODE_REG      0x0388
#define  RBL_STATUS_DEAD           0x0000DEADu
#define  RBL_STATUS_SUCCESS        0x0000ABBAu
#define  RBL_STATUS_FAILURE        0x00000BADu
#define  RBL_STATUS_HOST_BOOT      0x0000F1A7u
#define AQ_FW_GLB_CPU_SEM_REG(i)   (0x03A0 + (i) * 4)
#define AQ_FW_SEM_RAM_REG          AQ_FW_GLB_CPU_SEM_REG(2)
#define AQ2_ART_SEM_REG            AQ_FW_GLB_CPU_SEM_REG(3)
#define AQ_FW_GLB_CTL2_REG         0x0404
#define AQ_GLB_GENERAL_PROVISIONING9_REG 0x0520
#define AQ_GLB_NVR_PROVISIONING2_REG     0x0534
#define FW_MPI_DAISY_CHAIN_STATUS_REG    0x0704
#define AQ_PCI_REG_CONTROL_6_REG   0x1014
#define AQ_MBOXIF_POWER_GATING_CONTROL_REG 0x32A8
#define FW_MPI_RESETCTRL_REG       0x4000
#define  FW_MPI_RESETCTRL_RESET_DIS (1u << 29)

// --- interrupts ----------------------------------------------------------
#define AQ_INTR_STATUS_REG         0x2000
#define AQ_INTR_STATUS_CLR_REG     0x2050
#define AQ_INTR_MASK_REG           0x2060   // write 1 to unmask
#define AQ_INTR_MASK_CLR_REG       0x2070   // write 1 to mask
#define AQ_INTR_AUTOMASK_REG       0x2090
// Ring i's cause -> interrupt bit; two rings per register.
#define AQ_INTR_IRQ_MAP_TXRX_REG(i)     (0x2100 + ((i) / 2) * 4)
#define  AQ_INTR_IRQ_MAP_TX_IRQMAP(i)   (0x1Fu << (((i) & 1) ? 16 : 24))
#define  AQ_INTR_IRQ_MAP_TX_EN(i)       (1u << (((i) & 1) ? 23 : 31))
#define  AQ_INTR_IRQ_MAP_RX_IRQMAP(i)   (0x1Fu << (((i) & 1) ? 0 : 8))
#define  AQ_INTR_IRQ_MAP_RX_EN(i)       (1u << (((i) & 1) ? 7 : 15))
#define AQ_GEN_INTR_MAP_REG(i)     (0x2180 + (i) * 4)
#define  AQ_B0_ERR_INT             8u
#define AQ_INTR_CTRL_REG           0x2300
#define  AQ_INTR_CTRL_IRQMODE      0x3u
#define  AQ_INTR_CTRL_IRQMODE_LEGACY 0
#define  AQ_INTR_CTRL_IRQMODE_MSI    1
#define  AQ_INTR_CTRL_IRQMODE_MSIX   2
#define  AQ_INTR_CTRL_MULTIVEC     (1u << 2)
#define  AQ_INTR_CTRL_RESET_DIS    (1u << 29)
#define  AQ_INTR_CTRL_RESET_IRQ    (1u << 31)

// --- receive -------------------------------------------------------------
#define RX_SYSCONTROL_REG          0x5000
#define  RX_SYSCONTROL_RESET_DIS   (1u << 29)
#define RX_TCP_RSS_HASH_REG        0x5040
#define  RX_TCP_RSS_HASH_RPF2      (0xFu << 16)
#define  RX_TCP_RSS_HASH_TYPE      0xFFFFu
#define AQ2_RPF_L2BC_TAG_REG       0x50F0
#define  AQ2_RPF_L2BC_TAG_MASK     0x0000003Fu
#define RPF_L2BC_REG               0x5100
#define  RPF_L2BC_EN               (1u << 0)
#define  RPF_L2BC_PROMISC          (1u << 3)
#define  RPF_L2BC_ACTION           0x7000u
#define  RPF_L2BC_THRESHOLD        0xFFFF0000u
#define AQ2_RPF_NEW_CTRL_REG       0x5104
#define  AQ2_RPF_NEW_CTRL_ENABLE   (1u << 11)
#define RPF_L2UC_LSW_REG(i)        (0x5110 + (i) * 8)
#define RPF_L2UC_MSW_REG(i)        (0x5114 + (i) * 8)
#define  RPF_L2UC_MSW_MACADDR_HI   0xFFFFu
#define  RPF_L2UC_MSW_ACTION       0x70000u
#define  RPF_L2UC_MSW_TAG          0x03C00000u
#define  RPF_L2UC_MSW_EN           (1u << 31)
#define AQ_HW_MAC_NUM              34
#define RPF_ACTION_HOST            1
#define RPF_MCAST_FILTER_REG(i)    (0x5250 + (i) * 4)
#define RPF_MCAST_FILTER_MASK_REG  0x5270
#define RPF_VLAN_MODE_REG          0x5280
#define  RPF_VLAN_MODE_PROMISC     (1u << 1)
#define  RPF_VLAN_MODE_ACCEPT_UNTAGGED (1u << 2)
#define  RPF_VLAN_MODE_UNTAGGED_ACTION 0x38u
#define RPF_VLAN_TPID_REG          0x5284
#define  RPF_VLAN_TPID_OUTER       0xFFFF0000u
#define  RPF_VLAN_TPID_INNER       0xFFFFu
#define RPF_ETHERTYPE_FILTER_REG(i) (0x5300 + (i) * 4)
#define  RPF_ETHERTYPE_FILTER_EN   (1u << 31)
#define RX_FLR_RSS_CONTROL1_REG    0x54C0
#define RPF_RPB_RX_TC_UPT_REG      0x54C4
#define AQ2_RPF_REDIR2_REG         0x54C8
#define  AQ2_RPF_REDIR2_HASHTYPE   0x000001FFu
#define RPO_HWCSUM_REG             0x5580
#define RPB_RPF_RX_REG             0x5700
#define  RPB_RPF_RX_TC_MODE        (1u << 8)
#define  RPB_RPF_RX_FC_MODE        0x30u
#define  RPB_RPF_RX_BUF_EN         (1u << 0)
#define RPB_RXB_BUFSIZE_REG(i)     (0x5710 + (i) * 0x10)
#define  RPB_RXB_BUFSIZE           0x1FFu
#define RPB_RXB_XOFF_REG(i)        (0x5714 + (i) * 0x10)
#define  RPB_RXB_XOFF_EN           (1u << 31)
#define  RPB_RXB_XOFF_THRESH_HI    0x3FFF0000u
#define  RPB_RXB_XOFF_THRESH_LO    0x3FFFu
#define AQ2_RX_Q_TC_MAP_REG(i)     (0x5900 + (i) * 4)
#define RX_DMA_DESC_CACHE_INIT_REG 0x5A00
#define  RX_DMA_DESC_CACHE_INIT    (1u << 0)
#define RX_DMA_INT_DESC_WRWB_EN_REG 0x5A30
#define  RX_DMA_INT_DESC_WRWB_EN   (1u << 2)
#define  RX_DMA_INT_DESC_MODERATE_EN (1u << 3)
#define RX_INTR_MODERATION_CTL_REG(i) (0x5A40 + (i) * 4)
#define  RX_INTR_MODERATION_CTL_EN  (1u << 1)
#define  RX_INTR_MODERATION_CTL_MIN (0xFFu << 8)
#define  RX_INTR_MODERATION_CTL_MAX (0x1FFu << 16)
#define RX_DMA_DESC_BASE_ADDRLSW_REG(i) (0x5B00 + (i) * 0x20)
#define RX_DMA_DESC_BASE_ADDRMSW_REG(i) (0x5B04 + (i) * 0x20)
#define RX_DMA_DESC_REG(i)         (0x5B08 + (i) * 0x20)
#define  RX_DMA_DESC_LEN           (0x3FFu << 3)
#define  RX_DMA_DESC_HEADER_SPLIT  (1u << 28)
#define  RX_DMA_DESC_VLAN_STRIP    (1u << 29)
#define  RX_DMA_DESC_EN            (1u << 31)
#define RX_DMA_DESC_HEAD_PTR_REG(i) (0x5B0C + (i) * 0x20)
#define  RX_DMA_DESC_HEAD_PTR      0xFFFu
#define RX_DMA_DESC_TAIL_PTR_REG(i) (0x5B10 + (i) * 0x20)
#define RX_DMA_DESC_BUFSIZE_REG(i) (0x5B18 + (i) * 0x20)
#define  RX_DMA_DESC_BUFSIZE_DATA  0x000Fu
#define  RX_DMA_DESC_BUFSIZE_HDR   0x0FF0u
#define RX_DMA_DCAD_REG(i)         (0x6100 + (i) * 4)
#define RX_DMA_DCA_REG             0x6180
#define AQ2_RPF_REC_TAB_ENABLE_REG 0x6FF0
#define  AQ2_RPF_REC_TAB_ENABLE_MASK 0x0000FFFFu

// --- transmit ------------------------------------------------------------
#define TX_SYSCONTROL_REG          0x7000
#define  TX_SYSCONTROL_RESET_DIS   (1u << 29)
#define TX_TPO2_REG                0x7040
#define  TX_TPO2_EN                (1u << 16)
#define TPS_DATA_TC_ARB_MODE_REG   0x7100
#define  TPS_DATA_TC_ARB_MODE      (1u << 0)
#define TPS_DATA_TCT_REG(i)        (0x7110 + (i) * 4)
#define  TPS_DATA_TCT_CREDIT_MAX   0x0FFF0000u
#define  TPS_DATA_TCT_WEIGHT       0x1FFu
#define  TPS2_DATA_TCT_CREDIT_MAX  0xFFFF0000u
#define  TPS2_DATA_TCT_WEIGHT      0x7FFFu
#define TPS_DESC_TC_ARB_MODE_REG   0x7200
#define  TPS_DESC_TC_ARB_MODE      0x3u
#define TPS_DESC_TCT_REG(i)        (0x7210 + (i) * 4)
#define  TPS_DESC_TCT_CREDIT_MAX   0x0FFF0000u
#define  TPS_DESC_TCT_WEIGHT       0x1FFu
#define TPS_DESC_VM_ARB_MODE_REG   0x7300
#define  TPS_DESC_VM_ARB_MODE      (1u << 0)
#define TPS_DESC_RATE_REG          0x7310
#define  TPS_DESC_RATE_TA_RST      (1u << 31)
#define  TPS_DESC_RATE_LIM         0x7FFu
#define TPO_HWCSUM_REG             0x7800
#define THM_LSO_TCP_FLAG1_REG      0x7820
#define  THM_LSO_TCP_FLAG1_FIRST   0xFFFu
#define  THM_LSO_TCP_FLAG1_MID     0x0FFF0000u
#define THM_LSO_TCP_FLAG2_REG      0x7824
#define  THM_LSO_TCP_FLAG2_LAST    0xFFFu
#define TPB_TX_BUF_REG             0x7900
#define  TPB_TX_BUF_EN             (1u << 0)
#define  TPB_TX_BUF_SCP_INS_EN     (1u << 2)
#define  TPB_TX_BUF_CLK_GATE_EN    (1u << 5)
#define  TPB_TX_BUF_TC_MODE_EN     (1u << 8)
#define  TPB_TX_BUF_TC_Q_RAND_MAP_EN (1u << 9)
#define TPB_TXB_BUFSIZE_REG(i)     (0x7910 + (i) * 0x10)
#define  TPB_TXB_BUFSIZE           0xFFu
#define TPB_TXB_THRESH_REG(i)      (0x7914 + (i) * 0x10)
#define  TPB_TXB_THRESH_HI         0x1FFF0000u
#define  TPB_TXB_THRESH_LO         0x1FFFu
#define AQ2_TX_Q_TC_MAP_REG(i)     (0x799C + (i) * 4)
#define AQ2_LAUNCHTIME_CTRL_REG    0x7A1C
#define  AQ2_LAUNCHTIME_CTRL_RATIO 0x0000FF00u
#define AQ_HW_TX_DMA_TOTAL_REQ_LIMIT_REG 0x7B20
#define TX_DMA_INT_DESC_WRWB_EN_REG 0x7B40
#define  TX_DMA_INT_DESC_WRWB_EN   (1u << 1)
#define  TX_DMA_INT_DESC_MODERATE_EN (1u << 4)
#define TX_DMA_DESC_BASE_ADDRLSW_REG(i) (0x7C00 + (i) * 0x40)
#define TX_DMA_DESC_BASE_ADDRMSW_REG(i) (0x7C04 + (i) * 0x40)
#define TX_DMA_DESC_REG(i)         (0x7C08 + (i) * 0x40)
#define  TX_DMA_DESC_LEN           0x00000FF8u
#define  TX_DMA_DESC_EN            0x80000000u
#define TX_DMA_DESC_HEAD_PTR_REG(i) (0x7C0C + (i) * 0x40)
#define  TX_DMA_DESC_HEAD_PTR      0x00000FFFu
#define TX_DMA_DESC_TAIL_PTR_REG(i) (0x7C10 + (i) * 0x40)
#define TX_DMA_DESC_WRWB_THRESH_REG(i) (0x7C18 + (i) * 0x40)
#define AQ2_TX_INTR_MODERATION_CTL_REG(i) (0x7C28 + (i) * 0x40)
#define TDM_DCAD_REG(i)            (0x8400 + (i) * 4)
#define TDM_DCA_REG                0x8480
#define TX_INTR_MODERATION_CTL_REG(i) (0x8980 + (i) * 4)

// --- A2: the firmware's boot and its shared-memory interface -----------
#define AQ2_MIF_HOST_FINISHED_STATUS_WRITE_REG 0x0E00
#define AQ2_MIF_HOST_FINISHED_STATUS_READ_REG  0x0E04
#define  AQ2_MIF_HOST_FINISHED_STATUS_ACK      (1u << 0)
#define AQ2_MCP_HOST_REQ_INT_REG       0x0F00
#define  AQ2_MCP_HOST_REQ_INT_READY    (1u << 0)
#define AQ2_MCP_HOST_REQ_INT_CLR_REG   0x0F08
#define AQ2_MIF_BOOT_REG               0x3040
#define  AQ2_MIF_BOOT_BOOT_STARTED     (1u << 24)
#define  AQ2_MIF_BOOT_FW_INIT_FAILED   (1u << 29)
#define  AQ2_MIF_BOOT_FW_INIT_COMP_SUCCESS (1u << 31)

// The IN buffer is the host's half (requests), OUT the firmware's.
#define AQ2_FW_IN_MTU_REG              0x12000
#define AQ2_FW_IN_MAC_ADDRESS_REG      0x12008
#define AQ2_FW_IN_LINK_CONTROL_REG     0x12010
#define  AQ2_FW_IN_LINK_CONTROL_MODE   0x0000000Fu
#define  AQ2_LINK_MODE_ACTIVE          1
#define  AQ2_LINK_MODE_SHUTDOWN        4
#define AQ2_FW_IN_LINK_OPTIONS_REG     0x12018
#define  AQ2_LINK_OPT_PAUSE_TX         (1u << 25)
#define  AQ2_LINK_OPT_PAUSE_RX         (1u << 24)
#define  AQ2_LINK_OPT_EEE_MASK         0x001F0000u   // 100M, 1G, 2.5G, 5G, 10G
#define  AQ2_LINK_OPT_RATE_10G         (1u << 15)
#define  AQ2_LINK_OPT_RATE_N5G         (1u << 14)
#define  AQ2_LINK_OPT_RATE_5G          (1u << 13)
#define  AQ2_LINK_OPT_RATE_N2G5        (1u << 12)
#define  AQ2_LINK_OPT_RATE_2G5         (1u << 11)
#define  AQ2_LINK_OPT_RATE_1G          (1u << 10)
#define  AQ2_LINK_OPT_RATE_100M        (1u << 9)
#define  AQ2_LINK_OPT_RATE_10M         (1u << 8)
#define  AQ2_LINK_OPT_RATE_1G_HD       (1u << 7)
#define  AQ2_LINK_OPT_RATE_100M_HD     (1u << 6)
#define  AQ2_LINK_OPT_RATE_10M_HD      (1u << 5)
#define  AQ2_LINK_OPT_RATE_MASK        0x0000FFE0u
#define  AQ2_LINK_OPT_LINK_UP          (1u << 0)
#define AQ2_FW_IN_REQUEST_POLICY_REG   0x12A58
#define  AQ2_POLICY_MCAST_QUEUE_OR_TC  0x00800000u
#define  AQ2_POLICY_MCAST_RX_INDEX     0x007C0000u
#define  AQ2_POLICY_MCAST_ACCEPT       0x00010000u
#define  AQ2_POLICY_BCAST_QUEUE_OR_TC  0x00008000u
#define  AQ2_POLICY_BCAST_RX_INDEX     0x00007C00u
#define  AQ2_POLICY_BCAST_ACCEPT       0x00000100u
#define  AQ2_POLICY_PROMISC_QUEUE_OR_TC 0x00000080u
#define  AQ2_POLICY_PROMISC_RX_INDEX   0x0000007Cu

#define AQ2_FW_OUT_TRANSACTION_ID_REG  0x13000
#define AQ2_FW_OUT_VERSION_BUNDLE_REG  0x13004
#define AQ2_FW_OUT_VERSION_IFACE_REG   0x13010
#define  AQ2_FW_OUT_VERSION_IFACE_VER  0x0000000Fu
#define AQ2_FW_OUT_LINK_STATUS_REG     0x13014
#define AQ2_FW_OUT_FILTER_CAPS_REG     0x13774
#define  AQ2_FILTER_CAPS3_RESOLVER_BASE 0x00FF0000u

// The action resolver table (ART): a tag/mask match per entry, and what
// to do with a frame whose filter tags match.
#define AQ2_RPF_ACT_ART_REQ_TAG_REG(i)    (0x14000 + (i) * 0x10)
#define AQ2_RPF_ACT_ART_REQ_MASK_REG(i)   (0x14004 + (i) * 0x10)
#define AQ2_RPF_ACT_ART_REQ_ACTION_REG(i) (0x14008 + (i) * 0x10)
#define AQ2_ART_ACTION(act, rss, idx, en) \
    (((uint32_t)(act) << 8) | ((rss) ? 0x80u : 0) | ((uint32_t)(idx) << 2) | ((en) ? 1u : 0))
#define AQ2_ART_ACTION_DROP            AQ2_ART_ACTION(0, 0, 0, 1)
#define AQ2_ART_ACTION_ASSIGN_TC(tc)   AQ2_ART_ACTION(1, 1, (tc), 1)
#define AQ2_RPF_TAG_PCP_MASK           0xE0000000u
#define AQ2_RPF_TAG_PCP_SHIFT          29
#define AQ2_RPF_TAG_UNTAG_MASK         0x00004000u
#define AQ2_RPF_TAG_VLAN_MASK          0x00003C00u
#define AQ2_RPF_TAG_ALLMC_MASK         0x00000040u
#define AQ2_RPF_TAG_UC_MASK            0x0000002Fu
#define AQ2_RPF_INDEX_L2_PROMISC_OFF   0
#define AQ2_RPF_INDEX_VLAN_PROMISC_OFF 1
#define AQ2_RPF_INDEX_PCP_TO_TC        56

#endif
