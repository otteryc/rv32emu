/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#if !RV32_HAS(SYSTEM)
#error "Do not manage to build this file unless you enable system support."
#endif

#include <assert.h>

#include "devices/plic.h"
#include "devices/uart.h"
#include "devices/virtio.h"
#include "riscv_private.h"

#define R 1
#define W 0

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
void emu_update_uart_interrupts(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    u8250_update_interrupts(attr->uart);
    if (attr->uart->pending_intrs)
        attr->plic->active |= IRQ_UART_BIT;
    else
        attr->plic->active &= ~IRQ_UART_BIT;
    plic_update_interrupts(attr->plic);
}

static void emu_update_vblk_interrupts(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    if (attr->vblk->interrupt_status)
        attr->plic->active |= IRQ_VBLK_BIT;
    else
        attr->plic->active &= ~IRQ_VBLK_BIT;
    plic_update_interrupts(attr->plic);
}
/*
 * Linux kernel might create signal frame when returning from trap
 * handling, which modifies the SEPC CSR. Thus, the fault instruction
 * cannot always redo. For example, invalid memory access causes SIGSEGV.
 */
extern bool need_handle_signal;
#define CHECK_PENDING_SIGNAL(rv, signal_flag)              \
    do {                                                   \
        signal_flag = (rv->csr_sepc != rv->last_csr_sepc); \
    } while (0)

#define MMIO_R 1
#define MMIO_W 0

enum SUPPORTED_MMIO {
    MMIO_PLIC,
    MMIO_UART,
    MMIO_VIRTIOBLK,
};

/* clang-format off */
#define MMIO_OP(io, rw)                                                             \
    switch(io){                                                                     \
        case MMIO_PLIC:                                                             \
            IIF(rw)( /* read */                                                     \
                mmio_read_val = plic_read(PRIV(rv)->plic, addr & 0x3FFFFFF); \
                plic_update_interrupts(PRIV(rv)->plic);                             \
                return mmio_read_val;                                               \
                ,    /* write */                                                    \
                plic_write(PRIV(rv)->plic, addr & 0x3FFFFFF, val);           \
                plic_update_interrupts(PRIV(rv)->plic);                             \
                return;                                                             \
            )                                                                       \
            break;                                                                  \
        case MMIO_UART:                                                             \
            IIF(rw)( /* read */                                                     \
                mmio_read_val = u8250_read(PRIV(rv)->uart, addr & 0xFFFFF);         \
                emu_update_uart_interrupts(rv);                                     \
                return mmio_read_val;                                               \
                ,    /* write */                                                    \
                u8250_write(PRIV(rv)->uart, addr & 0xFFFFF, val);                   \
                emu_update_uart_interrupts(rv);                                     \
                return;                                                             \
            )                                                                       \
            break;                                                                  \
        case MMIO_VIRTIOBLK:                                                        \
            IIF(rw)( /* read */                                                     \
                mmio_read_val = virtio_blk_read(PRIV(rv)->vblk, addr & 0xFFFFF);    \
                emu_update_vblk_interrupts(rv);                                     \
                return mmio_read_val;                                               \
                ,    /* write */                                                    \
                virtio_blk_write(PRIV(rv)->vblk, addr & 0xFFFFF, val);              \
                emu_update_vblk_interrupts(rv);                                     \
                return;                                                             \
            )                                                                       \
            break;                                                                  \
        default:                                                                    \
            rv_log_error("Unknown MMIO type %d", io);                          \
            break;                                                                  \
    }
/* clang-format on */

#define MMIO_READ()                                         \
    do {                                                    \
        uint32_t mmio_read_val;                             \
        if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */ \
            /* 256 regions of 1MiB */                       \
            switch ((addr >> 20) & MASK(8)) {               \
            case 0x0:                                       \
            case 0x2: /* PLIC (0 - 0x3F) */                 \
                MMIO_OP(MMIO_PLIC, MMIO_R);                 \
                break;                                      \
            case 0x40: /* UART */                           \
                MMIO_OP(MMIO_UART, MMIO_R);                 \
                break;                                      \
            case 0x42: /* Virtio-blk */                     \
                MMIO_OP(MMIO_VIRTIOBLK, MMIO_R);            \
                break;                                      \
            default:                                        \
                __UNREACHABLE;                              \
                break;                                      \
            }                                               \
        }                                                   \
    } while (0)

