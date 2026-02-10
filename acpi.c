#include "acpi.h"
#include "serial.h"
#include "vmm.h"
#include "io.h"
#include "limine/limine.h"
#include <stddef.h>

// Limine RSDP request
__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

// Global parsed ACPI info
static acpi_info_t acpi_info;

// Convert physical address to virtual (via HHDM)
static void *acpi_phys_to_virt(uint64_t phys) {
    return (void *)(phys + vmm_get_hhdm_offset());
}

// Validate checksum of an ACPI table
static int acpi_checksum(void *table, size_t length) {
    uint8_t sum = 0;
    uint8_t *data = (uint8_t *)table;
    for (size_t i = 0; i < length; i++) {
        sum += data[i];
    }
    return sum == 0;
}

// Compare 4-byte signature
static int acpi_sig_match(const char *sig, const char *match) {
    return sig[0] == match[0] && sig[1] == match[1] &&
           sig[2] == match[2] && sig[3] == match[3];
}

// Parse MADT (Multiple APIC Description Table)
static void acpi_parse_madt(acpi_madt_t *madt) {
    kprintf("[ACPI] MADT: LAPIC addr=0x%x, flags=0x%x\n",
            madt->lapic_address, madt->flags);

    uint8_t *ptr = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];
        if (length < 2) break;

        switch (type) {
        case MADT_ENTRY_LAPIC: {
            madt_lapic_t *lapic = (madt_lapic_t *)ptr;
            uint32_t flags = lapic->flags;
            int enabled = (flags & 1) || (flags & 2);
            kprintf("[ACPI]   LAPIC: proc=%d apic=%d %s\n",
                    lapic->processor_id, lapic->apic_id,
                    enabled ? "enabled" : "disabled");
            break;
        }
        case MADT_ENTRY_IOAPIC: {
            madt_ioapic_t *ioapic = (madt_ioapic_t *)ptr;
            kprintf("[ACPI]   IOAPIC: id=%d addr=0x%x gsi_base=%d\n",
                    ioapic->ioapic_id, ioapic->ioapic_address, ioapic->gsi_base);
            // Store the first IOAPIC
            if (!acpi_info.has_ioapic) {
                acpi_info.ioapic_address = ioapic->ioapic_address;
                acpi_info.ioapic_id = ioapic->ioapic_id;
                acpi_info.ioapic_gsi_base = ioapic->gsi_base;
                acpi_info.has_ioapic = 1;
            }
            break;
        }
        case MADT_ENTRY_ISO: {
            madt_iso_t *iso = (madt_iso_t *)ptr;
            kprintf("[ACPI]   ISO: bus=%d source=IRQ%d -> GSI%d flags=0x%x\n",
                    iso->bus, iso->source, iso->gsi, iso->flags);
            if (acpi_info.num_irq_overrides < MAX_IRQ_OVERRIDES) {
                int idx = acpi_info.num_irq_overrides++;
                acpi_info.irq_overrides[idx].source = iso->source;
                acpi_info.irq_overrides[idx].gsi = iso->gsi;
                acpi_info.irq_overrides[idx].flags = iso->flags;
            }
            break;
        }
        case MADT_ENTRY_NMI:
            kprintf("[ACPI]   NMI source\n");
            break;
        case MADT_ENTRY_LAPIC_OVERRIDE:
            kprintf("[ACPI]   LAPIC address override\n");
            break;
        default:
            kprintf("[ACPI]   Unknown MADT entry type %d\n", type);
            break;
        }

        ptr += length;
    }
}

