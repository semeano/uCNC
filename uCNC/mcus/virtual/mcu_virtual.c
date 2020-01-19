/*
	Name: mcu_virtual.c
	Description: Simulates and MCU that runs on a PC. This is mainly used to test/simulate uCNC.
		For now it's only working/tested on Windows.
		Besides all the functions declared in the mcu.h it also implements the code responsible
		for handling:
			interpolator.h
				void itp_step_isr();
				void itp_step_reset_isr();
			serial.h
				void serial_rx_isr(char c);
				char serial_tx_isr();
			trigger_control.h
				void dio_limits_isr(uint8_t limits);
				void dio_controls_isr(uint8_t controls);
				
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 01/11/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include "../../config.h"
#if(MCU == MCU_VIRTUAL)
#include "../../mcudefs.h"

#include "../../mcumap.h"
#include "../../mcu.h"
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h> 
#include <math.h>

typedef union{
    uint32_t r; // occupies 4 bytes
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    struct
    {
        uint16_t rl;
        uint16_t rh;
    };
    struct
    {
        uint8_t r0;
        uint8_t r1;
        uint8_t r2;
        uint8_t r3;
    };
#else
    struct
    {
        uint16_t rh;
        uint16_t rl;
    };
    struct
    {
        uint8_t r3;
        uint8_t r2;
        uint8_t r1;
        uint8_t r0;
    }
    ;
#endif
} IO_REGISTER;

typedef struct virtual_map_t
{
	uint8_t steps;
	uint8_t dirs;
	uint8_t controls;
	uint8_t limits;
	uint8_t probe;
	IO_REGISTER outputs;
}VIRTUAL_MAP;

#include "../../settings.h"
#include "timer.h"
#include "virtualserial.h"
#include "../../serial.h"
#include "../../interpolator.h"

#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#ifndef BAUD
#define BAUD 115200
#endif

#ifndef COM_BUFFER_SIZE
#define COM_BUFFER_SIZE 50
#endif

//#define USECONSOLE

virtports_t virtualports;

static VIRTUAL_MAP virtualmap;
/**/

//UART communication
uint8_t g_mcu_combuffer[COM_BUFFER_SIZE];
uint8_t g_mcu_bufferhead;
uint8_t g_mcu_buffertail;
uint8_t g_mcu_buffercount;
char* mcu_tx_buffer;
volatile bool mcu_tx_ready;
/*
#ifdef __DEBUG__
#define MAX(A,B) if(B>A) A=B
volatile uint8_t g_mcu_perfCounterOffset = 0;
volatile PERFORMANCE_METER mcu_performacecounters;
#endif
*/
uint32_t _previnputs = 0;

volatile bool global_irq_enabled = false;

unsigned long g_cpu_freq = 0;
volatile bool pulse_enabled = false;
volatile bool integrator_enabled = false;
volatile unsigned long pulse_interval = 0;
volatile unsigned long resetpulse_interval = 0;
//volatile unsigned long integrator_interval = F_CPU/F_INTEGRATOR;
volatile unsigned long pulse_counter = 0;
volatile unsigned long *pulse_counter_ptr;
volatile unsigned long integrator_counter = 0;
volatile bool send_char = false;

pthread_t thread_id;
pthread_t thread_idout;
pthread_t thread_timer_id;

void* timersimul()
{
	unsigned long pulse_counter = 0;
	unsigned long integrator_counter = 0;
	unsigned long ticks = 0;

	for(;;)
	{
		if(global_irq_enabled && pulse_enabled)
		{
			if((*pulse_counter_ptr)==pulse_interval && pulse_enabled )
			{
				itp_step_isr();
			}
			
			if((*pulse_counter_ptr)>=resetpulse_interval && pulse_enabled )
			{
				(*pulse_counter_ptr) = 0;
				itp_step_reset_isr();
			}
		}
	}
}


void* comsimul()
{
	for(;;)
	{
		#ifdef USECONSOLE
		char c = getch();
		#else
		char c = virtualserial_getc();
		#endif
		if(c != 0)
		{
			serial_rx_isr(c);
		}
	}
}

void* comoutsimul()
{
	char combuffer[128];
	static uint8_t i = 0;
	for(;;)
	{
		if(mcu_tx_ready)
		{
			char c = serial_tx_isr();
			if(c != 0)
			{
				#ifdef USECONSOLE
				mcu_putc(c);
				#else
				combuffer[i] = c;
				i++;
				if(c == '\n')
				{
					combuffer[i] = 0;
					virtualserial_puts(combuffer);
					i = 0;
				}
				#endif
			}
			else
			{
				mcu_tx_ready = false;
			}
		}
	}
}

