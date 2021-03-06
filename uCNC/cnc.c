/*
	Name: cnc.c
	Description: uCNC main unit.
	
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 17/09/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "utils.h"
#include "settings.h"
#include "mcudefs.h"
#include "mcumap.h"
#include "mcu.h"
#include "grbl_interface.h"
#include "serial.h"
#include "protocol.h"
#include "parser.h"
#include "kinematics.h"
#include "motion_control.h"
#include "planner.h"
#include "interpolator.h"
#include "io_control.h"
#include "cnc.h"

typedef struct
{
    //uint8_t system_state;		//signals if CNC is system_state and gcode can run
    volatile uint8_t exec_state;
    uint8_t active_alarm;
    volatile uint8_t rt_cmd;
} cnc_state_t;

static cnc_state_t cnc_state;

static void cnc_check_fault_systems();
static bool cnc_check_interlocking();
static void cnc_exec_rt_command(uint8_t command);
static void cnc_reset();

void cnc_init()
{
	//initializes cnc state
	#ifdef FORCE_GLOBALS_TO_0
	memset(&cnc_state, 0, sizeof(cnc_state_t));
	#endif
	
	//initializes all systems
	mcu_init();		//mcu
	serial_init();	//serial
	settings_init();//settings
	parser_init();	//parser
	mc_init();		//motion control
	planner_init();	//motion planner
	itp_init();		//interpolator
	
	serial_flush();
}

void cnc_run()
{
	cnc_reset();
	
	do
	{
		//process gcode commands
		if(!serial_rx_is_empty())
		{
			uint8_t error = 0;
			//protocol_echo();
			uint8_t c = serial_peek();
			switch(c)
			{
				case '\n':
					serial_getc();
					break;
				case '$':
					serial_getc();
					error = parser_grbl_command();
					break;
				default:
					if(!cnc_get_exec_state(EXEC_LOCKED))
					{
						error = parser_gcode_command();
					}
					else
					{
						error = STATUS_SYSTEM_GC_LOCK;
					}
					break;
			}
			
			if(!error)
			{
				protocol_send_ok();
			}
			else
			{
				protocol_send_error(error);
				serial_discard_cmd();//flushes the rest of the command
			}
		}
		
		cnc_doevents();
	}while(!cnc_get_exec_state(EXEC_ABORT)); //while abort is not issued
	
	serial_flush();
	cnc_clear_exec_state(EXEC_ABORT); //clears the abort flag
	if(cnc_get_exec_state(EXEC_ALARM_ABORT))//checks if any alarm is active (except NOHOME - ignore it)
	{
		cnc_check_fault_systems();
		protocol_send_string(MSG_FEEDBACK_1);
		do
		{
		}while(cnc_state.rt_cmd != RT_CMD_RESET);
	}
}

void cnc_call_rt_command(uint8_t command)
{
	if(!cnc_state.rt_cmd || cnc_state.rt_cmd == RT_CMD_REPORT)
	{
		cnc_state.rt_cmd = command;
	}
}

void cnc_doevents()
{
	//check if RT commands are pending execution
	if(cnc_state.rt_cmd)
	{
		cnc_exec_rt_command(cnc_state.rt_cmd);
		cnc_state.rt_cmd = 0; //clears the rt_cmd for the next instruction
	}
	
	if(!cnc_check_interlocking())
	{
		return;
	}	

	itp_run();
}

void cnc_home()
{
	cnc_set_exec_state(EXEC_HOMING);
	uint8_t error = kinematics_home();
	if(error)
	{
		//disables homing and reenables alarm messages
		cnc_clear_exec_state(EXEC_HOMING);
		cnc_alarm(error);
		return;
	}
	
	//unlocks the machine to go to offset
	cnc_unlock();

	float target[AXIS_COUNT];
	planner_block_data_t block_data;
	planner_get_position(target);
	
	for(uint8_t i = AXIS_COUNT; i != 0;)
	{
		i--;
		target[i] += ((g_settings.homing_dir_invert_mask & (1<<i)) ? -g_settings.homing_offset : g_settings.homing_offset);
	}
	
	block_data.feed = g_settings.homing_fast_feed_rate * MIN_SEC_MULT;
	block_data.spindle = 0;
	block_data.dwell = 0;
	//starts offset and waits to finnish
	planner_add_line((float*)&target, block_data);
	do{
		cnc_doevents();
	} while(cnc_get_exec_state(EXEC_RUN));

	//reset position
	itp_reset_rt_position();
	planner_resync_position();
}

void cnc_alarm(uint8_t code)
{
	cnc_set_exec_state(EXEC_ABORT);
	cnc_state.active_alarm = code;
}

void cnc_stop()
{
	//halt is active and was running flags it lost home position
	if(cnc_get_exec_state(EXEC_RUN) && g_settings.homing_enabled)
	{
		cnc_set_exec_state(EXEC_NOHOME);
	}
	itp_stop();
	//stop tools
	#ifdef USE_SPINDLE
	io_set_pwm(SPINDLE_PWM_CHANNEL, 0);
	io_clear_outputs(SPINDLE_DIR);
	#endif
	#ifdef USE_COOLANT
	io_clear_outputs(COOLANT_FLOOD | COOLANT_MIST);
	#endif
}

void cnc_unlock()
{
	//on unlock any alarm caused by not having homing reference or hitting a limit switch is reset at user request
	//all other alarm flags remain active if any input is still active
	CLEARFLAG(cnc_state.exec_state,EXEC_NOHOME | EXEC_LIMITS);
	//clears all other locking flags
	cnc_clear_exec_state(EXEC_LOCKED);
}

uint8_t cnc_get_exec_state(uint8_t statemask)
{
	return CHECKFLAG(cnc_state.exec_state, statemask);
}

void cnc_set_exec_state(uint8_t statemask)
{
	SETFLAG(cnc_state.exec_state,statemask);
}

void cnc_clear_exec_state(uint8_t statemask)
{
	#ifdef ESTOP
	if(io_get_controls(ESTOP_MASK)) //can't clear the alarm flag if ESTOP is active
	{
		CLEARFLAG(statemask,EXEC_ABORT);
	}
	#endif
	#ifdef SAFETY_DOOR
	if(io_get_controls(SAFETY_DOOR_MASK)) //can't clear the door flag if SAFETY_DOOR is active
	{
		CLEARFLAG(statemask,EXEC_DOOR);
	}
	#endif
	#ifdef FHOLD
	if(io_get_controls(FHOLD_MASK)) //can't clear the hold flag if FHOLD is active
	{
		CLEARFLAG(statemask,EXEC_HOLD);
	}
	#endif
	#if(LIMITS_MASK!=0)
	if(g_settings.hard_limits_enabled && io_get_controls(LIMITS_MASK)) //can't clear the EXEC_LIMITS is any limit is triggered
	{
		CLEARFLAG(statemask,EXEC_LIMITS);
	}
	#endif
	if(g_settings.homing_enabled) //if the machine doesn't know the homing position and homing is enabled
	{
		CLEARFLAG(statemask,EXEC_NOHOME);
	}

	CLEARFLAG(cnc_state.exec_state,statemask);
}

void cnc_reset()
{
	cnc_state.rt_cmd = 0;
	cnc_state.active_alarm = EXEC_ALARM_RESET;
	cnc_state.exec_state = EXEC_ALARM | EXEC_HOLD; //Activates all alarms and hold
	
	//clear all systems
	itp_clear();
	planner_clear();
	serial_clear();		
	protocol_send_string(MSG_STARTUP);
	//tries to clear alarms or active hold state
	cnc_clear_exec_state(EXEC_ALARM | EXEC_HOLD);
	
	//if any alarm state is still active checks system faults
	if(cnc_get_exec_state(EXEC_ALARM))
	{
		cnc_check_fault_systems();
		if(!cnc_get_exec_state(EXEC_ABORT))
		{
			protocol_send_string(MSG_FEEDBACK_2);
		}
	} 
	/*else
	{
		//if all ok warns user that machine is unlocked and ready to use
		protocol_send_string(MSG_FEEDBACK_3);
	}*/
}

