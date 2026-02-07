#include "pic.h"
#include "io.h"

#define PIC1		0x20		/* IO base address for master PIC */
#define PIC2		0xA0		/* IO base address for slave PIC */
#define PIC1_COMMAND	PIC1
#define PIC1_DATA	(PIC1+1)
#define PIC2_COMMAND	PIC2
#define PIC2_DATA	(PIC2+1)

#define PIC_EOI		0x20		/* End-of-interrupt command code */

void pic_send_eoi(unsigned char irq) {
    if(irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    
    outb(PIC1_COMMAND, PIC_EOI);
}

/*
arguments:
	offset1 - vector offset for master PIC
		vectors on the master become offset1..offset1+7
	offset2 - same for slave PIC: offset2..offset2+7
*/
void pic_remap(int offset1, int offset2) {
	outb(PIC1_COMMAND, 0x11);  // starts the initialization sequence (in cascade mode)
	io_wait();
	outb(PIC2_COMMAND, 0x11);
	io_wait();
	outb(PIC1_DATA, offset1);                 // ICW2: Master PIC vector offset
	io_wait();
	outb(PIC2_DATA, offset2);                 // ICW2: Slave PIC vector offset
	io_wait();
	outb(PIC1_DATA, 4);                       // ICW3: tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
	io_wait();
	outb(PIC2_DATA, 2);                       // ICW3: tell Slave PIC its cascade identity (0000 0010)
	io_wait();

	outb(PIC1_DATA, 0x01);                    // ICW4: 8086 mode
	io_wait();
	outb(PIC2_DATA, 0x01);
	io_wait();

	// Set masks: enable IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade to slave)
	// Master mask: 0xF8 = 11111000 = disable IRQ3-7, enable IRQ0,1,2
	outb(PIC1_DATA, 0xF8);
	// Slave mask: 0xEF = 11101111 = disable all except IRQ12 (mouse, bit 4)
	outb(PIC2_DATA, 0xEF);
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
}