#define MMIO_WRITE()                                        \
    do {                                                    \
        if ((addr >> 28) == 0xF) { /* MMIO at 0xF_______ */ \
            /* 256 regions of 1MiB */                       \
            switch ((addr >> 20) & MASK(8)) {               \
            case 0x0:                                       \
            case 0x2: /* PLIC (0 - 0x3F) */                 \
                MMIO_OP(MMIO_PLIC, MMIO_W);                 \
                break;                                      \
            case 0x40: /* UART */                           \
                MMIO_OP(MMIO_UART, MMIO_W);                 \
                break;                                      \
            case 0x42: /* Virtio-blk */                     \
                MMIO_OP(MMIO_VIRTIOBLK, MMIO_W);            \
                break;                                      \
            default:                                        \
                __UNREACHABLE;                              \
                break;                                      \
            }                                               \
        }                                                   \
    } while (0)
#endif

static bool ppn_is_valid(riscv_t *rv, uint32_t ppn)
{
    vm_attr_t *attr = PRIV(rv);
    const uint32_t nr_pg_max = attr->mem_size / RV_PG_SIZE;
    return ppn < nr_pg_max;
}

#define PAGE_TABLE(ppn)                                               \
    ppn_is_valid(rv, ppn)                                             \
        ? (uint32_t *) (attr->mem->mem_base + (ppn << (RV_PG_SHIFT))) \
        : NULL

/* Walk through page tables and get the corresponding PTE by virtual address if
 * exists
 * @rv: RISC-V emulator
 * @vaddr: virtual address
 * @level: the level of which the PTE is located
 * @return: NULL if a not found or fault else the corresponding PTE
 */
pte_t *mmu_walk(riscv_t *rv, const uint32_t vaddr, uint32_t *level)
{
    vm_attr_t *attr = PRIV(rv);
    uint32_t ppn = rv->csr_satp & MASK(22);

    /* root page table */
    uint32_t *page_table = PAGE_TABLE(ppn);
    if (!page_table)
        return NULL;

    for (int i = 1; i >= 0; i--) {
        *level = 2 - i;
        uint32_t vpn =
            (vaddr >> RV_PG_SHIFT >> (i * (RV_PG_SHIFT - 2))) & MASK(10);
        pte_t *pte = page_table + vpn;

        uint8_t XWRV_bit = (*pte & MASK(4));
        switch (XWRV_bit) {
        case NEXT_PG_TBL: /* next level of the page table */
            ppn = (*pte >> (RV_PG_SHIFT - 2));
            page_table = PAGE_TABLE(ppn);
            if (!page_table)
                return NULL;
            break;
        case RO_PAGE:
        case RW_PAGE:
        case EO_PAGE:
        case RX_PAGE:
        case RWX_PAGE:
            ppn = (*pte >> (RV_PG_SHIFT - 2));
            if (*level == 1 &&
                unlikely(ppn & MASK(10))) /* misaligned superpage */
                return NULL;
            return pte; /* leaf PTE */
        case RESRV_PAGE1:
        case RESRV_PAGE2:
        default:
            return NULL;
        }
    }

    return NULL;
}

/* Verify the PTE and generate corresponding faults if needed
 * @op: the operation
 * @rv: RISC-V emulator
 * @pte: to be verified pte
 * @vaddr: the corresponding virtual address to cause fault
 * @return: false if a any fault is generated which caused by violating the
 * access permission else true
 */
/* FIXME: handle access fault, addr out of range check */
#define MMU_FAULT_CHECK(op, rv, pte, vaddr, access_bits) \
    mmu_##op##_fault_check(rv, pte, vaddr, access_bits)
