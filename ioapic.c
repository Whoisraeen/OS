#include "ioapic.h"
#include "acpi.h"
#include "vmm.h"
#include "serial.h"
#include "pic.h"

// IOAPIC MMIO base (virtual address)
static volatile uint32_t *ioapic_base = NULL;
static uint32_t ioapic_max_entries = 0;

static void ioapic_write(uint8_t reg, uint32_t value) {
    ioapic_base[0] = reg;                          // IOREGSEL
    ioapic_base[4] = value;                         // IOWIN (offset 0x10 / 4 = 4)
}

static uint32_t ioapic_read(uint8_t reg) {
    ioapic_base[0] = reg;                           // IOREGSEL
    return ioapic_base[4];                          // IOWIN
}

// Write a 64-bit redirection entry (split into two 32-bit writes)
static void ioapic_write_redir(uint8_t index, uint64_t value) {
    ioapic_write(IOAPIC_REG_REDTBL + 2 * index, (uint32_t)(value & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REG_REDTBL + 2 * index + 1, (uint32_t)(value >> 32));
}

static uint64_t ioapic_read_redir(uint8_t index) {
    uint32_t lo = ioapic_read(IOAPIC_REG_REDTBL + 2 * index);
    uint32_t hi = ioapic_read(IOAPIC_REG_REDTBL + 2 * index + 1);
    return ((uint64_t)hi << 32) | lo;
}

void ioapic_init(void) {
    acpi_info_t *info = acpi_get_info();

    if (!info->has_ioapic) {
        kprintf("[IOAPIC] No IOAPIC found in ACPI tables, keeping PIC\n");
        return;
    }

    // Map IOAPIC MMIO registers (physical -> virtual via HHDM)
    uint64_t phys = info->ioapic_address;
    uint64_t virt = phys + vmm_get_hhdm_offset();

    // Ensure the page is mapped with no-cache
    vmm_map_page(virt, phys, 0x13); // Present | Writable | NoCache

    ioapic_base = (volatile uint32_t *)virt;

    // Read IOAPIC version and max redirection entries
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_entries = ((ver >> 16) & 0xFF) + 1;

    kprintf("[IOAPIC] ID=%d, Version=0x%x, Max IRQs=%d\n",
            info->ioapic_id, ver & 0xFF, ioapic_max_entries);

    // Mask all entries initially
    for (uint32_t i = 0; i < ioapic_max_entries; i++) {
        ioapic_write_redir(i, IOAPIC_MASKED);
    }

    // Disable the legacy 8259 PIC (mask all IRQs)
    pic_disable();

    kprintf("[IOAPIC] PIC disabled, IOAPIC active\n");

    // Route standard ISA IRQs via IOAPIC
    // IRQ0 = Timer -> Vector 32, to BSP (LAPIC 0)
    // IRQ1 = Keyboard -> Vector 33
    // IRQ12 = Mouse -> Vector 44
    // Apply MADT Interrupt Source Overrides

    // Timer (IRQ0)
    uint32_t timer_gsi = ioapic_get_gsi_for_irq(0);
    uint16_t timer_flags = ioapic_get_flags_for_irq(0);
    uint32_t timer_redir_flags = IOAPIC_FIXED | IOAPIC_PHYSICAL | IOAPIC_EDGE | IOAPIC_ACTIVE_HIGH;
    // Apply override flags if present
    if (timer_flags) {
        timer_redir_flags = IOAPIC_FIXED | IOAPIC_PHYSICAL;
        // Polarity: bits 0-1 (00=conform, 01=active high, 11=active low)
        if ((timer_flags & 0x03) == 0x03)
            timer_redir_flags |= IOAPIC_ACTIVE_LOW;
        // Trigger: bits 2-3 (00=conform, 01=edge, 11=level)
        if ((timer_flags & 0x0C) == 0x0C)
            timer_redir_flags |= IOAPIC_LEVEL;
    }
    ioapic_route_irq(timer_gsi, 32, 0, timer_redir_flags);
    kprintf("[IOAPIC] Timer: IRQ0 -> GSI%d -> Vector 32\n", timer_gsi);

    // Keyboard (IRQ1)
    uint32_t kbd_gsi = ioapic_get_gsi_for_irq(1);
    ioapic_route_irq(kbd_gsi, 33, 0, IOAPIC_FIXED | IOAPIC_PHYSICAL | IOAPIC_EDGE | IOAPIC_ACTIVE_HIGH);
    kprintf("[IOAPIC] Keyboard: IRQ1 -> GSI%d -> Vector 33\n", kbd_gsi);

    // Mouse (IRQ12)
    uint32_t mouse_gsi = ioapic_get_gsi_for_irq(12);
    uint16_t mouse_flags = ioapic_get_flags_for_irq(12);
    uint32_t mouse_redir_flags = IOAPIC_FIXED | IOAPIC_PHYSICAL | IOAPIC_EDGE | IOAPIC_ACTIVE_HIGH;
    if (mouse_flags) {
        mouse_redir_flags = IOAPIC_FIXED | IOAPIC_PHYSICAL;
        if ((mouse_flags & 0x03) == 0x03)
            mouse_redir_flags |= IOAPIC_ACTIVE_LOW;
        if ((mouse_flags & 0x0C) == 0x0C)
            mouse_redir_flags |= IOAPIC_LEVEL;
    }
    ioapic_route_irq(mouse_gsi, 44, 0, mouse_redir_flags);
    kprintf("[IOAPIC] Mouse: IRQ12 -> GSI%d -> Vector 44\n", mouse_gsi);
}

void ioapic_route_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic, uint32_t flags) {
    if (!ioapic_base || gsi >= ioapic_max_entries) return;

    uint64_t entry = (uint64_t)vector | flags;
    entry |= ((uint64_t)dest_lapic << 56); // Destination field in bits 56-63
    ioapic_write_redir(gsi, entry);
}

void ioapic_mask(uint8_t gsi) {
    if (!ioapic_base || gsi >= ioapic_max_entries) return;
    uint64_t entry = ioapic_read_redir(gsi);
    entry |= IOAPIC_MASKED;
    ioapic_write_redir(gsi, entry);
}

void ioapic_unmask(uint8_t gsi) {
    if (!ioapic_base || gsi >= ioapic_max_entries) return;
    uint64_t entry = ioapic_read_redir(gsi);
    entry &= ~((uint64_t)IOAPIC_MASKED);
    ioapic_write_redir(gsi, entry);
}

uint32_t ioapic_get_gsi_for_irq(uint8_t irq) {
    acpi_info_t *info = acpi_get_info();
    for (int i = 0; i < info->num_irq_overrides; i++) {
        if (info->irq_overrides[i].source == irq) {
            return info->irq_overrides[i].gsi;
        }
    }
    // No override — identity mapping (ISA IRQ N = GSI N)
    return irq;
}

uint16_t ioapic_get_flags_for_irq(uint8_t irq) {
    acpi_info_t *info = acpi_get_info();
    for (int i = 0; i < info->num_irq_overrides; i++) {
        if (info->irq_overrides[i].source == irq) {
            return info->irq_overrides[i].flags;
        }
    }
    return 0; // No override
}
