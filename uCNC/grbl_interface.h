/*
	Name: grbl_interface.h
	Description: uCNC definitions and standard codes used by Grbl.
	
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 07/12/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#ifndef GRBL_INTERFACE_H
#define GRBL_INTERFACE_H

#include "mcudefs.h"

/*
	Grbl error codes 
*/

// Define Grbl status codes. Valid values (0-255)
#define STATUS_OK 0
#define STATUS_EXPECTED_COMMAND_LETTER 1
#define STATUS_BAD_NUMBER_FORMAT 2
#define STATUS_INVALID_STATEMENT 3
#define STATUS_NEGATIVE_VALUE 4
#define STATUS_SETTING_DISABLED 5
#define STATUS_SETTING_STEP_PULSE_MIN 6 //
#define STATUS_SETTING_READ_FAIL 7
#define STATUS_IDLE_ERROR 8
#define STATUS_SYSTEM_GC_LOCK 9
#define STATUS_SOFT_LIMIT_ERROR 10
#define STATUS_OVERFLOW 11 //
#define STATUS_MAX_STEP_RATE_EXCEEDED 12
#define STATUS_CHECK_DOOR 13
#define STATUS_LINE_LENGTH_EXCEEDED 14 //
#define STATUS_TRAVEL_EXCEEDED 15
#define STATUS_INVALID_JOG_COMMAND 16
#define STATUS_SETTING_DISABLED_LASER 17 //
#define STATUS_GCODE_UNSUPPORTED_COMMAND 20
#define STATUS_GCODE_MODAL_GROUP_VIOLATION 21
#define STATUS_GCODE_UNDEFINED_FEED_RATE 22
#define STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER 23
#define STATUS_GCODE_AXIS_COMMAND_CONFLICT 24 //
#define STATUS_GCODE_WORD_REPEATED 25
#define STATUS_GCODE_NO_AXIS_WORDS 26
#define STATUS_GCODE_INVALID_LINE_NUMBER 27
#define STATUS_GCODE_VALUE_WORD_MISSING 28
#define STATUS_GCODE_UNSUPPORTED_COORD_SYS 29
#define STATUS_GCODE_G53_INVALID_MOTION_MODE 30
#define STATUS_GCODE_AXIS_WORDS_EXIST 31
#define STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE 32
#define STATUS_GCODE_INVALID_TARGET 33
#define STATUS_GCODE_ARC_RADIUS_ERROR 34
#define STATUS_GCODE_NO_OFFSETS_IN_PLANE 35
#define STATUS_GCODE_UNUSED_WORDS 36 //
#define STATUS_GCODE_G43_DYNAMIC_AXIS_ERROR 37 //
#define STATUS_GCODE_MAX_VALUE_EXCEEDED 38 //

//additional codes
#define STATUS_BAD_COMMENT_FORMAT 39
#define STATUS_INVALID_TOOL 40
#define STATUS_UNDEFINED_AXIS 41
#define STATUS_FEED_NOT_SET 42

#define EXEC_ALARM_RESET					  0
// Grbl alarm codes. Valid values (1-255). Zero is reserved.
#define EXEC_ALARM_HARD_LIMIT                 1
#define EXEC_ALARM_SOFT_LIMIT                 2
#define EXEC_ALARM_ABORT_CYCLE                3
#define EXEC_ALARM_PROBE_FAIL_INITIAL         4
#define EXEC_ALARM_PROBE_FAIL_CONTACT         5
#define EXEC_ALARM_HOMING_FAIL_RESET          6
#define EXEC_ALARM_HOMING_FAIL_DOOR           7
#define EXEC_ALARM_HOMING_FAIL_PULLOFF        8
#define EXEC_ALARM_HOMING_FAIL_APPROACH       9
#define EXEC_ALARM_HOMING_FAIL_DUAL_APPROACH  10
#define EXEC_ALARM_HOMING_FAIL_LIMIT_ACTIVE   11

//formated messages
#define MSG_OK __romstr__("ok\r\n")
#define MSG_ERROR __romstr__("error:")
#define MSG_ALARM __romstr__("ALARM:")
#define MSG_ECHO __romstr__("[echo:")
#define MSG_STARTUP __romstr__("uCNC 0.01a ['$' for help]\r\n")
#define MSG_HELP __romstr__("[HLP:$$ $# $G $I $N $x=val $Nx=line $J=line $C $X $H ~ ! ? ctrl-x]\r\n")

//Non query feedback messages
#define MSG_FEEDBACK_1 __romstr__("[MSG:Reset to continue]\r\n")
#define MSG_FEEDBACK_2 __romstr__("[MSG:'$H'|'$X' to unlock]\r\n")
#define MSG_FEEDBACK_3 __romstr__("[MSG:Caution: Unlocked]\r\n")
#define MSG_FEEDBACK_4 __romstr__("[MSG:Enabled]\r\n")
#define MSG_FEEDBACK_5 __romstr__("[MSG:Disabled]\r\n")
#define MSG_FEEDBACK_6 __romstr__("[MSG:Check Door]\r\n")
#define MSG_FEEDBACK_7 __romstr__("[MSG:Check Limits]\r\n")
#define MSG_FEEDBACK_8 __romstr__("[MSG:Pgm End]\r\n")
#define MSG_FEEDBACK_9 __romstr__("[MSG:Restoring defaults]\r\n")
#define MSG_FEEDBACK_10 __romstr__("[MSG:Restoring spindle]\r\n")
#define MSG_FEEDBACK_11 __romstr__("[MSG:Sleeping]\r\n")
/*NEW*/
#define MSG_FEEDBACK_12 __romstr__("[MSG:Check Emergency Stop]\r\n")

//#define MSG_INT "%d"
//#define MSG_FLT "%0.3f"
//#define MSG_FLT_IMPERIAL "%0.5f"

#endif
