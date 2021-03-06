/******************************************************************************
 * Copyright 2015 Espressif Systems
 *
 * Description: A stub to make the ESP8266 debuggable by GDB over the serial
 * port.
 *
 * License: ESPRESSIF MIT License
 *******************************************************************************/

#include "gdbstub.h"
#include <stddef.h>
#include "ets_sys.h"
#include "eagle_soc.h"
#include "c_types.h"
#include "gpio.h"
#include "xtensa/corebits.h"
#include "uart_register.h"

#include "gdbstub-entry.h"
#include "gdbstub-cfg.h"


//From xtruntime-frames.h
struct XTensa_exception_frame_s {
	uint32_t pc;
	uint32_t ps;
	uint32_t sar;
	uint32_t vpri;
	uint32_t a[16]; //a0..a15
//These are added manually by the exception code; the HAL doesn't set these on an exception.
	uint32_t litbase;
	uint32_t sr176;
	uint32_t sr208;
	 //'reason' is abused for both the debug and the exception vector: if bit 7 is set,
	//this contains an exception reason, otherwise it contains a debug vector bitmap.
	uint32_t reason;
};

#if GDBSTUB_FREERTOS

struct XTensa_rtos_int_frame_s {
	uint32_t exitPtr;
	uint32_t pc;
	uint32_t ps;
	uint32_t a[16];
	uint32_t sar;
};

/*
Definitions for FreeRTOS. This redefines some os_* functions to use their non-os* counterparts. It
also sets up some function pointers for ROM functions that aren't in the FreeRTOS ld files.
*/
#include <string.h>
#include <stdio.h>
void os_isr_attach(int inum, void *fn);
void os_install_putc1(void (*p)(char c));
#define os_printf(...) printf(__VA_ARGS__)
#define os_strncmp(...) strncmp(__VA_ARGS__)
typedef void wdtfntype();
static wdtfntype *ets_wdt_disable=(wdtfntype *)0x400030f0;
static wdtfntype *ets_wdt_enable=(wdtfntype *)0x40002fa0;

#else
/*
OS-less SDK defines. Defines some headers for things that aren't in the include files, plus
the xthal stack frame struct.
*/
#include "osapi.h"
#include "user_interface.h"

void _xtos_set_exception_handler(int cause, void (exhandler)(struct XTensa_exception_frame_s *frame));

#endif

#define EXCEPTION_GDB_SP_OFFSET 0x100


//Length of buffer used to reserve GDB commands. Has to be at least able to fit the G command, which
//implies a minimum size of about 190 bytes.
#define PBUFLEN 256
//Length of gdb stdout buffer, for console redirection
#define OBUFLEN 32

//The asm stub saves the Xtensa registers here when a debugging exception happens.
struct XTensa_exception_frame_s gdbstub_savedRegs;
#if GDBSTUB_USE_OWN_STACK
//This is the debugging exception stack.
int exceptionStack[256];
#endif

static bool gdb_attached=false;
static bool paused=false;
static unsigned char cmd[PBUFLEN];		//GDB command input buffer
static char chsum;						//Running checksum of the output packet
#if GDBSTUB_CTRLC_BREAK
static void (*uart_isr_callback)(void*, uint8_t)=NULL;
static void* uart_isr_arg=NULL;
#endif
#if GDBSTUB_REDIRECT_CONSOLE_OUTPUT
static void (*uart_putc1_callback)(char)=NULL;
#endif
static unsigned char obuf[OBUFLEN];		//GDB stdout buffer
static size_t obufpos=0;					//Current position in the buffer
static int32_t singleStepPs=-1;			//Stores ps when single-stepping instruction. -1 when not in use.

//Uart libs can reference these to see if gdb is attaching to them
bool gdbstub_has_putc1_control() {
#if GDBSTUB_REDIRECT_CONSOLE_OUTPUT
	return true;
#else
	return false;
#endif
}
bool gdbstub_has_uart_isr_control() {
#if GDBSTUB_CTRLC_BREAK
	return true;
#else
	return false;
#endif
}

void __attribute__((weak)) ATTR_GDBEXTERNFN gdbstub_hook_enable_tx_pin_uart0(uint8_t pin) { (void)pin; }
void __attribute__((weak)) ATTR_GDBEXTERNFN gdbstub_hook_enable_rx_pin_uart0(uint8_t pin) { (void)pin; }


//Small function to feed the hardware watchdog. Needed to stop the ESP from resetting
//due to a watchdog timeout while reading a command.
static void ATTR_GDBFN keepWDTalive() {
	uint64_t *wdtval=(uint64_t*)0x3ff21048;
	uint64_t *wdtovf=(uint64_t*)0x3ff210cc;
	int *wdtctl=(int*)0x3ff210c8;
	*wdtovf=*wdtval+1600000;
	*wdtctl|=(1<<31);
}