void cnc_exec_rt_command(uint8_t command)
{
	switch(command)
	{
		case RT_CMD_REPORT:
			protocol_send_status();
			break;
		case RT_CMD_RESET:
			cnc_stop();
			cnc_alarm(EXEC_ALARM_RESET);
			break;
		case RT_CMD_SAFETY_DOOR:
			cnc_set_exec_state(EXEC_DOOR|EXEC_HOLD);
			protocol_send_string(MSG_FEEDBACK_6);
			break;
		case RT_CMD_JOG_CANCEL:
		case RT_CMD_FEED_HOLD:
			if(!cnc_get_exec_state(EXEC_HOMING)) //if in homming ignores
			{
				cnc_set_exec_state(EXEC_HOLD); //activates hold
			}
			break;
		case RT_CMD_CYCLE_START:
			if(cnc_get_exec_state(EXEC_ALARM))
			{
				return;
			}
			
			//clears active hold
			#ifdef USE_SPINDLE
			planner_update_spindle(true);
			protocol_send_string(MSG_FEEDBACK_10);
			#endif
			itp_delay(DELAY_ON_RESUME*100);
			cnc_clear_exec_state(EXEC_HOLD);
			break;
		case RT_CMD_FEED_100:
			planner_feed_ovr_reset();
			break;
		case RT_CMD_FEED_INC_COARSE:
			planner_feed_ovr_inc(FEED_OVR_COARSE);
			break;
		case RT_CMD_FEED_DEC_COARSE:
			planner_feed_ovr_inc(-FEED_OVR_COARSE);
			break;
		case RT_CMD_FEED_INC_FINE:
			planner_feed_ovr_inc(FEED_OVR_FINE);
			break;
		case RT_CMD_FEED_DEC_FINE:
			planner_feed_ovr_inc(-FEED_OVR_FINE);
			break;
		case RT_CMD_RAPIDFEED_100:
			planner_rapid_feed_ovr_reset();
			break;
		case RT_CMD_RAPIDFEED_OVR1:
			planner_rapid_feed_ovr(RAPID_FEED_OVR1);
			break;
		case RT_CMD_RAPIDFEED_OVR2:
			planner_rapid_feed_ovr(RAPID_FEED_OVR2);
			break;
		#ifdef USE_SPINDLE
		case RT_CMD_SPINDLE_100:
			planner_spindle_ovr_reset();
			break;
		case RT_CMD_SPINDLE_INC_COARSE:
			planner_spindle_ovr_inc(SPINDLE_OVR_COARSE);
			break;
		case RT_CMD_SPINDLE_DEC_COARSE:
			planner_spindle_ovr_inc(-SPINDLE_OVR_COARSE);
			break;
		case RT_CMD_SPINDLE_INC_FINE:
			planner_spindle_ovr_inc(SPINDLE_OVR_FINE);
			break;
		case RT_CMD_SPINDLE_DEC_FINE:
			planner_spindle_ovr_inc(-SPINDLE_OVR_FINE);
			break;
		case RT_CMD_SPINDLE_TOGGLE:
			if(cnc_get_exec_state(EXEC_HOLD))
			{
				//toogle state
				if(io_get_pwm(SPINDLE_PWM_CHANNEL))
				{
					io_set_pwm(SPINDLE_PWM_CHANNEL, 0);
				}
				else
				{
					protocol_send_string(MSG_FEEDBACK_10);
					planner_update_spindle(true);
				}
			}
			break;
		#endif
		#ifdef USE_COOLANT
		case RT_CMD_COOL_FLD_TOGGLE:
		case RT_CMD_COOL_MST_TOGGLE:
			if(!cnc_get_exec_state(EXEC_ALARM)) //if no alarm is active
			{
				parser_toogle_coolant(command - (RT_CMD_COOL_FLD_TOGGLE - 1));
			}
			break;
		#endif
	}
	
	#ifdef USE_SPINDLE
	if(command>=RT_CMD_SPINDLE_100 && command<=RT_CMD_SPINDLE_DEC_FINE)
	{
		planner_update_spindle(true);
	}
	#endif
}

