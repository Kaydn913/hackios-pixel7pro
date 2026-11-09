/*
 * The touchscreen's Synaptics chip (S3908, TouchComm v1) over bit-banged SPI (touch.c). With the
 * MMU off, from EL1 or EL2 alike.
 */
void touch_pins(void);
int touch_reset(void);				/* 1: it answered (its interrupt line went low) */
int touch_irq(void);				/* 1: it has a message */
/* the next message: its code, its payload in buf (up to max bytes); -1 if there was none */
int tcm_read(u8 *code, u8 *buf, u32 max);
void tcm_command(u8 cmd, const u8 *payload, u32 n);

#define TCM_IDENTIFY		0x10
#define TCM_TOUCH		0x11
#define TCM_DISABLE_REPORT	0x06
