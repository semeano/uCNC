/*
	Name: planner.c
	Description: Chain planner for linear motions and acceleration/deacceleration profiles.
        It uses a similar algorithm to Grbl.
			
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 24/09/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "config.h"
#include "grbl_interface.h"
#include "mcumap.h"
#include "mcu.h"
#include "settings.h"
#include "planner.h"
#include "interpolator.h"
#include "utils.h"
#include "io_control.h"
#include "cnc.h"

typedef struct
{
	uint8_t feed_override;
	uint8_t rapid_feed_override;
	#ifdef USE_SPINDLE
	uint8_t spindle_override;
	#endif
	bool overrides_enabled;

} planner_overrides_t;

static float planner_coord[AXIS_COUNT];
#ifdef USE_SPINDLE
static float planner_spindle;
#endif
static planner_block_t planner_data[PLANNER_BUFFER_SIZE];
static uint8_t planner_data_write;
static uint8_t planner_data_read;
static uint8_t planner_data_slots;
static planner_overrides_t planner_overrides;
static uint8_t planner_ovr_counter;

/*
	Planner buffer functions
*/
static inline void planner_buffer_read()
{
	planner_data_read++;
	planner_data_slots++;
	if (planner_data_read == PLANNER_BUFFER_SIZE)
	{
		planner_data_read = 0;
	}
}

static inline void planner_buffer_write()
{
	planner_data_write++;
	planner_data_slots--;
	if (planner_data_write == PLANNER_BUFFER_SIZE)
	{
		planner_data_write = 0;
	}
}

static inline uint8_t planner_buffer_next(uint8_t index)
{
	index++;
	if (index == PLANNER_BUFFER_SIZE)
	{
		index = 0;
	}

	return index;
}

static inline uint8_t planner_buffer_prev(uint8_t index)
{
	if (index == 0)
	{
		index = PLANNER_BUFFER_SIZE;
	}

	index--;
	return index;
}

bool planner_buffer_is_empty()
{
	return (planner_data_slots == PLANNER_BUFFER_SIZE);
}

bool planner_buffer_is_full()
{
	return (planner_data_slots == 0);
}

static inline void planner_buffer_clear()
{
	planner_data_write = 0;
	planner_data_read = 0;
	planner_data_slots = PLANNER_BUFFER_SIZE;
	#ifdef FORCE_GLOBALS_TO_0
	memset(planner_data, 0, sizeof(planner_data));
	#endif
}

void planner_init()
{
	#ifdef FORCE_GLOBALS_TO_0
	memset(&planner_coord, 0, AXIS_COUNT * sizeof(float));
	//resets buffer
	memset(&planner_data, 0, sizeof(planner_data));
	#endif
	planner_buffer_clear();
	planner_overrides.overrides_enabled = true;
	planner_overrides.feed_override = 100;
	planner_overrides.rapid_feed_override = 100;
	#ifdef USE_SPINDLE
	planner_overrides.spindle_override = 100;
	planner_spindle = 0;
	#endif
}

void planner_clear()
{
	//clears all motions stored in the buffer
	planner_buffer_clear();
	#ifdef USE_SPINDLE
	planner_spindle = 0;
	#endif
	//resyncs position with interpolator
	planner_resync_position();
}

planner_block_t *planner_get_block()
{
	return &planner_data[planner_data_read];
}

float planner_get_block_exit_speed_sqr()
{
	//only one block in the buffer (exit speed is 0)
	if (planner_data_slots >= (PLANNER_BUFFER_SIZE - 1))
		return 0;

	//exit speed = next block entry speed
	uint8_t next = planner_buffer_next(planner_data_read);
	float exit_speed_sqr = planner_data[next].entry_feed_sqr;
	if (!planner_overrides.overrides_enabled)
	{
		return exit_speed_sqr;
	}

	if (planner_overrides.feed_override != 100)
	{
		exit_speed_sqr *= planner_overrides.feed_override * planner_overrides.feed_override;
		exit_speed_sqr *= 0.0001f;
	}

	//if rapid overrides are active the feed must not exceed the rapid motion feed
	if (planner_overrides.rapid_feed_override != 100)
	{
		float rapid_feed_sqr = planner_data[next].rapid_feed_sqr;
		rapid_feed_sqr *= planner_overrides.rapid_feed_override * planner_overrides.rapid_feed_override;
		rapid_feed_sqr *= 0.0001f;
		exit_speed_sqr = MIN(exit_speed_sqr, rapid_feed_sqr);
	}

	return exit_speed_sqr;
}