void ticksimul()
{
	static uint16_t tick_counter = 0;
	/*
	FILE *infile = fopen("inputs.txt", "r");
	char inputs[255];
	
	if(infile!=NULL)
	{
		fscanf(infile, "%uX %uX", &(virtualports->controls), &(virtualports->limits));
		fclose(infile);
	}
	*/
	if(global_irq_enabled)
	{
		if(pulse_enabled)
			(*pulse_counter_ptr)++;
	}
}

void mcu_init()
{
	send_char = false;
	virtualports = &virtualmap;
	FILE *infile = fopen("inputs.txt", "w+");
	if(infile!=NULL)
	{
		fprintf(infile, "%X %X", virtualports->controls, virtualports->limits);
		fflush(infile);
		fclose(infile);
	}
	else
	{
		printf("Failed to open input file");
	}
	
	g_cpu_freq = getCPUFreq();
	#ifndef USECONSOLE
	virtualserial_open();
	#endif
	start_timer(1, &ticksimul);
	pthread_create(&thread_id, NULL, &comsimul, NULL);
	pthread_create(&thread_idout, NULL, &comoutsimul, NULL);
	mcu_tx_ready = false;
	pthread_create(&thread_timer_id, NULL, &timersimul, NULL); 
	g_mcu_buffercount = 0;
	pulse_counter_ptr = &pulse_counter;
	mcu_enable_interrupts();
	
}

//IO functions    
//Inputs  
//returns the value of the input pins
uint32_t mcu_get_inputs()
{
	return 0;	
}
//returns the value of the critical input pins
uint8_t mcu_get_controls()
{
	return virtualports->controls;
}

uint8_t mcu_get_limits()
{
	return virtualports->limits;
}

uint8_t mcu_get_probe()
{
	return virtualports->probe;
}

#ifdef PROBE
void mcu_enable_probe_isr()
{
}
void mcu_disable_probe_isr()
{
}
#endif

uint8_t mcu_get_analog(uint8_t channel)
{
	return 0;
}

void mcu_set_pwm(uint8_t pwm, uint8_t value)
{
}

uint8_t mcu_get_pwm(uint8_t pwm)
{
	return 0;
}

//outputs
//sets all step and dir pins
void mcu_set_steps(uint8_t value)
{	
	virtualports->steps = value;
}

void mcu_set_dirs(uint8_t value)
{	
	virtualports->dirs = value;
}

//sets all digital outputs pins
void mcu_set_outputs(uint32_t value)
{
	IO_REGISTER reg;
	reg.r = value;
	
	#ifdef DOUTS_R0_OUTREG
		virtualports->outputs.r0 = ((~DOUTS_R0_MASK & virtualports->outputs.r0) | reg.r0);
	#endif
	#ifdef DOUTS_R1_OUTREG
		virtualports->outputs.r1 = ((~DOUTS_R1_MASK & virtualports->outputs.r1) | reg.r1);
	#endif
	#ifdef DOUTS_R2_OUTREG
		virtualports->outputs.r2 = ((~DOUTS_R2_MASK & virtualports->outputs.r2) | reg.r2);
	#endif
	#ifdef DOUTS_R3_OUTREG
		virtualports->outputs.r3 = ((~DOUTS_R3_MASK & virtualports->outputs.r3) | reg.r3);
	#endif
}

uint32_t mcu_get_outputs()
{
	IO_REGISTER reg;

	#ifdef DOUTS_R0_OUTREG
		reg.r0 = virtualports->outputs.r0;
	#endif
	#ifdef DOUTS_R1_OUTREG
		reg.r1 = virtualports->outputs.r1;
	#endif
	#ifdef DOUTS_R2_OUTREG
		reg.r2 = virtualports->outputs.r2;
	#endif
	#ifdef DOUTS_R3_OUTREG
		reg.r3 = virtualports->outputs.r3;
	#endif
	
	return (reg.r & DOUTS_MASK);
}

//Communication functions
//sends a packet
void mcu_start_send()
{
	mcu_tx_ready = true;
}

void mcu_putc(char c)
{
	#ifdef USECONSOLE
	putchar(c);
	#else
	virtualserial_putc(c);
	#endif
}