#define MMU_FAULT_CHECK_IMPL(op, pgfault)                                     \
    static bool mmu_##op##_fault_check(riscv_t *rv, pte_t *pte,               \
                                       uint32_t vaddr, uint32_t access_bits)  \
    {                                                                         \
        uint32_t scause;                                                      \
        uint32_t stval = vaddr;                                               \
        switch (access_bits) {                                                \
        case PTE_R:                                                           \
            scause = PAGEFAULT_LOAD;                                          \
            break;                                                            \
        case PTE_W:                                                           \
            scause = PAGEFAULT_STORE;                                         \
            break;                                                            \
        case PTE_X:                                                           \
            scause = PAGEFAULT_INSN;                                          \
            break;                                                            \
        default:                                                              \
            __UNREACHABLE;                                                    \
            break;                                                            \
        }                                                                     \
        if (pte && (!(*pte & PTE_V))) {                                       \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        if (!(pte && (*pte & access_bits))) {                                 \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /*                                                                    \
         * (1) When MXR=0, only loads from pages marked readable (R=1) will   \
         * succeed.                                                           \
         *                                                                    \
         * (2) When MXR=1, loads from pages marked either readable or         \
         * executable (R=1 or X=1) will succeed.                              \
         */                                                                   \
        if (pte && ((!(SSTATUS_MXR & rv->csr_sstatus) && !(*pte & PTE_R) &&   \
                     (access_bits == PTE_R)) ||                               \
                    ((SSTATUS_MXR & rv->csr_sstatus) &&                       \
                     !((*pte & PTE_R) | (*pte & PTE_X)) &&                    \
                     (access_bits == PTE_R)))) {                              \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /*                                                                    \
         * When SUM=0, S-mode memory accesses to pages that are accessible by \
         * U-mode will fault.                                                 \
         */                                                                   \
        if (pte && rv->priv_mode == RV_PRIV_S_MODE &&                         \
            !(SSTATUS_SUM & rv->csr_sstatus) && (*pte & PTE_U)) {             \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /* PTE not found, map it in handler */                                \
        if (!pte) {                                                           \
            SET_CAUSE_AND_TVAL_THEN_TRAP(rv, scause, stval);                  \
            return false;                                                     \
        }                                                                     \
        /* valid PTE */                                                       \
        return true;                                                          \
    }

MMU_FAULT_CHECK_IMPL(ifetch, pagefault_insn)
MMU_FAULT_CHECK_IMPL(read, pagefault_load)
MMU_FAULT_CHECK_IMPL(write, pagefault_store)

uint32_t ppn;
uint32_t offset;
#define get_ppn_and_offset()                                   \
    do {                                                       \
        assert(pte);                                           \
        ppn = *pte >> (RV_PG_SHIFT - 2) << RV_PG_SHIFT;        \
        offset = level == 1 ? vaddr & MASK((RV_PG_SHIFT + 10)) \
                            : vaddr & MASK(RV_PG_SHIFT);       \
    } while (0)

/* The IO handler that operates when the Memory Management Unit (MMU)
 * is enabled during system emulation is responsible for managing
 * input/output operations. These callbacks are designed to implement
 * the riscv_io_t interface, ensuring compatibility and consistency to
 * the structure required by the interface. As a result, the riscv_io_t
 * interface can be reused.
 *
 * The IO handlers include:
 * - mmu_ifetch
 * - mmu_read_w
 * - mmu_read_s
 * - mmu_read_b
 * - mmu_write_w
 * - mmu_write_s
 * - mmu_write_b
 */
extern bool need_retranslate;
static uint32_t mmu_ifetch(riscv_t *rv, const uint32_t vaddr)
{
    /*
     * Do not call rv->io.mem_translate() because the basic block might be
     * retranslated and the corresponding PTE is NULL, get_ppn_and_offset()
     * cannot work on a NULL PTE.
     */

    if (!rv->csr_satp)
        return memory_ifetch(vaddr);

    uint32_t level;
    pte_t *pte = mmu_walk(rv, vaddr, &level);
    bool ok = MMU_FAULT_CHECK(ifetch, rv, pte, vaddr, PTE_X);
    if (unlikely(!ok)) {
#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
        CHECK_PENDING_SIGNAL(rv, need_handle_signal);
        if (need_handle_signal)
            return 0;
#endif
        pte = mmu_walk(rv, vaddr, &level);
    }

    if (need_retranslate)
        return 0;

    get_ppn_and_offset();
    return memory_ifetch(ppn | offset);
}

static uint32_t mmu_read_w(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return 0;
#endif

    if (addr == vaddr || addr < PRIV(rv)->mem->mem_size)
        return memory_read_w(addr);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    MMIO_READ();
#endif

    __UNREACHABLE;
}

static uint16_t mmu_read_s(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return 0;
#endif

    return memory_read_s(addr);
}

static uint8_t mmu_read_b(riscv_t *rv, const uint32_t vaddr)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, R);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return 0;
#endif

    if (addr == vaddr || addr < PRIV(rv)->mem->mem_size)
        return memory_read_b(addr);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    MMIO_READ();
#endif

    __UNREACHABLE;
}

static void mmu_write_w(riscv_t *rv, const uint32_t vaddr, const uint32_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return;
#endif

    if (addr == vaddr || addr < PRIV(rv)->mem->mem_size) {
        memory_write_w(addr, (uint8_t *) &val);
        return;
    }

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    MMIO_WRITE();
#endif
}

static void mmu_write_s(riscv_t *rv, const uint32_t vaddr, const uint16_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return;
#endif

    if (addr == vaddr)
        return memory_write_s(addr, (uint8_t *) &val);

    memory_write_s(addr, (uint8_t *) &val);
}

static void mmu_write_b(riscv_t *rv, const uint32_t vaddr, const uint8_t val)
{
    uint32_t addr = rv->io.mem_translate(rv, vaddr, W);

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    if (need_handle_signal)
        return;
#endif

    if (addr == vaddr || addr < PRIV(rv)->mem->mem_size) {
        memory_write_b(addr, (uint8_t *) &val);
        return;
    }

#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
    MMIO_WRITE();
#endif
}

/*
 * TODO: dTLB can be introduced here to
 * cache the gVA to gPA tranlation.
 */
static uint32_t mmu_translate(riscv_t *rv, uint32_t vaddr, bool rw)
{
    if (!rv->csr_satp)
        return vaddr;

    uint32_t level;
    pte_t *pte = mmu_walk(rv, vaddr, &level);
    bool ok = rw ? MMU_FAULT_CHECK(read, rv, pte, vaddr, PTE_R)
                 : MMU_FAULT_CHECK(write, rv, pte, vaddr, PTE_W);
    if (unlikely(!ok)) {
#if RV32_HAS(SYSTEM) && !RV32_HAS(ELF_LOADER)
        CHECK_PENDING_SIGNAL(rv, need_handle_signal);
        if (need_handle_signal)
            return 0;
#endif
        pte = mmu_walk(rv, vaddr, &level);
    }

    get_ppn_and_offset();
    return ppn | offset;
}

riscv_io_t mmu_io = {
    /* memory read interface */
    .mem_ifetch = mmu_ifetch,
    .mem_read_w = mmu_read_w,
    .mem_read_s = mmu_read_s,
    .mem_read_b = mmu_read_b,

    /* memory write interface */
    .mem_write_w = mmu_write_w,
    .mem_write_s = mmu_write_s,
    .mem_write_b = mmu_write_b,

    /* VA2PA handler */
    .mem_translate = mmu_translate,

    /* system services or essential routines */
    .on_ecall = ecall_handler,
    .on_ebreak = ebreak_handler,
    .on_memcpy = memcpy_handler,
    .on_memset = memset_handler,
    .on_trap = trap_handler,
};