float planner_get_block_top_speed()
{
	/*
	Computed the junction speed
	
	At full acceleration and deacceleration we have the following equations
		v_max^2 = v_entry^2 + 2 * distance * acceleration
		v_max^2 = v_exit^2 + 2 * distance * acceleration
		
	In this case v_max^2 for acceleration and deacceleration will be the same at
	
	d_deaccel = d_total - d_start;
	
	this translates to the equation
	
	v_max = v_entry + (2 * acceleration * distance + v_exit - v_entry)/acceleration
	*/
	float exit_speed_sqr = planner_get_block_exit_speed_sqr();
	float speed_delta = exit_speed_sqr - planner_data[planner_data_read].entry_feed_sqr;
	float speed_change = 2 * planner_data[planner_data_read].acceleration * planner_data[planner_data_read].distance;
	speed_change += speed_delta;
	speed_change *= planner_data[planner_data_read].accel_inv;
	float junction_speed_sqr = planner_data[planner_data_read].entry_feed_sqr + speed_change;

	float target_speed_sqr = planner_data[planner_data_read].feed_sqr;
	if (planner_overrides.overrides_enabled)
	{
		if (planner_overrides.feed_override != 100)
		{
			target_speed_sqr *= planner_overrides.feed_override * planner_overrides.feed_override;
			target_speed_sqr *= 0.0001f;
		}

		float rapid_feed_sqr = planner_data[planner_data_read].rapid_feed_sqr;
		//if rapid overrides are active the feed must not exceed the rapid motion feed
		if (planner_overrides.rapid_feed_override != 100)
		{
			rapid_feed_sqr *= planner_overrides.rapid_feed_override * planner_overrides.rapid_feed_override;
			rapid_feed_sqr *= 0.0001f;
		}

		//can't ever exceed rapid move speed
		target_speed_sqr = MIN(target_speed_sqr, rapid_feed_sqr);
	}

	return MIN(junction_speed_sqr, target_speed_sqr);
}

#ifdef USE_SPINDLE
float planner_update_spindle(bool update_outputs)
{
	float spindle = (planner_data_slots == PLANNER_BUFFER_SIZE) ? planner_spindle : planner_data[planner_data_read].spindle;

	if(update_outputs)
	{
		if (spindle >= 0)
		{
			io_clear_outputs(SPINDLE_DIR);
		}
		else
		{
			io_set_outputs(SPINDLE_DIR);
		}
	}
	
	uint8_t pwm = 0;
	if (spindle != 0)
	{
		spindle = ABS(spindle);
		if (planner_overrides.overrides_enabled && planner_overrides.spindle_override != 100)
		{
			spindle *= 0.01f * planner_overrides.spindle_override;
		}
		spindle = MIN(spindle, g_settings.spindle_max_rpm);
		spindle = MAX(spindle, g_settings.spindle_min_rpm);
		pwm = (uint8_t)roundf(255 * (spindle / g_settings.spindle_max_rpm));
		pwm = MAX(pwm, 1);
	}
	
	if(update_outputs)
	{	
		io_set_pwm(SPINDLE_PWM_CHANNEL, pwm);
	}

	return spindle;
}
#endif

void planner_discard_block()
{
	planner_buffer_read();
}

void planner_recalculate()
{
	uint8_t last = planner_data_write;
	uint8_t first = planner_data_read;
	uint8_t block = planner_data_write;
	//starts in the last added block
	//calculates the maximum entry speed of the block so that it can do a full stop in the end
	float entry_feed_sqr = 2 * planner_data[block].distance * planner_data[block].acceleration;
	planner_data[block].entry_feed_sqr = MIN(planner_data[block].entry_max_feed_sqr, entry_feed_sqr);
	//optimizes entry speeds given the current exit speed (backward pass)
	uint8_t next = block;
	block = planner_buffer_prev(block);

	while (!planner_data[block].optimal && block != first)
	{
		if (planner_data[block].entry_feed_sqr != planner_data[block].entry_max_feed_sqr)
		{
			entry_feed_sqr = planner_data[next].entry_feed_sqr + 2 * planner_data[block].distance * planner_data[block].acceleration;
			planner_data[block].entry_feed_sqr = MIN(planner_data[block].entry_max_feed_sqr, entry_feed_sqr);
		}

		next = block;
		block = planner_buffer_prev(block);
	}

	//optimizes exit speeds (forward pass)
	while (block != last)
	{
		//next block is moving at a faster speed
		if (planner_data[block].entry_feed_sqr < planner_data[next].entry_feed_sqr)
		{
			//check if the next block entry speed can be achieved
			float exit_speed_sqr = planner_data[block].entry_feed_sqr + (2 * planner_data[block].distance * planner_data[block].acceleration);
			if (exit_speed_sqr < planner_data[next].entry_feed_sqr)
			{
				//lowers next entry speed (aka exit speed) to the maximum reachable speed from current block
				//optimization achieved for this movement
				planner_data[next].entry_feed_sqr = exit_speed_sqr;
				planner_data[next].optimal = true;
			}
		}

		//if the executing block was updated then update the interpolator limits
		if (block == first)
		{
			itp_update();
		}

		block = next;
		next = planner_buffer_next(block);
	}
}