//Error states used by the routines that grab stuff from the incoming gdb packet
#define ST_ENDPACKET -1
#define ST_ERR -2
#define ST_OK -3
#define ST_CONT -4
#define ST_DETACH -5

//Grab a hex value from the gdb packet. Ptr will get positioned on the end
//of the hex string, as far as the routine has read into it. Bits/4 indicates
//the max amount of hex chars it gobbles up. Bits can be -1 to eat up as much
//hex chars as possible.
static long ATTR_GDBFN gdbGetHexVal(unsigned char **ptr, int bits) {
	int i;
	int no;
	unsigned int v=0;
	char c;
	no=bits/4;
	if (bits==-1) no=64;
	for (i=0; i<no; i++) {
		c=**ptr;
		(*ptr)++;
		if (c>='0' && c<='9') {
			v<<=4;
			v|=(c-'0');
		} else if (c>='A' && c<='F') {
			v<<=4;
			v|=(c-'A')+10;
		} else if (c>='a' && c<='f') {
			v<<=4;
			v|=(c-'a')+10;
		} else if (c=='#') {
			if (bits==-1) {
				(*ptr)--;
				return v;
			}
			return ST_ENDPACKET;
		} else {
			if (bits==-1) {
				(*ptr)--;
				return v;
			}
			return ST_ERR;
		}
	}
	return v;
}

//Swap an int into the form gdb wants it
static int ATTR_GDBFN iswap(int i) {
	int r;
	r=((i>>24)&0xff);
	r|=((i>>16)&0xff)<<8;
	r|=((i>>8)&0xff)<<16;
	r|=((i>>0)&0xff)<<24;
	return r;
}

//Read a byte from the ESP8266 memory.
static unsigned char ATTR_GDBFN readbyte(unsigned int p) {
	if (p<0x20000000 || p>=0x60000000) return -1;
	int *i=(int*)(p&(~3));
	return *i>>((p&3)*8);
}

//Write a byte to the ESP8266 memory.
static void ATTR_GDBFN writeByte(unsigned int p, unsigned char d) {
	int *i=(int*)(p&(~3));
	if (p<0x20000000 || p>=0x60000000) return;
	if ((p&3)==0) *i=(*i&0xffffff00)|(d<<0);
	if ((p&3)==1) *i=(*i&0xffff00ff)|(d<<8);
	if ((p&3)==2) *i=(*i&0xff00ffff)|(d<<16);
	if ((p&3)==3) *i=(*i&0x00ffffff)|(d<<24);
}

//Returns 1 if it makes sense to write to addr p
static int ATTR_GDBFN validWrAddr(int p) {
	return (p>=0x3ff00000 && p<0x40000000)
		|| (p>=0x40100000 && p<0x40140000)
		|| (p>=0x60000000 && p<0x60002000);
}