char mcu_getc()
{
	char c = 0;
	if(g_mcu_buffertail!=g_mcu_bufferhead)
	{
		c = g_mcu_combuffer[g_mcu_buffertail];
		if(++g_mcu_buffertail==COM_BUFFER_SIZE)
		{
			g_mcu_buffertail = 0;
		}
		
		if(c=='\n')
		{
			g_mcu_buffercount--;
		}
	}
	
	return c;
}

char mcu_peek()
{
	if(g_mcu_buffercount==0)
		return 0;
	return g_mcu_combuffer[g_mcu_buffertail];
}

void mcu_bufferClear()
{
	memset(&g_mcu_combuffer, 0, sizeof(char)*COM_BUFFER_SIZE);
	g_mcu_buffertail = 0;
	g_mcu_bufferhead = 0;
}

//RealTime
void mcu_freq_to_clocks(float frequency, uint16_t* ticks, uint8_t* tick_reps)
{
	if(frequency < F_STEP_MIN)
		frequency = F_STEP_MIN;
	if(frequency > F_STEP_MAX)
		frequency = F_STEP_MAX;

	*ticks = (uint16_t)floorf((F_CPU/frequency));
	*tick_reps = 1;
}

//enables all interrupts on the mcu. Must be called to enable all IRS functions
void mcu_enable_interrupts()
{
	global_irq_enabled = true;
}
//disables all ISR functions
void mcu_disable_interrupts()
{
	global_irq_enabled = false;
}

//starts a constant rate pulse at a given frequency. This triggers to ISR handles with an offset of MIN_PULSE_WIDTH useconds
void mcu_start_step_ISR(uint16_t clocks_speed, uint8_t prescaller)
{
	pulse_interval = clocks_speed>>1;
	resetpulse_interval = clocks_speed;
	(*pulse_counter_ptr) = 0;
	pulse_enabled = true;
}

void mcu_change_step_ISR(uint16_t clocks_speed, uint8_t prescaller)
{
	pulse_enabled = false;
	pulse_interval = clocks_speed>>1;
	resetpulse_interval = clocks_speed;
	(*pulse_counter_ptr) = 0;
	pulse_enabled = true;
}
//stops the pulse 
void mcu_step_stop_ISR()
{
	pulse_enabled = false;
}

void mcu_delay_ms(uint16_t miliseconds)
{
}

void mcu_printfp(const char* __fmt, ...)
{
	char buffer[50];
	char* newfmt = strcpy((char*)&buffer, __fmt);
	va_list __ap;
 	va_start(__ap,__fmt);
 	vprintf(newfmt,__ap);
 	va_end(__ap);
}

void mcu_loadDummyPayload(const char* __fmt, ...)
{
	char buffer[30];
	char payload[50];
	char* newfmt = strcpy((char*)&buffer, __fmt);
	va_list __ap;
 	va_start(__ap,__fmt);
 	vsprintf((char*)&payload, newfmt,__ap);
 	va_end(__ap);
	g_mcu_bufferhead = strlen(payload);
	memset(&g_mcu_combuffer, 0, g_mcu_bufferhead);
	strcpy((char*)&g_mcu_combuffer, payload);
	g_mcu_buffertail = 0;
	g_mcu_buffercount++;
}

uint8_t mcu_eeprom_getc(uint16_t address)
{
	FILE* fp = fopen("virtualeeprom", "r");
	uint8_t c = 0;
	
	if(fp!=NULL)
	{
		if(!fseek(fp, address, SEEK_SET))
		{
			c = getc(fp);
			fclose(fp);
		}
		
	}
	
	return c;
}

uint8_t mcu_eeprom_putc(uint16_t address, uint8_t value)
{
	FILE* src = fopen("virtualeeprom", "r");
	FILE* dest = fopen("newvirtualeeprom", "w");
	
	for(int i = 0; i < address; i++)
	{
		if(src!=NULL)
			putc(getc(src), dest);
		else
			putc(0, dest);
	}
	
	if(src!=NULL)
		getc(src);
	putc(value, dest);
	
	for(int i = address + 1; i < 1024; i++)
	{
		if(src!=NULL)
			putc(getc(src), dest);
		else
			putc(0, dest);
	}
	
	if(src!=NULL)
		fclose(src);
	fflush(dest);
	fclose(dest);
	remove("virtualeeprom");
	rename("newvirtualeeprom", "virtualeeprom");
	
	return value;
}

void mcu_startPerfCounter()
{
	startCycleCounter();
}

uint16_t mcu_stopPerfCounter()
{
    return (uint16_t)stopCycleCounter();
}

#endif