/*
	Adds a new line to the trajectory planner
	The planner is responsible for calculating the entry and exit speeds of the transitions
	The trajectory planner does the following actions:
		1. Calculates the direction change of the new movement
		2. Adjusts maximum entry feed according to the angle of the junction point
		3. Recalculates all chained segments
	
	For profiling the motion 4 feeds are calculated
		1. The target feed
		2. The rapid motion feed given the direction (maximum allowed feed with overrides)
		3. The entry feed (initialy set to 0)
		4. The maximum entry feed given the juntion angle between planner blocks
*/
void planner_add_line(float *target, planner_block_data_t block_data)
{
	static float prev_dir_vect[AXIS_COUNT];
	planner_data[planner_data_write].dirbits = 0;
	planner_data[planner_data_write].optimal = false;
	planner_data[planner_data_write].acceleration = 0;
	planner_data[planner_data_write].rapid_feed_sqr = 0;
	planner_data[planner_data_write].feed_sqr = 0;
	planner_data[planner_data_write].entry_feed_sqr = 0;
	planner_data[planner_data_write].entry_max_feed_sqr = 0;
	#ifdef USE_SPINDLE
	planner_spindle = planner_data[planner_data_write].spindle = block_data.spindle;
	#endif
	planner_data[planner_data_write].dwell = block_data.dwell;

	planner_data[planner_data_write].distance = block_data.distance;
	if(block_data.motion_mode == PLANNER_MOTION_MODE_NOMOTION)
	{
		planner_buffer_write();
		return;
	}

	memcpy(&(planner_data[planner_data_write].pos), target, sizeof(planner_data[planner_data_write].pos));

	//calculates the normalized direction vector
	//it also calculates the angle between previous direction and the current
	//this is given by the equation cos(theta) = dotprod(u,v)/(magnitude(u)*magnitude(v))
	//since normalized vector are being used (magnitude=1) this simplifies to cos(theta) = dotprod(u,v)
	//in the same loop the maximum linear speed and accel is calculated
	//planner_data[planner_data_write].distance = sqrtf(planner_data[planner_data_write].distance);
	float inv_magn = 1.0f / planner_data[planner_data_write].distance;
	float cos_theta = 0;
	uint8_t prev = 0;
	
	if (!planner_buffer_is_empty())
	{
		prev = planner_buffer_prev(planner_data_write); //BUFFER_PTR(planner_buffer, prev_index);
	}

	//calculates (given the motion direction), the maximum acceleration an feed allowed by the machine settings.
	float rapid_feed = FLT_MAX;
	planner_data[planner_data_write].acceleration = FLT_MAX;
	for (uint8_t i = AXIS_COUNT; i != 0; )
	{
		i--;
		//if target doesn't move skip computations
		if (block_data.dir_vect[i] != 0)
		{
			block_data.dir_vect[i] *= inv_magn;
			float dir_axis_abs = 1.0f/block_data.dir_vect[i];
			if (block_data.dir_vect[i] < 0) //sets direction bits
			{
				SETBIT(planner_data[planner_data_write].dirbits, i);
				dir_axis_abs = -dir_axis_abs;
			}
			
			if (!planner_buffer_is_empty())
			{
				cos_theta += block_data.dir_vect[i] * prev_dir_vect[i];
			}

			//calcs maximum allowable speed for this diretion
			float axis_speed = g_settings.max_feed_rate[i] * dir_axis_abs;
			rapid_feed = MIN(rapid_feed, axis_speed);
			//calcs maximum allowable acceleration for this direction
			float axis_accel = g_settings.acceleration[i] * dir_axis_abs;
			planner_data[planner_data_write].acceleration = MIN(planner_data[planner_data_write].acceleration, axis_accel);
		}

	}

	planner_data[planner_data_write].accel_inv = 1.0f / planner_data[planner_data_write].acceleration;
	//reduces target speed if exceeds the maximum allowed speed in the current direction
	rapid_feed *= MIN_SEC_MULT; //converto to mm/s
	if (block_data.feed > rapid_feed)
	{
		block_data.feed = rapid_feed;
	}

	//sets entry and max entry feeds as if it would start and finish from a stoped state
	planner_data[planner_data_write].entry_feed_sqr = 0;
	planner_data[planner_data_write].feed_sqr = (block_data.feed * block_data.feed);
	planner_data[planner_data_write].entry_max_feed_sqr = planner_data[planner_data_write].feed_sqr;
	planner_data[planner_data_write].rapid_feed_sqr = rapid_feed * rapid_feed;

	//if more than one move stored cals juntion speeds and recalculates speed profiles
	if (!planner_buffer_is_empty())
	{
		//calculates the junction angle with previous
		if (cos_theta > 0)
		{
			//uses the half angle identity conversion to convert from cos(theta) to tan(theta/2) where:
			//	tan(theta/2) = sqrt((1-cos(theta)/(1+cos(theta))
			//to simplify the calculations it multiplies by sqrt((1+cos(theta)/(1+cos(theta))
			//transforming the equation to sqrt((1^2-cos(theta)^2))/(1+cos(theta))
			//this way the output will be between 0<tan(theta/2)<inf
			//but if theta is 0<theta<90 the tan(theta/2) will be 0<tan(theta/2)<1
			//all angles greater than 1 that can be excluded
			planner_data[planner_data_write].angle_factor = 1.0f / (1.0f + cos_theta);
			cos_theta = (1.0f - cos_theta * cos_theta);
			planner_data[planner_data_write].angle_factor *= fast_sqrt(cos_theta);
		}

		//sets the maximum allowed speed at junction (if angle doesn't force a full stop)
		if (planner_data[planner_data_write].angle_factor < 1.0f)
		{
			float junc_feed_sqr = (1 - planner_data[planner_data_write].angle_factor);
			junc_feed_sqr *= junc_feed_sqr;
			junc_feed_sqr *= planner_data[prev].feed_sqr;
			//the maximum feed is the minimal feed between the previous feed given the angle and the current feed
			planner_data[planner_data_write].entry_max_feed_sqr = MIN(planner_data[planner_data_write].feed_sqr, junc_feed_sqr);
		}

		//forces reaclculation with the new block
		planner_recalculate();
	}

	//advances the buffer
	planner_buffer_write();
	//updates the current planner coordinates
	memcpy(&planner_coord, target, sizeof(planner_coord));
	//updates the previous dir vect
	memcpy(&prev_dir_vect, block_data.dir_vect, sizeof(prev_dir_vect));
}