static bool ATTR_GDBFN gdbRxFifoIsEmpty() {
	return ((READ_PERI_REG(UART_STATUS(0))>>UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT)==0;
}

static inline bool ATTR_GDBFN gdbTxFifoIsFull() {
	return ((READ_PERI_REG(UART_STATUS(0))>>UART_TXFIFO_CNT_S)&UART_TXFIFO_CNT)>=126;
}

//Send a char to the uart.
static void ATTR_GDBFN gdbSendChar(char c) {
	while (gdbTxFifoIsFull()) ;
	WRITE_PERI_REG(UART_FIFO(0), c);
}


//Send the start of a packet; reset checksum calculation.
static void ATTR_GDBFN gdbPacketStart() {
	chsum=0;
	gdbSendChar('$');
}

//Send a char as part of a packet
static void ATTR_GDBFN gdbPacketChar(char c) {
	if (c=='#' || c=='$' || c=='}' || c=='*') {
		gdbSendChar('}');
		chsum+='}';
		c^=0x20;
	}
	gdbSendChar(c);
	chsum+=c;
}

//Can't be a local variable or gcc will implicitly call memcpy, which could be
//in flash.
static const char hexChars[]="0123456789abcdef";

//Send a hex val as part of a packet. 'bits'/4 dictates the number of hex chars sent.
static void ATTR_GDBFN gdbPacketHex(int val, int bits) {
	int i;
	for (i=bits; i>0; i-=4) {
		gdbPacketChar(hexChars[(val>>(i-4))&0xf]);
	}
}

//Send a hex val as part of a packet. 'bits'/4 dictates the number of hex chars sent.
static void ATTR_GDBFN gdbPacketSwappedHexInt(int val) {
	gdbPacketHex(iswap(val), 32);
}

//Finish sending a packet.
static void ATTR_GDBFN gdbPacketEnd() {
	gdbSendChar('#');
	//Ok to use packet version here since hex char can never be an
	//excape-requiring character
	gdbPacketHex(chsum, 8);
}

// Send a complete packet containing str
static void ATTR_GDBFN gdbSendPacketStr(const char *c) {
	gdbPacketStart();
	while (*c!=0) {
		gdbPacketChar(*c);
		c++;
	}
	gdbPacketEnd();
}

// Send a complete packet containing str as an output command
static inline void ATTR_GDBFN gdbSendOutputPacketStr(const unsigned char* buf, size_t size) {
	size_t i;
	gdbPacketStart();
	gdbPacketChar('O');
	for (i=0; i<size; i++) gdbPacketHex(buf[i], 8);
	gdbPacketEnd();
}

static long ATTR_GDBFN gdbGetSwappedHexInt(unsigned char **ptr) {
	return iswap(gdbGetHexVal(ptr, 32));
}


static const char exceptionSignal[]={4,31,11,11,2,6,8,0,6,7,0,0,7,7,7,7};

//Send the reason execution is stopped to GDB.
static void ATTR_GDBFN sendReason() {
#if 0
	char *reason=""; //default
#endif
	//exception-to-signal mapping
	size_t i;
	gdbPacketStart();
	gdbPacketChar('T');
	if (gdbstub_savedRegs.reason==0xff) {
		gdbPacketHex(2, 8); //sigint
	} else if (gdbstub_savedRegs.reason&0x80) {
		//We stopped because of an exception. Convert exception code to a signal number and send it.
		i=gdbstub_savedRegs.reason&0x7f;
		if (i<sizeof(exceptionSignal)) gdbPacketHex(exceptionSignal[i], 8); else gdbPacketHex(11, 8);
	} else {
		//We stopped because of a debugging exception.
		gdbPacketHex(5, 8); //sigtrap
//Current Xtensa GDB versions don't seem to request this, so let's leave it off.
#if 0
		if (gdbstub_savedRegs.reason&(1<<0)) reason="break";
		if (gdbstub_savedRegs.reason&(1<<1)) reason="hwbreak";
		if (gdbstub_savedRegs.reason&(1<<2)) reason="watch";
		if (gdbstub_savedRegs.reason&(1<<3)) reason="swbreak";
		if (gdbstub_savedRegs.reason&(1<<4)) reason="swbreak";

		gdbPacketStr(reason);
		gdbPacketChar(':');
		//ToDo: watch: send address
#endif
	}
	gdbPacketEnd();
}

//Can't be a local variables or gcc will implicitly call memcpy, which could be
//in flash.
static const char * const gdb_ok_str="OK";
static inline void ATTR_GDBFN gdbSendPacketOK() {
	gdbSendPacketStr(gdb_ok_str);
}

static const char * const gdb_e01_str="E01";
static inline void ATTR_GDBFN gdbSendPacketE01() {
	gdbSendPacketStr(gdb_e01_str);
}

static inline void ATTR_GDBFN gdbSendEmptyPacket() {
	gdbPacketStart();
	gdbPacketEnd();
}

void ATTR_GDBFN gdbstub_flush() {
	size_t i;
	if (obufpos>0) {
		if (gdb_attached) {
			gdbSendOutputPacketStr(obuf, obufpos);
		} else {
			for (i=0; i<obufpos; i++) gdbSendChar(obuf[i]);
		}
		obufpos=0;
	}
}

void ATTR_GDBEXTERNFN gdbstub_buffered_write_char(char c) {
	obuf[obufpos++]=c;
	if (c=='\n' || obufpos==OBUFLEN || paused) {
		gdbstub_flush();
	}
}

void ATTR_GDBEXTERNFN gdbstub_write_char(char c) {
	obuf[obufpos++]=c;
	gdbstub_flush();
}

void ATTR_GDBEXTERNFN gdbstub_write(const char* buf, size_t size) {
	size_t i;
	for (i=0; i<size; i++) gdbstub_buffered_write_char(buf[i]);
	gdbstub_flush();
}

/*
Register file in the format lx106 gdb port expects it.
Inspired by gdb/regformats/reg-xtensa.dat from
https://github.com/jcmvbkbc/crosstool-NG/blob/lx106-g%2B%2B/overlays/xtensa_lx106.tar
As decoded by Cesanta.

struct regfile {
	uint32_t a[16];
	uint32_t pc;
	uint32_t sar;
	uint32_t litbase;
	uint32_t sr176;
	uint32_t sr208;
	uint32_t ps;
};
*/

//Can't be a local variables or gcc will implicitly call memcpy, which could be
//in flash.
static const char * const gdb_supported_str="Supported";
// PacketSize is in hex
static const char * const gdb_supported_resp_str="swbreak+;hwbreak+;PacketSize=FF";
static const char * const gdb_attached_str="Attached";
static const char * const gdb_1_str="1";

//Handle a command as received from GDB.
static inline int ATTR_GDBFN gdbHandleCommand(unsigned char *cmd, int len) {
	(void)len; // unused
	//Handle a command
	int i, j, k;
	unsigned char *data=cmd+1;
	if (cmd[0]=='g') {		//send all registers to gdb
		gdbPacketStart();
		for (i=0; i<16; i++) gdbPacketSwappedHexInt(gdbstub_savedRegs.a[i]);
		gdbPacketSwappedHexInt(gdbstub_savedRegs.pc);
		gdbPacketSwappedHexInt(gdbstub_savedRegs.sar);
		gdbPacketSwappedHexInt(gdbstub_savedRegs.litbase);
		gdbPacketSwappedHexInt(gdbstub_savedRegs.sr176);
		gdbPacketHex(0, 32);
		gdbPacketSwappedHexInt(gdbstub_savedRegs.ps);
		gdbPacketEnd();
	} else if (cmd[0]=='G') {	//receive content for all registers from gdb
		for (i=0; i<16; i++) gdbstub_savedRegs.a[i]=gdbGetSwappedHexInt(&data);
		gdbstub_savedRegs.pc=gdbGetSwappedHexInt(&data);
		gdbstub_savedRegs.sar=gdbGetSwappedHexInt(&data);
		gdbstub_savedRegs.litbase=gdbGetSwappedHexInt(&data);
		gdbstub_savedRegs.sr176=gdbGetSwappedHexInt(&data);
		gdbGetHexVal(&data, 32);
		gdbstub_savedRegs.ps=gdbGetSwappedHexInt(&data);
		gdbSendPacketOK();
	} else if (cmd[0]=='m') {	//read memory to gdb
		i=gdbGetHexVal(&data, -1);
		data++;
		j=gdbGetHexVal(&data, -1);
		gdbPacketStart();
		for (k=0; k<j; k++) {
			gdbPacketHex(readbyte(i++), 8);
		}
		gdbPacketEnd();
	} else if (cmd[0]=='M') {	//write memory from gdb
		i=gdbGetHexVal(&data, -1); //addr
		data++; //skip ,
		j=gdbGetHexVal(&data, -1); //length
		data++; //skip :
		if (validWrAddr(i) && validWrAddr(i+j)) {
			for (k=0; k<j; k++) {
				writeByte(i, gdbGetHexVal(&data, 8));
				i++;
			}
			//Make sure caches are up-to-date. Procedure according to Xtensa ISA document, ISYNC inst desc.
			asm volatile("ISYNC\nISYNC\n");
			gdbSendPacketOK();
		} else {
			//Trying to do a software breakpoint on a flash proc, perhaps?
			gdbSendPacketE01();
		}
	} else if (cmd[0]=='?') {	//Reply with stop reason
		sendReason();
	} else if (cmd[0]=='c') {	//continue execution
		return ST_CONT;
	} else if (cmd[0]=='s') {	//single-step instruction
		//Single-stepping can go wrong if an interrupt is pending, especially when it is e.g. a task switch:
		//the ICOUNT register will overflow in the task switch code. That is why we disable interupts when
		//doing single-instruction stepping.
		singleStepPs=gdbstub_savedRegs.ps;
		gdbstub_savedRegs.ps=(gdbstub_savedRegs.ps & ~0xf)|(XCHAL_DEBUGLEVEL-1);
		gdbstub_icount_ena_single_step();
		return ST_CONT;
	} else if (cmd[0]=='D') {	//detach
		gdbSendPacketOK();
		return ST_DETACH;
	} else if (cmd[0]=='k') {	//kill
		system_restart_core();
	} else if (cmd[0]=='q') {	//Extended query
		if (os_strncmp((char*)&cmd[1], gdb_supported_str, 9)==0) { //Capabilities query
			gdbSendPacketStr(gdb_supported_resp_str); // PacketSize is in hex
		} else if (os_strncmp((char*)&cmd[1], gdb_attached_str, 8)==0) {
			//Let gdb know that it is attaching to a running program
			//In general that just means it detaches instead of killing when it exits
			gdbSendPacketStr(gdb_1_str);
		} else {
			//We don't support other queries.
			gdbSendEmptyPacket();
		}
	} else if (cmd[0]=='Z') {	//Set hardware break/watchpoint.
		data+=2; //skip 'x,'
		i=gdbGetHexVal(&data, -1);
		data++; //skip ','
		j=gdbGetHexVal(&data, -1);
		if (cmd[1]=='1') {	//Set breakpoint
			if (gdbstub_set_hw_breakpoint(i, j)) {
				gdbSendPacketOK();
			} else {
				gdbSendPacketE01();
			}
		} else if (cmd[1]=='2' || cmd[1]=='3' || cmd[1]=='4') { //Set watchpoint
			int access=0;
			unsigned int mask=0;
			if (cmd[1]=='2') access=2; //write
			if (cmd[1]=='3') access=1; //read
			if (cmd[1]=='4') access=3; //access
			if (j==1) mask=0x3F;
			if (j==2) mask=0x3E;
			if (j==4) mask=0x3C;
			if (j==8) mask=0x38;
			if (j==16) mask=0x30;
			if (j==32) mask=0x20;
			if (j==64) mask=0x00;
			if (mask!=0 && gdbstub_set_hw_watchpoint(i,mask, access)) {
				gdbSendPacketOK();
			} else {
				gdbSendPacketE01();
			}
		} else {
			gdbSendEmptyPacket();
		}
	} else if (cmd[0]=='z') {	//Clear hardware break/watchpoint
		data+=2; //skip 'x,'
		i=gdbGetHexVal(&data, -1);
		data++; //skip ','
		j=gdbGetHexVal(&data, -1);
		if (cmd[1]=='1') {	//hardware breakpoint
			if (gdbstub_del_hw_breakpoint(i)) {
				gdbSendPacketOK();
			} else {
				gdbSendPacketE01();
			}
		} else if (cmd[1]=='2' || cmd[1]=='3' || cmd[1]=='4') { //hardware watchpoint
			if (gdbstub_del_hw_watchpoint(i)) {
				gdbSendPacketOK();
			} else {
				gdbSendPacketE01();
			}
		} else {
			gdbSendEmptyPacket();
		}
	} else {
		//We don't recognize or support whatever GDB just sent us.
		gdbSendEmptyPacket();
	}
	return ST_OK;
}

static inline void ATTR_GDBFN gdbPause() {
	paused=true;
	ETS_UART_INTR_DISABLE();
	ets_wdt_disable();
	gdbstub_flush();
}

static inline void ATTR_GDBFN gdbUnpause() {
	paused=false;
	ets_wdt_enable();
	ETS_UART_INTR_ENABLE();
}

enum gdb_read_state_t {
	gdb_read_state_init,
	gdb_read_state_cmd_read,
	gdb_read_state_escape_char,
	gdb_read_state_read_chsum_1,
	gdb_read_state_read_chsum_2,
};

//Lower layer: grab a command packet and check the checksum
//Calls gdbHandleCommand on the packet if the checksum is OK
//Returns only if execution of the user program should continue
//Otherwise keeps reading uart data and executing commands
//Flags that gdb has been attached whenever a gdb formatted
// packet is received
//While gdb is attached, checks for ctl-c (\x03) if it's not
// already paused
//Keeps reading commands if it is paused, until either a
// continue, detach, or kill command is received
//It is not necessary for gdb to be attached for it to be paused
//For example, during an exception break, the program is
// paused but gdb might not be attached yet
static void ATTR_GDBFN gdbReadCommand(bool system_stopped) {
	static enum gdb_read_state_t read_state=gdb_read_state_init;
	static unsigned char chsum;
	static unsigned char sentchs[2];
	static int p;
	unsigned char c;
	unsigned char rchsum;
	unsigned char *ptr;
	if (system_stopped) {
		gdbPause();
		sendReason();
	}
	while (true) {
		if (paused) {
			while (gdbRxFifoIsEmpty()) {
				keepWDTalive();
			}
		}
		if (gdbRxFifoIsEmpty()) {
			break;
		}
		c=READ_PERI_REG(UART_FIFO(0)) & 0xFF;
#if GDBSTUB_CTRLC_BREAK
		if (!gdb_attached && uart_isr_callback!=NULL) {
			uart_isr_callback(uart_isr_arg, c);
		}
#endif
		switch (read_state) {
		case gdb_read_state_init:
			if (c=='$') {
				read_state=gdb_read_state_cmd_read;
				chsum=0;
				p=0;
			} else if (c==0x3 && gdb_attached && !paused) {
				gdbPause();
				gdbstub_savedRegs.reason=0xff; //mark as user break reason
				sendReason();
			}
			break;
		case gdb_read_state_cmd_read:
			if (c=='#') {	//end of packet, checksum follows
				read_state=gdb_read_state_read_chsum_1;
				cmd[p]=0;
			} else if (c=='$') {
				//Wut, restart packet?
				chsum=0;
				p=0;
			} else if ((p+1)>=PBUFLEN) {
				//Received more than the size of the command buffer
				read_state=gdb_read_state_init;
			} else {
				chsum+=c;
				if (c=='}') {		//escape the next char
					read_state=gdb_read_state_escape_char;
				} else {
					cmd[p++]=c;
				}
			}
			break;
		case gdb_read_state_escape_char:
			chsum+=c;
			cmd[p++]=c^0x20;
			break;
		case gdb_read_state_read_chsum_1:
			read_state=gdb_read_state_read_chsum_2;
			sentchs[0]=c;
			break;
		case gdb_read_state_read_chsum_2:
			read_state=gdb_read_state_init;
			sentchs[1]=c;
			ptr=&sentchs[0];
			rchsum=gdbGetHexVal(&ptr, 8);
			if (rchsum==chsum) {
				gdb_attached=true;
				if (!paused) {
					gdbPause();
					gdbstub_savedRegs.reason=0xff; //mark as user break reason
				}
				gdbSendChar('+');
				switch (gdbHandleCommand(cmd, p)) {
				case ST_DETACH:
					gdb_attached=false;
					if (system_stopped) {
						break;
					}
				case ST_CONT:
					gdbUnpause();
				}
			} else if (gdb_attached) {
				gdbSendChar('-');
			}
			break;
		}
	}
}



//Get the value of one of the A registers
static unsigned int ATTR_GDBFN getaregval(int reg) {
	return gdbstub_savedRegs.a[reg];
}

//Set the value of one of the A registers
static inline void ATTR_GDBFN setaregval(int reg, unsigned int val) {
	// os_printf("%x -> %x\n", val, reg);
	gdbstub_savedRegs.a[reg]=val;
}

//Emulate the l32i/s32i instruction we're stopped at.
static inline void ATTR_GDBFN emulLdSt() {
	unsigned char i0=readbyte(gdbstub_savedRegs.pc);
	unsigned char i1=readbyte(gdbstub_savedRegs.pc+1);
	unsigned char i2;
	int *p;

	if ((i0&0xf)==2 && (i1&0xb0)==0x20) {
		//l32i or s32i
		i2=readbyte(gdbstub_savedRegs.pc+2);
		p=(int*)getaregval(i1&0xf)+(i2*4);
		i0>>=4;
		if ((i1&0xf0)==0x20) { //l32i
			setaregval(i0, *p);
		} else { //s32i
			*p=getaregval(i0);
		}
		gdbstub_savedRegs.pc+=3;
	} else if ((i0&0xe)==0x8) {
		//l32i.n or s32i.n
		p=(int*)getaregval(i1&0xf)+((i1>>4)*4);
		if ((i0&0xf)==0x8) { //l32i.n
			setaregval(i0>>4, *p);
		} else {
			*p=getaregval(i0>>4);
		}
		gdbstub_savedRegs.pc+=2;
	// } else {
	// 	os_printf("GDBSTUB: No l32i/s32i instruction: %x %x. Huh?", i1, i0);
	}
}

//We just caught a debug exception and need to handle it. This is called from an assembly
//routine in gdbstub-entry.S
void ATTR_GDBFN gdbstub_handle_debug_exception() {
	if (singleStepPs!=-1) {
		//We come here after single-stepping an instruction. Interrupts are disabled
		//for the single step. Re-enable them here.
		gdbstub_savedRegs.ps=(gdbstub_savedRegs.ps&~0xf)|(singleStepPs&0xf);
		singleStepPs=-1;
	}

	gdbReadCommand(true);
	if ((gdbstub_savedRegs.reason&0x84)==0x4) {
		//We stopped due to a watchpoint. We can't re-execute the current instruction
		//because it will happily re-trigger the same watchpoint, so we emulate it
		//while we're still in debugger space.
		emulLdSt();
	} else if ((gdbstub_savedRegs.reason&0x88)==0x8) {
		//We stopped due to a BREAK instruction. Skip over it.
		//Check the instruction first; gdb may have replaced it with the original instruction
		//if it's one of the breakpoints it set.
		if (readbyte(gdbstub_savedRegs.pc+2)==0 &&
					(readbyte(gdbstub_savedRegs.pc+1)&0xf0)==0x40 &&
					(readbyte(gdbstub_savedRegs.pc)&0x0f)==0x00) {
			gdbstub_savedRegs.pc+=3;
		}
	} else if ((gdbstub_savedRegs.reason&0x90)==0x10) {
		//We stopped due to a BREAK.N instruction. Skip over it, after making sure the instruction
		//actually is a BREAK.N
		if ((readbyte(gdbstub_savedRegs.pc+1)&0xf0)==0xf0 &&
					readbyte(gdbstub_savedRegs.pc)==0x2d) {
			gdbstub_savedRegs.pc+=3;
		}
	}
}



#if GDBSTUB_BREAK_ON_EXCEPTION || GDBSTUB_CTRLC_BREAK

#if !GDBSTUB_FREERTOS
static inline void ATTR_GDBFN gdbReadCommandWithFrame(bool system_stopped, void* frame) {
	//Copy registers the Xtensa HAL did save to gdbstub_savedRegs
	os_memcpy(&gdbstub_savedRegs, frame, 5*4);
	os_memcpy(&gdbstub_savedRegs.a[2], ((uint32_t*)frame)+5, 14*4);
	//Credits go to Cesanta for this trick. A1 seems to be destroyed, but because it
	//has a fixed offset from the address of the passed frame, we can recover it.
	gdbstub_savedRegs.a[1]=(uint32_t)frame+EXCEPTION_GDB_SP_OFFSET;

	gdbReadCommand(system_stopped);
	//Copy any changed registers back to the frame the Xtensa HAL uses.
	os_memcpy(frame, &gdbstub_savedRegs, 5*4);
	os_memcpy(((uint32_t*)frame)+5, &gdbstub_savedRegs.a[2], 14*4);
}
#endif

#endif

#if GDBSTUB_BREAK_ON_EXCEPTION

#if GDBSTUB_FREERTOS
//Freertos exception. This routine is called by an assembly routine in gdbstub-entry.S
void ATTR_GDBFN gdbstub_handle_user_exception() {
	gdbstub_savedRegs.reason|=0x80; //mark as an exception reason
	gdbReadCommand(true);
}

//FreeRTOS doesn't use the Xtensa HAL for exceptions, but uses its own fatal exception handler.
//We use a small hack to replace that with a jump to our own handler, which then has the task of
//decyphering and re-instating the registers the FreeRTOS code left.
extern void user_fatal_exception_handler();
extern void gdbstub_user_exception_entry();

static void ATTR_GDBINIT install_exceptions() {
	//Replace the user_fatal_exception_handler by a jump to our own code
	int *ufe=(int*)user_fatal_exception_handler;
	//This mess encodes as a relative jump instruction to user_fatal_exception_handler
	*ufe=((((int)gdbstub_user_exception_entry-(int)user_fatal_exception_handler)-4)<<6)|6;
}
#else
//Non-OS exception handler. Gets called by the Xtensa HAL.
static void ATTR_GDBFN gdbstub_exception_handler(struct XTensa_exception_frame_s *frame) {
	//Save the extra registers the Xtensa HAL doesn't save
	gdbstub_save_extra_sfrs_for_exception();
	gdbstub_savedRegs.reason|=0x80; //mark as an exception reason
	*((uint32_t*)UART_INT_ENA(0)) = 0;
	gdbReadCommandWithFrame(true, (void*)frame);
}

//The OS-less SDK uses the Xtensa HAL to handle exceptions. We can use those functions to catch any
//fatal exceptions and invoke the debugger when this happens.
static void ATTR_GDBINIT install_exceptions() {
	unsigned int i;
	int exno[]={EXCCAUSE_ILLEGAL, EXCCAUSE_SYSCALL, EXCCAUSE_INSTR_ERROR, EXCCAUSE_LOAD_STORE_ERROR,
			EXCCAUSE_DIVIDE_BY_ZERO, EXCCAUSE_UNALIGNED, EXCCAUSE_INSTR_DATA_ERROR, EXCCAUSE_LOAD_STORE_DATA_ERROR,
			EXCCAUSE_INSTR_ADDR_ERROR, EXCCAUSE_LOAD_STORE_ADDR_ERROR, EXCCAUSE_INSTR_PROHIBITED,
			EXCCAUSE_LOAD_PROHIBITED, EXCCAUSE_STORE_PROHIBITED};
	for (i=0; i<(sizeof(exno)/sizeof(exno[0])); i++) {
		_xtos_set_exception_handler(exno[i], gdbstub_exception_handler);
	}
}
#endif

#endif



#if GDBSTUB_REDIRECT_CONSOLE_OUTPUT
//Replacement putchar1 routine. Instead of spitting out the character directly, it will buffer up to
//OBUFLEN characters (or up to a \n, whichever comes earlier) and send it out as a gdb stdout packet.
static void ATTR_GDBEXTERNFN gdbstub_semihost_putchar1(char c) {
	if (!gdb_attached && uart_putc1_callback != NULL) {
		uart_putc1_callback(c);
		return;
	}
	gdbstub_buffered_write_char(c);
}

void ATTR_GDBINIT gdbstub_set_putc1_callback(void (*func)(char)) {
	uart_putc1_callback=func;
}
#endif



#if GDBSTUB_FREERTOS
static void ATTR_GDBINIT configure_uart() {}
#else
static void ATTR_GDBINIT configure_uart() {
    gdbstub_hook_enable_tx_pin_uart0(1);
    gdbstub_hook_enable_rx_pin_uart0(3);
	
	WRITE_PERI_REG(UART_CONF0(0), 0b00011100); //8N1

	SET_PERI_REG_MASK(UART_CONF0(0), UART_RXFIFO_RST | UART_TXFIFO_RST);    //RESET FIFO
	CLEAR_PERI_REG_MASK(UART_CONF0(0), UART_RXFIFO_RST | UART_TXFIFO_RST);
}
#endif

#if GDBSTUB_CTRLC_BREAK

#if GDBSTUB_FREERTOS

void ATTR_GDBFN gdbstub_handle_uart_int(struct XTensa_rtos_int_frame_s *frame) {
	int doDebug=0, fifolen=0, x;

	fifolen=(READ_PERI_REG(UART_STATUS(0))>>UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT;
	while (fifolen!=0) {
		//Check if any of the chars is control-C. Throw away rest.
		if ((READ_PERI_REG(UART_FIFO(0)) & 0xFF)==0x3) doDebug=1;
		fifolen--;
	}
	WRITE_PERI_REG(UART_INT_CLR(0), UART_RXFIFO_FULL_INT_CLR|UART_RXFIFO_TOUT_INT_CLR);

	if (doDebug) {
		//Copy registers the Xtensa HAL did save to gdbstub_savedRegs
		gdbstub_savedRegs.pc=frame->pc;
		gdbstub_savedRegs.ps=frame->ps;
		gdbstub_savedRegs.sar=frame->sar;
		for (x=0; x<16; x++) gdbstub_savedRegs.a[x]=frame->a[x];

//		gdbstub_savedRegs.a1=(uint32_t)frame+EXCEPTION_GDB_SP_OFFSET;

		gdbstub_savedRegs.reason=0xff; //mark as user break reason

		sendReason();
		gdbReadCommand(false);
		//Copy any changed registers back to the frame the Xtensa HAL uses.
		frame->pc=gdbstub_savedRegs.pc;
		frame->ps=gdbstub_savedRegs.ps;
		frame->sar=gdbstub_savedRegs.sar;
		for (x=0; x<16; x++) frame->a[x]=gdbstub_savedRegs.a[x];
	}
}

static void ATTR_GDBINIT install_uart_hdlr() {
	os_isr_attach(ETS_UART_INUM, gdbstub_uart_entry);
	SET_PERI_REG_MASK(UART_INT_ENA(0), UART_RXFIFO_FULL_INT_ENA|UART_RXFIFO_TOUT_INT_ENA);
	ETS_UART_INTR_ENABLE();
}

#else

static void ATTR_GDBFN gdbstub_uart_hdlr(void* arg, void* frame) {
	(void) arg;
	//Save the extra registers the Xtensa HAL doesn't save
	gdbstub_save_extra_sfrs_for_exception();
	gdbReadCommandWithFrame(false, frame);
	WRITE_PERI_REG(UART_INT_CLR(0), UART_RXFIFO_FULL_INT_CLR|UART_RXFIFO_TOUT_INT_CLR);
}

static void ATTR_GDBINIT install_uart_hdlr() {
	ETS_UART_INTR_DISABLE();
	ETS_UART_INTR_ATTACH(gdbstub_uart_hdlr, NULL);

	configure_uart();

	WRITE_PERI_REG(UART_CONF1(0),
		((100 & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
		((0x02 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S) |
		UART_RX_TOUT_EN);

	WRITE_PERI_REG(UART_INT_CLR(0), 0xffff);
	SET_PERI_REG_MASK(UART_INT_ENA(0), UART_RXFIFO_FULL_INT_ENA|UART_RXFIFO_TOUT_INT_ENA);
	ETS_UART_INTR_ENABLE();
}

void ATTR_GDBINIT gdbstub_set_uart_isr_callback(void (*func)(void*, uint8_t), void* arg) {
	ETS_UART_INTR_DISABLE();
	uart_isr_callback=func;
	uart_isr_arg=arg;
	ETS_UART_INTR_ENABLE();
}

#endif

#endif



//gdbstub initialization routine.
void ATTR_GDBINIT gdbstub_init() {
#if GDBSTUB_REDIRECT_CONSOLE_OUTPUT
	os_install_putc1(gdbstub_semihost_putchar1);
#endif
#if GDBSTUB_CTRLC_BREAK
	install_uart_hdlr();
#else
	configure_uart();
#endif
#if GDBSTUB_BREAK_ON_EXCEPTION
	install_exceptions();
#endif
	gdbstub_init_debug_entry();
#if GDBSTUB_BREAK_ON_INIT
	gdbstub_do_break();
#endif
}

bool ATTR_GDBEXTERNFN gdb_present()
{
	return true;
}

void ATTR_GDBFN gdb_do_break() { gdbstub_do_break(); }
void ATTR_GDBINIT gdb_init() __attribute__((alias("gdbstub_init")));