void cnc_check_fault_systems()
{
	#ifdef ESTOP
	if(io_get_controls(ESTOP_MASK)) //fault on emergency stop
	{
		protocol_send_string(MSG_FEEDBACK_12);
	}
	#endif
	#ifdef SAFETY_DOOR
	if(io_get_controls(SAFETY_DOOR_MASK)) //fault on safety door
	{
		protocol_send_string(MSG_FEEDBACK_6);
	}
	#endif
	#if(LIMITS_MASK != 0)
	if(g_settings.hard_limits_enabled) //fault on limits
	{
		if(io_get_limits(LIMITS_MASK))
		{
			protocol_send_string(MSG_FEEDBACK_7);
		}
	}
	#endif
}

bool cnc_check_interlocking()
{
	//if abort is flagged
	if(CHECKFLAG(cnc_state.exec_state, EXEC_ABORT))
	{
		if(cnc_state.active_alarm) //active alarm message
		{
			protocol_send_alarm(cnc_state.active_alarm);
			cnc_state.active_alarm = 0;
			return false;
		}
		return false;
	}
	
	if(CHECKFLAG(cnc_state.exec_state, EXEC_DOOR | EXEC_HOLD))
	{
		if(CHECKFLAG(cnc_state.exec_state, EXEC_RUN))
		{
			return true;
		}
		
		itp_stop();
		if(CHECKFLAG(cnc_state.exec_state, EXEC_DOOR))
		{
			cnc_stop(); //stop all tools not only motion
		}
		
		if(CHECKFLAG(cnc_state.exec_state, EXEC_HOMING) && CHECKFLAG(cnc_state.exec_state, EXEC_DOOR)) //door opened during a homing cycle
		{
			cnc_alarm(EXEC_ALARM_HOMING_FAIL_DOOR);
		}
		
		if(CHECKFLAG(cnc_state.exec_state, EXEC_HOMING | EXEC_JOG)) //flushes the buffers if motions was homing or jog
		{
			itp_clear();
			planner_clear();
			CLEARFLAG(cnc_state.exec_state, EXEC_HOMING | EXEC_JOG | EXEC_HOLD);
		}
		
		return false;
	}
	
	if(CHECKFLAG(cnc_state.exec_state, EXEC_LIMITS))
	{
		if(!CHECKFLAG(cnc_state.exec_state, EXEC_HOMING)) //door opened during a homing cycle
		{
			cnc_alarm(EXEC_ALARM_HARD_LIMIT);
		}

		return false;
	}
	
	return true;
}