// Try to extract S5 sleep type from DSDT
// The \_S5 object is typically encoded as: 08 5F 53 35 5F 12 <pkglen> <count> 0A <SLP_TYPa> ...
// This is a simplified parser that searches for the \_S5_ name and extracts the value
static void acpi_parse_s5(acpi_sdt_header_t *dsdt) {
    uint8_t *ptr = (uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
    uint8_t *end = (uint8_t *)dsdt + dsdt->length;

    // Search for "_S5_" in the DSDT AML bytecode
    while (ptr < end - 4) {
        if (ptr[0] == '_' && ptr[1] == 'S' && ptr[2] == '5' && ptr[3] == '_') {
            // Found \_S5_ — skip the name, parse the package
            ptr += 4;
            // Check for package op (0x12)
            if (ptr >= end) break;

            // There may be a NameOp (0x08) before, but we already skipped the name
            if (*ptr == 0x12) {
                ptr++; // skip package op
                // Parse PkgLength (simplified: assume single-byte length)
                uint8_t pkg_len = *ptr;
                (void)pkg_len;
                ptr++;
                // NumElements
                ptr++;
                // First element is SLP_TYPa
                if (*ptr == 0x0A) {
                    // BytePrefix
                    ptr++;
                    acpi_info.slp_typa = *ptr;
                    acpi_info.can_shutdown = 1;
                    kprintf("[ACPI] S5 sleep type: 0x%x\n", acpi_info.slp_typa);
                } else if (*ptr <= 0x0F) {
                    // Could be a direct byte value (0-15 don't need BytePrefix in some implementations)
                    acpi_info.slp_typa = *ptr;
                    acpi_info.can_shutdown = 1;
                    kprintf("[ACPI] S5 sleep type: 0x%x (direct)\n", acpi_info.slp_typa);
                }
                return;
            }
            continue;
        }
        ptr++;
    }
    kprintf("[ACPI] Warning: _S5 not found in DSDT\n");
}

// Parse FADT (Fixed ACPI Description Table)
static void acpi_parse_fadt(acpi_fadt_t *fadt) {
    kprintf("[ACPI] FADT: PM1a_ctrl=0x%x, SCI_INT=%d\n",
            fadt->pm1a_control_block, fadt->sci_interrupt);

    acpi_info.pm1a_control_port = (uint16_t)fadt->pm1a_control_block;
    acpi_info.century_register = fadt->century;

    kprintf("[ACPI] FADT: century_reg=%d, boot_flags=0x%x\n",
            fadt->century, fadt->boot_architecture_flags);

    // Parse DSDT for S5 sleep type
    if (fadt->dsdt != 0) {
        acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)acpi_phys_to_virt(fadt->dsdt);
        if (acpi_sig_match(dsdt->signature, "DSDT")) {
            kprintf("[ACPI] DSDT at 0x%x (len=%d)\n", fadt->dsdt, dsdt->length);
            acpi_parse_s5(dsdt);
        }
    }
}

// Walk RSDT (32-bit table pointers)
static void acpi_walk_rsdt(uint32_t rsdt_phys) {
    acpi_sdt_header_t *rsdt = (acpi_sdt_header_t *)acpi_phys_to_virt(rsdt_phys);

    if (!acpi_sig_match(rsdt->signature, "RSDT")) {
        kprintf("[ACPI] Invalid RSDT signature\n");
        return;
    }
    if (!acpi_checksum(rsdt, rsdt->length)) {
        kprintf("[ACPI] RSDT checksum failed\n");
        return;
    }

    uint32_t entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    uint32_t *table_ptrs = (uint32_t *)((uint8_t *)rsdt + sizeof(acpi_sdt_header_t));

    kprintf("[ACPI] RSDT has %d entries\n", entries);

    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_phys_to_virt(table_ptrs[i]);

        char sig[5] = { header->signature[0], header->signature[1],
                        header->signature[2], header->signature[3], 0 };
        kprintf("[ACPI]   Table: %s at 0x%x (len=%d)\n", sig, table_ptrs[i], header->length);

        if (acpi_sig_match(header->signature, "APIC")) {
            acpi_parse_madt((acpi_madt_t *)header);
        } else if (acpi_sig_match(header->signature, "FACP")) {
            acpi_parse_fadt((acpi_fadt_t *)header);
        }
    }
}

