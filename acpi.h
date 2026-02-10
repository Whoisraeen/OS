#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>

// RSDP (Root System Description Pointer)
typedef struct {
    char signature[8];      // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;       // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;
} __attribute__((packed)) acpi_rsdp_t;

// RSDP extended (ACPI 2.0+)
typedef struct {
    acpi_rsdp_t rsdp;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp2_t;

// Generic SDT (System Description Table) header
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

// MADT (Multiple APIC Description Table) entry types
#define MADT_ENTRY_LAPIC            0
#define MADT_ENTRY_IOAPIC           1
#define MADT_ENTRY_ISO              2  // Interrupt Source Override
#define MADT_ENTRY_NMI              4
#define MADT_ENTRY_LAPIC_OVERRIDE   5

// MADT header
typedef struct {
    acpi_sdt_header_t header;
    uint32_t lapic_address;
    uint32_t flags;         // bit 0 = dual 8259 PICs installed
} __attribute__((packed)) acpi_madt_t;

// MADT entry: Local APIC
typedef struct {
    uint8_t type;           // 0
    uint8_t length;         // 8
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;         // bit 0 = enabled, bit 1 = online capable
} __attribute__((packed)) madt_lapic_t;

// MADT entry: I/O APIC
typedef struct {
    uint8_t type;           // 1
    uint8_t length;         // 12
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;      // Global System Interrupt base
} __attribute__((packed)) madt_ioapic_t;

// MADT entry: Interrupt Source Override
typedef struct {
    uint8_t type;           // 2
    uint8_t length;         // 10
    uint8_t bus;            // 0 = ISA
    uint8_t source;         // ISA IRQ number
    uint32_t gsi;           // Global System Interrupt
    uint16_t flags;         // polarity & trigger mode
} __attribute__((packed)) madt_iso_t;

// FADT (Fixed ACPI Description Table) — partial, just what we need
typedef struct {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t gpe1_base;
    uint8_t c_state_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_architecture_flags;
    uint8_t reserved1;
    uint32_t flags;
    // ... more fields follow in ACPI 2.0+ but we don't need them
} __attribute__((packed)) acpi_fadt_t;

// Parsed ACPI info (global)
typedef struct {
    // IOAPIC
    uint32_t ioapic_address;
    uint8_t ioapic_id;
    uint32_t ioapic_gsi_base;
    int has_ioapic;

    // IRQ overrides (ISA IRQ -> GSI mapping)
    #define MAX_IRQ_OVERRIDES 16
    struct {
        uint8_t source;     // ISA IRQ
        uint32_t gsi;       // Mapped GSI
        uint16_t flags;     // Polarity/trigger
    } irq_overrides[MAX_IRQ_OVERRIDES];
    int num_irq_overrides;

    // Shutdown info (from FADT)
    uint16_t pm1a_control_port;
    uint16_t slp_typa;     // SLP_TYP value for S5
    int can_shutdown;

    // Century register index (from FADT)
    uint8_t century_register;
} acpi_info_t;

// Initialize ACPI (parse tables)
void acpi_init(void);

// Get parsed ACPI info
acpi_info_t *acpi_get_info(void);

// ACPI shutdown (S5 state)
void acpi_shutdown(void);

// Reboot via keyboard controller
void acpi_reboot(void);

#endif
