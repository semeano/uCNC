/*
	Name: serial.c
	Description: Serial communication basic read/write functions uCNC.
	
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 30/12/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include "grbl_interface.h"
#include "settings.h"
#include "mcudefs.h"
#include "mcu.h"
#include "io_control.h"
#include "cnc.h"
#include "serial.h"
#include "utils.h"

#include <math.h>

#define RX_BUFFER_SIZE 128
#define TX_BUFFER_SIZE 112


static unsigned char serial_rx_buffer[RX_BUFFER_SIZE];
volatile static uint8_t serial_rx_count;
volatile static uint8_t serial_rx_read;
volatile static uint8_t serial_rx_write;

static unsigned char serial_tx_buffer[TX_BUFFER_SIZE];
volatile static uint8_t serial_tx_read;
volatile static uint8_t serial_tx_write;
volatile static uint8_t serial_tx_count;

void serial_init()
{
	#ifdef FORCE_GLOBALS_TO_0
	serial_rx_write = 0;
	serial_rx_read = 0;
	serial_rx_count = 0;
	
	serial_tx_read = 0;
	serial_tx_write = 0;
	serial_tx_count = 0;
	
	//resets buffers
	memset(&serial_rx_buffer, 0, sizeof(serial_rx_buffer));
	memset(&serial_tx_buffer, 0, sizeof(serial_tx_buffer));
	#endif
}

void serial_clear()
{
	serial_rx_write = 0;
	serial_rx_read = 0;
	serial_rx_count = 0;
	
	serial_tx_read = 0;
	serial_tx_write = 0;
	serial_tx_count = 0;
}

bool serial_rx_is_empty()
{
	return (!serial_rx_count);
}

bool serial_tx_is_empty()
{
	return (!serial_tx_count);
}

unsigned char serial_getc()
{
	#ifdef ECHO_CMD
	static bool echo = false;
	#endif
	unsigned char c = '\0';
	
	if(!serial_rx_count)
	{
		return c;
	}
	
	#ifdef ECHO_CMD
	if(!echo)
	{
		echo = true;
		serial_print_str(MSG_ECHO);
	}	
    #endif
	
	c = serial_rx_buffer[serial_rx_read];
	switch(c)
	{
		case '\n':
			serial_rx_count--;
			#ifdef ECHO_CMD
			echo = false;
		    serial_print_str(__romstr__("]\r\n"));
		    #endif
			break;
		default:
			#ifdef ECHO_CMD
			serial_putc(c);
			#endif
			break;				
	}
	
	serial_rx_read++;
	if(serial_rx_read == RX_BUFFER_SIZE)
	{
		serial_rx_read = 0;
	}
	
	return c;
}

unsigned char serial_peek()
{
	return ((serial_rx_count != 0) ? serial_rx_buffer[serial_rx_read] : 0);
}

void serial_inject_cmd(const unsigned char* __s)
{
	unsigned char c = rom_strptr(__s++);
	do{
		serial_rx_isr(c);
		c = rom_strptr(__s++);
	} while(c != 0);
}

void serial_discard_cmd()
{
	if(serial_rx_count != 0)
	{
		while(serial_getc() != '\n');
	}
}

void serial_putc(unsigned char c)
{
	while((serial_tx_write == serial_tx_read) && (serial_tx_count != 0))
	{
		mcu_start_send();
		cnc_doevents();	
	} //while buffer is full 
	
	serial_tx_buffer[serial_tx_write] = c;
	serial_tx_write++;
	if(c == '\n')
	{
		serial_tx_count++;
		mcu_start_send();
	}

	if(serial_tx_write == TX_BUFFER_SIZE)
	{
		serial_tx_write = 0;
	}
}

void serial_print_str(const unsigned char* __s)
{
	unsigned char c = rom_strptr(__s++);
	do
	{
		serial_putc(c);
		c = rom_strptr(__s++);
	} while(c != 0);
}

void serial_print_int(uint16_t num)
{
	if (num == 0)
	{
		serial_putc('0');
		return;
	}
	
	unsigned char buffer[6];
	uint8_t i = 0;
	
	while (num > 0)
	{
		uint8_t digit = num % 10;
		num = ((((uint32_t)num * (UINT16_MAX/10))>>16) + ((digit!=0) ? 0 : 1)); //same has divide by 10 but faster
		buffer[i++] = digit;
		/*buffer[i++] = num % 10;
		num /= 10;*/
	}
	
	do
	{
		i--;
		serial_putc('0' + buffer[i]);
	}while(i);
}
/*
void serial_print_int(uint16_t num)
{
	char buffer[6];
	sprintf(buffer, MSG_INT, num);
	
	uint8_t i = 0;
	do
	{
		serial_putc(buffer[i]);
		i++;
	} while(buffer[i] != 0);	

}*/

void serial_print_flt(float num)
{
	if(g_settings.report_inches)
	{
		num *= MM_INCH_MULT;
	}
	
	if(num < 0)
	{
		serial_putc('-');
		num = -num;
	}
	
	uint16_t digits = (uint16_t)floorf(num);
	serial_print_int(digits);
	serial_putc('.');
	num -= digits;
	
	num *= 1000;
	digits = (uint16_t)roundf(num);
	
	if(g_settings.report_inches)
	{
		if(digits<10000)
		{
			serial_putc('0');
		}
		
		if(digits<1000)
		{
			serial_putc('0');
		}
	}
	
	if(digits<100)
	{
		serial_putc('0');
	}
	
	if(digits<10)
	{
		serial_putc('0');
	}

	serial_print_int(digits);
}

void serial_print_intarr(uint16_t* arr, uint8_t count)
{
	do
	{
		serial_print_int(*arr++);
		count--;
		if(count)
		{
			serial_putc(',');
		}
			
	}while(count);
}

void serial_print_fltarr(float* arr, uint8_t count)
{
	do
	{
		serial_print_flt(*arr++);
		count--;
		if(count)
		{
			serial_putc(',');
		}
			
	}while(count);

}

void serial_flush()
{
	while(serial_tx_count)
	{
		mcu_start_send();
		cnc_doevents();
	}
}

//ISR

void serial_rx_isr(unsigned char c)
{
	static uint8_t comment_count = 0;
	
	//c &= 0x7F;
	
	if((c > 0x22) && (c < 0x7B))
	{
		switch(c)
		{
			case RT_CMD_REPORT:
				cnc_call_rt_command((uint8_t)RT_CMD_REPORT);
				return;
			case '(':
				comment_count++;
				return;
			case ')':
				comment_count--;
				return;
			default:
				if(!comment_count)
				{
					serial_rx_buffer[serial_rx_write] = c;
					serial_rx_write++;
				}
				break;
		}
	}
	else
	{
		switch(c)
		{
			case '\r':
				c = '\n';//replaces CR with LF
			case '\n':
				serial_rx_buffer[serial_rx_write] = c;
				serial_rx_write++;
				serial_rx_count++;
				comment_count = 0;
				break;
			default:
				cnc_call_rt_command((uint8_t)c);
				return;
		}
	}
	
	if(serial_rx_write == RX_BUFFER_SIZE)
	{
		serial_rx_write = 0;
	}
}

unsigned char serial_tx_isr()
{
	if(serial_tx_count == 0)
	{
		return 0;
	}
	
	unsigned char c = serial_tx_buffer[serial_tx_read];
	
	if(c == '\n')
	{
		serial_tx_count--;
	}

	if(++serial_tx_read == TX_BUFFER_SIZE)
	{
		serial_tx_read = 0;
	}
	
	return c;
}