// Walk XSDT (64-bit table pointers)
static void acpi_walk_xsdt(uint64_t xsdt_phys) {
    acpi_sdt_header_t *xsdt = (acpi_sdt_header_t *)acpi_phys_to_virt(xsdt_phys);

    if (!acpi_sig_match(xsdt->signature, "XSDT")) {
        kprintf("[ACPI] Invalid XSDT signature\n");
        return;
    }
    if (!acpi_checksum(xsdt, xsdt->length)) {
        kprintf("[ACPI] XSDT checksum failed\n");
        return;
    }

    uint32_t entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
    uint64_t *table_ptrs = (uint64_t *)((uint8_t *)xsdt + sizeof(acpi_sdt_header_t));

    kprintf("[ACPI] XSDT has %d entries\n", entries);

    for (uint32_t i = 0; i < entries; i++) {
        acpi_sdt_header_t *header = (acpi_sdt_header_t *)acpi_phys_to_virt(table_ptrs[i]);

        char sig[5] = { header->signature[0], header->signature[1],
                        header->signature[2], header->signature[3], 0 };
        kprintf("[ACPI]   Table: %s at 0x%lx (len=%d)\n", sig, table_ptrs[i], header->length);

        if (acpi_sig_match(header->signature, "APIC")) {
            acpi_parse_madt((acpi_madt_t *)header);
        } else if (acpi_sig_match(header->signature, "FACP")) {
            acpi_parse_fadt((acpi_fadt_t *)header);
        }
    }
}

void acpi_init(void) {
    // Zero out global state
    for (size_t i = 0; i < sizeof(acpi_info); i++)
        ((uint8_t *)&acpi_info)[i] = 0;

    // Get RSDP from Limine
    if (rsdp_request.response == NULL || rsdp_request.response->address == 0) {
        kprintf("[ACPI] No RSDP provided by bootloader\n");
        return;
    }

    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)acpi_phys_to_virt((uintptr_t)rsdp_request.response->address);

    // Validate RSDP signature
    const char *expected = "RSD PTR ";
    int valid = 1;
    for (int i = 0; i < 8; i++) {
        if (rsdp->signature[i] != expected[i]) { valid = 0; break; }
    }
    if (!valid) {
        kprintf("[ACPI] Invalid RSDP signature\n");
        return;
    }

    // Validate RSDP checksum (first 20 bytes)
    if (!acpi_checksum(rsdp, 20)) {
        kprintf("[ACPI] RSDP checksum failed\n");
        return;
    }

    kprintf("[ACPI] RSDP found: revision=%d, OEM='%.6s'\n",
            rsdp->revision, rsdp->oem_id);

    if (rsdp->revision >= 2) {
        // ACPI 2.0+ — use XSDT
        acpi_rsdp2_t *rsdp2 = (acpi_rsdp2_t *)rsdp;
        if (rsdp2->xsdt_address != 0) {
            kprintf("[ACPI] Using XSDT at 0x%lx\n", rsdp2->xsdt_address);
            acpi_walk_xsdt(rsdp2->xsdt_address);
            return;
        }
    }

    // ACPI 1.0 or no XSDT — use RSDT
    if (rsdp->rsdt_address != 0) {
        kprintf("[ACPI] Using RSDT at 0x%x\n", rsdp->rsdt_address);
        acpi_walk_rsdt(rsdp->rsdt_address);
    } else {
        kprintf("[ACPI] No RSDT or XSDT found\n");
    }
}

acpi_info_t *acpi_get_info(void) {
    return &acpi_info;
}

void acpi_shutdown(void) {
    if (!acpi_info.can_shutdown) {
        kprintf("[ACPI] Shutdown not supported (no S5 info)\n");
        return;
    }

    kprintf("[ACPI] Shutting down (PM1a=0x%x, SLP_TYP=0x%x)...\n",
            acpi_info.pm1a_control_port, acpi_info.slp_typa);

    // Write SLP_TYPa | SLP_EN (bit 13) to PM1a_CNT
    uint16_t val = (acpi_info.slp_typa << 10) | (1 << 13);
    outw(acpi_info.pm1a_control_port, val);

    // If we get here, shutdown failed
    kprintf("[ACPI] Shutdown failed!\n");
    for (;;) __asm__("hlt");
}

void acpi_reboot(void) {
    kprintf("[ACPI] Rebooting via keyboard controller...\n");

    // Triple-fault method as backup: disable interrupts, load null IDT, trigger interrupt
    // But first try the keyboard controller method (port 0x64)
    __asm__ volatile("cli");

    // Pulse reset line via keyboard controller
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02); // Wait for input buffer empty
    outb(0x64, 0xFE);     // Send reset command

    // If keyboard controller reset didn't work, triple fault
    __asm__ volatile("lidt %0" : : "m"((struct { uint16_t l; uint64_t b; }){0, 0}));
    __asm__ volatile("int $3");

    for (;;) __asm__("hlt");
}
