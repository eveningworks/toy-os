#ifndef I8042_H
#define I8042_H

// The PS/2 keyboard and mouse share one controller and one data port
// (0x60). Which device a waiting byte came from is only knowable from
// the status register (0x64) -- bit 5 set means it's from the auxiliary
// device, i.e. the mouse.
//
// So neither IRQ handler may read 0x60 on its own: if IRQ12 fires while
// a keyboard byte is waiting and reads blindly, it eats the keystroke
// AND feeds a bogus byte into the mouse packet decoder. Both IRQ1 and
// IRQ12 therefore call i8042_poll(), which drains the buffer and routes
// each byte to the right device by its AUX bit.
void i8042_poll(void);

// Probes for the controller and, if one answers, declares the PS/2
// keyboard and mouse to the input core. Called from kernel_main() once,
// after the IDT is up. See kernel/drivers/input/input.c.
void i8042_register_sources(void);

// 1 if a controller answered the probe. Without one nothing is
// registered, polled or written -- mouse_init() checks it too.
int i8042_present(void);

#endif