void planner_get_position(float *axis)
{
	memcpy(axis, planner_coord, sizeof(planner_coord));
}

void planner_resync_position()
{
	//resyncs the position with the interpolator
	itp_get_rt_position((float *)&planner_coord);
}

//overrides
void planner_toogle_overrides()
{
	planner_overrides.overrides_enabled = !planner_overrides.overrides_enabled;
	itp_update();
	planner_ovr_counter = 0;
}

bool planner_get_overrides()
{
	return planner_overrides.overrides_enabled;
}

void planner_feed_ovr_inc(float value)
{
	planner_overrides.feed_override += value;
	planner_overrides.feed_override = MAX(planner_overrides.feed_override, FEED_OVR_MIN);
	planner_overrides.feed_override = MIN(planner_overrides.feed_override, FEED_OVR_MAX);
	
	if (planner_overrides.overrides_enabled)
	{
		itp_update();
		planner_ovr_counter = 0;
	}
}

void planner_rapid_feed_ovr(float value)
{
	planner_overrides.rapid_feed_override = value;
	if (planner_overrides.overrides_enabled)
	{
		planner_ovr_counter = 0;
		itp_update();
	}
}

void planner_feed_ovr_reset()
{
	planner_overrides.feed_override = 100;
	planner_ovr_counter = 0;
}

void planner_rapid_feed_ovr_reset()
{
	planner_overrides.rapid_feed_override = 100;
	planner_ovr_counter = 0;
}
#ifdef USE_SPINDLE
void planner_spindle_ovr_inc(float value)
{
	planner_overrides.spindle_override += value;
	planner_overrides.spindle_override = MAX(planner_overrides.spindle_override, SPINDLE_OVR_MIN);
	planner_overrides.spindle_override = MIN(planner_overrides.spindle_override, SPINDLE_OVR_MAX);
	planner_ovr_counter = 0;
}

void planner_spindle_ovr_reset()
{
	planner_overrides.spindle_override = 100;
	planner_ovr_counter = 0;
}
#endif

bool planner_get_overflows(uint8_t *overflows)
{
	if(!planner_ovr_counter)
	{
		overflows[0] = planner_overrides.feed_override;
		overflows[1] = planner_overrides.rapid_feed_override;
		#ifdef USE_SPINDLE
		overflows[2] = planner_overrides.spindle_override;
		#else
		overflows[2] = 0;
		#endif
		planner_ovr_counter = STATUS_WCO_REPORT_MIN_FREQUENCY;
		return true;
	}
	
	planner_ovr_counter--;
	return false;
}

