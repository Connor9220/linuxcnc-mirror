
//
//    Copyright (C) 2007-2009 Sebastian Kuzminsky
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//

#include <linux/slab.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "hal/drivers/mesa-hostmot2/hostmot2.h"


#define f_period_s ((hal_float_t)(l_period_ns * 1e-9))




// 
// read accumulator to figure out where the stepper has gotten to
// 

void hm2_stepgen_process_tram_read(hostmot2_t *hm2, long l_period_ns) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        u32 acc = hm2->stepgen.accumulator_reg[i];
        s64 acc_delta;

        // those tricky users are always trying to get us to divide by zero
        if (fabs(hm2->stepgen.instance[i].hal.param.position_scale) < 1e-6) {
            if (hm2->stepgen.instance[i].hal.param.position_scale >= 0.0) {
                hm2->stepgen.instance[i].hal.param.position_scale = 1.0;
                HM2_ERR("stepgen %d position_scale is too close to 0, resetting to 1.0\n", i);
            } else {
                hm2->stepgen.instance[i].hal.param.position_scale = -1.0;
                HM2_ERR("stepgen %d position_scale is too close to 0, resetting to -1.0\n", i);
            }
        }

        // The HM2 Accumulator Register is a 16.16 bit fixed-point
        // representation of the current stepper position.
        // The fractional part gives accurate velocity at low speeds, and
        // sub-step position feedback (like sw stepgen).
        acc_delta = acc - hm2->stepgen.instance[i].prev_accumulator;
        if (acc_delta > INT32_MAX) {
            acc_delta -= UINT32_MAX;
        } else if (acc_delta < INT32_MIN) {
            acc_delta += UINT32_MAX;
        }

        hm2->stepgen.instance[i].subcounts += acc_delta;

        *(hm2->stepgen.instance[i].hal.pin.counts) = hm2->stepgen.instance[i].subcounts >> 16;

        *(hm2->stepgen.instance[i].hal.pin.position_fb) = ((hal_float_t)hm2->stepgen.instance[i].subcounts / 65536.0) / hm2->stepgen.instance[i].hal.param.position_scale;

        hm2->stepgen.instance[i].prev_accumulator = acc;
    }
}




//
// Here's the stepgen position controller.  It uses first-order
// feedforward and proportional error feedback.  This code is based
// on John Kasunich's software stepgen code.
//

static void hm2_stepgen_instance_position_control(hostmot2_t *hm2, long l_period_ns, int i, hal_float_t *new_vel) {
    hal_float_t ff_vel;
    hal_float_t velocity_error;
    hal_float_t match_accel;
    hal_float_t seconds_to_vel_match;
    hal_float_t position_at_match;
    hal_float_t position_cmd_at_match;
    hal_float_t error_at_match;
    hal_float_t velocity_cmd;

    hm2_stepgen_instance_t *s = &hm2->stepgen.instance[i];


    (*s->hal.pin.dbg_pos_minus_prev_cmd) = (*s->hal.pin.position_fb) - s->old_position_cmd;

    // calculate feed-forward velocity in machine units per second
    ff_vel = ((*s->hal.pin.position_cmd) - s->old_position_cmd) / f_period_s;
    (*s->hal.pin.dbg_ff_vel) = ff_vel;

    s->old_position_cmd = (*s->hal.pin.position_cmd);

    velocity_error = (*s->hal.pin.velocity_fb) - ff_vel;
    (*s->hal.pin.dbg_vel_error) = velocity_error;

    // Do we need to change speed to match the speed of position-cmd?
    // If maxaccel is 0, there's no accel limit: fix this velocity error
    // by the next servo period!  This leaves acceleration control up to
    // the trajectory planner.
    // If maxaccel is not zero, the user has specified a maxaccel and we
    // adhere to that.
    if (velocity_error > 0.0) {
        if (s->hal.param.maxaccel == 0) {
            match_accel = -velocity_error / f_period_s;
        } else {
            match_accel = -s->hal.param.maxaccel;
        }
    } else if (velocity_error < 0.0) {
        if (s->hal.param.maxaccel == 0) {
            match_accel = velocity_error / f_period_s;
        } else {
            match_accel = s->hal.param.maxaccel;
        }
    } else {
        match_accel = 0;
    }

    if (match_accel == 0) {
        // vel is just right, dont need to accelerate
        seconds_to_vel_match = 0.0;
    } else {
        seconds_to_vel_match = -velocity_error / match_accel;
    }
    *s->hal.pin.dbg_s_to_match = seconds_to_vel_match;

    // compute expected position at the time of velocity match
    // Note: this is "feedback position at the beginning of the servo period after we attain velocity match"
    {
        hal_float_t avg_v;
        avg_v = (ff_vel + *s->hal.pin.velocity_fb) * 0.5;
        position_at_match = *s->hal.pin.position_fb + (avg_v * (seconds_to_vel_match + f_period_s));
    }

    // Note: this assumes that position-cmd keeps the current velocity
    position_cmd_at_match = *s->hal.pin.position_cmd + (ff_vel * seconds_to_vel_match);
    error_at_match = position_at_match - position_cmd_at_match;

    *s->hal.pin.dbg_err_at_match = error_at_match;

    if (seconds_to_vel_match < f_period_s) {
        // we can match velocity in one period
        // try to correct whatever position error we have
        velocity_cmd = ff_vel - (0.5 * error_at_match / f_period_s);

        // apply accel limits?
        if (s->hal.param.maxaccel > 0) {
            if (velocity_cmd > (*s->hal.pin.velocity_fb + (s->hal.param.maxaccel * f_period_s))) {
                velocity_cmd = *s->hal.pin.velocity_fb + (s->hal.param.maxaccel * f_period_s);
            } else if (velocity_cmd < (*s->hal.pin.velocity_fb - (s->hal.param.maxaccel * f_period_s))) {
                velocity_cmd = *s->hal.pin.velocity_fb - (s->hal.param.maxaccel * f_period_s);
            }
        }

    } else {
        // we're going to have to work for more than one period to match velocity
        // FIXME: I dont really get this part yet

        hal_float_t dv;
        hal_float_t dp;

        /* calculate change in final position if we ramp in the opposite direction for one period */
        dv = -2.0 * match_accel * f_period_s;
        dp = dv * seconds_to_vel_match;

        /* decide which way to ramp */
        if (fabs(error_at_match + (dp * 2.0)) < fabs(error_at_match)) {
            match_accel = -match_accel;
        }

        /* and do it */
        velocity_cmd = *s->hal.pin.velocity_fb + (match_accel * f_period_s);
    }

    *new_vel = velocity_cmd;
}


static void hm2_stepgen_instance_prepare_tram_write(hostmot2_t *hm2, long l_period_ns, int i) {
    hal_float_t new_vel;

    hal_float_t physical_maxvel;  // max vel supported by current step timings & position-scale
    hal_float_t maxvel;           // actual max vel to use this time

    hal_float_t steps_per_sec_cmd;

    hm2_stepgen_instance_t *s = &hm2->stepgen.instance[i];


    //
    // first sanity-check our maxaccel and maxvel params
    //

    // maxvel must be >= 0.0, and may not be faster than 1 step per (steplen+stepspace) seconds
    {
        hal_float_t min_ns_per_step = s->hal.param.steplen + s->hal.param.stepspace;
        hal_float_t max_steps_per_s = 1.0e9 / min_ns_per_step;

        physical_maxvel = max_steps_per_s / fabs(s->hal.param.position_scale);

        if (s->hal.param.maxvel < 0.0) {
            HM2_ERR("stepgen.%02d.maxvel < 0, setting to its absolute value\n", i);
            s->hal.param.maxvel = fabs(s->hal.param.maxvel);
        }

        if (s->hal.param.maxvel > physical_maxvel) {
            HM2_ERR("stepgen.%02d.maxvel is too big for current step timings & position-scale, clipping to max possible\n", i);
            s->hal.param.maxvel = physical_maxvel;
        }

        if (s->hal.param.maxvel == 0.0) {
            maxvel = physical_maxvel;
        } else {
            maxvel = s->hal.param.maxvel;
        }
    }

    // maxaccel may not be negative
    if (s->hal.param.maxaccel < 0.0) {
        HM2_ERR("stepgen.%02d.maxaccel < 0, setting to its absolute value\n", i);
        s->hal.param.maxaccel = fabs(s->hal.param.maxaccel);
    }


    // select the new velocity we want
    if (*s->hal.pin.control_type == 0) {
        hm2_stepgen_instance_position_control(hm2, l_period_ns, i, &new_vel);
    } else {
        // velocity-mode control is easy
        new_vel = *s->hal.pin.velocity_cmd;
        if (s->hal.param.maxaccel > 0.0) {
            if (((new_vel - *s->hal.pin.velocity_fb) / f_period_s) > s->hal.param.maxaccel) {
                new_vel = (*s->hal.pin.velocity_fb) + (s->hal.param.maxaccel * f_period_s);
            } else if (((new_vel - *s->hal.pin.velocity_fb) / f_period_s) < -s->hal.param.maxaccel) {
                new_vel = (*s->hal.pin.velocity_fb) - (s->hal.param.maxaccel * f_period_s);
            }
        }
    }

    // clip velocity to maxvel
    if (new_vel > maxvel) {
        new_vel = maxvel;
    } else if (new_vel < -maxvel) {
        new_vel = -maxvel;
    }


    *s->hal.pin.velocity_fb = new_vel;

    steps_per_sec_cmd = new_vel * s->hal.param.position_scale;
    hm2->stepgen.step_rate_reg[i] = steps_per_sec_cmd * (4294967296.0 / (hal_float_t)hm2->stepgen.clock_frequency);
    *s->hal.pin.dbg_step_rate = hm2->stepgen.step_rate_reg[i];
}


void hm2_stepgen_prepare_tram_write(hostmot2_t *hm2, long l_period_ns) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        if (*(hm2->stepgen.instance[i].hal.pin.enable) == 0) {
            hm2->stepgen.step_rate_reg[i] = 0;
        } else {
            hm2_stepgen_instance_prepare_tram_write(hm2, l_period_ns, i);
        }
    }
}




static void hm2_stepgen_update_dir_setup_time(hostmot2_t *hm2, int i) {
    hm2->stepgen.dir_setup_time_reg[i] = (double)hm2->stepgen.instance[i].hal.param.dirsetup * ((double)hm2->stepgen.clock_frequency / (double)1e9);
    if (hm2->stepgen.dir_setup_time_reg[i] > 0x3FFF) {
        HM2_ERR("stepgen %d has invalid dirsetup, resetting to max\n", i);
        hm2->stepgen.dir_setup_time_reg[i] = 0x3FFF;
        hm2->stepgen.instance[i].hal.param.dirsetup = (double)hm2->stepgen.dir_setup_time_reg[i] * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
    }
    hm2->stepgen.instance[i].written_dirsetup = hm2->stepgen.instance[i].hal.param.dirsetup;
}


static void hm2_stepgen_update_dir_hold_time(hostmot2_t *hm2, int i) {
    hm2->stepgen.dir_hold_time_reg[i] = (double)hm2->stepgen.instance[i].hal.param.dirhold * ((double)hm2->stepgen.clock_frequency / (double)1e9);
    if (hm2->stepgen.dir_hold_time_reg[i] > 0x3FFF) {
        HM2_ERR("stepgen %d has invalid dirhold, resetting to max\n", i);
        hm2->stepgen.dir_hold_time_reg[i] = 0x3FFF;
        hm2->stepgen.instance[i].hal.param.dirhold = (double)hm2->stepgen.dir_hold_time_reg[i] * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
    }
    hm2->stepgen.instance[i].written_dirhold = hm2->stepgen.instance[i].hal.param.dirhold;
}


static void hm2_stepgen_update_pulse_idle_width(hostmot2_t *hm2, int i) {
    hm2->stepgen.pulse_idle_width_reg[i] = (double)hm2->stepgen.instance[i].hal.param.stepspace * ((double)hm2->stepgen.clock_frequency / (double)1e9);
    if (hm2->stepgen.pulse_idle_width_reg[i] > 0x3FFF) {
        HM2_ERR("stepgen %d has invalid stepspace, resetting to max\n", i);
        hm2->stepgen.pulse_idle_width_reg[i] = 0x3FFF;
        hm2->stepgen.instance[i].hal.param.stepspace = (double)hm2->stepgen.pulse_idle_width_reg[i] * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
    }
    hm2->stepgen.instance[i].written_stepspace = hm2->stepgen.instance[i].hal.param.stepspace;
}


static void hm2_stepgen_update_pulse_width(hostmot2_t *hm2, int i) {
    hm2->stepgen.pulse_width_reg[i] = (double)hm2->stepgen.instance[i].hal.param.steplen * ((double)hm2->stepgen.clock_frequency / (double)1e9);
    if (hm2->stepgen.pulse_width_reg[i] > 0x3FFF) {
        HM2_ERR("stepgen %d has invalid steplen, resetting to max\n", i);
        hm2->stepgen.pulse_width_reg[i] = 0x3FFF;
        hm2->stepgen.instance[i].hal.param.steplen = (double)hm2->stepgen.pulse_width_reg[i] * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
    }
    hm2->stepgen.instance[i].written_steplen = hm2->stepgen.instance[i].hal.param.steplen;
}


static void hm2_stepgen_update_mode(hostmot2_t *hm2, int i) {
    if (hm2->stepgen.instance[i].hal.param.step_type > 2) {
        HM2_ERR(
            "stepgen %d has invalid step_type %d, resetting to 0 (Step/Dir)\n",
            i,
            hm2->stepgen.instance[i].hal.param.step_type
        );
        hm2->stepgen.instance[i].hal.param.step_type = 0;
    }

    hm2->stepgen.mode_reg[i] = hm2->stepgen.instance[i].hal.param.step_type;
    hm2->stepgen.instance[i].written_step_type = hm2->stepgen.instance[i].hal.param.step_type;
}


void hm2_stepgen_write(hostmot2_t *hm2) {
    int i;

    // FIXME
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        if (hm2->stepgen.instance[i].hal.param.dirsetup != hm2->stepgen.instance[i].written_dirsetup) {
            hm2_stepgen_update_dir_setup_time(hm2, i);
            hm2->llio->write(hm2->llio, hm2->stepgen.dir_setup_time_addr + (i * sizeof(u32)), &hm2->stepgen.dir_setup_time_reg[i], sizeof(u32));
        }

        if (hm2->stepgen.instance[i].hal.param.dirhold != hm2->stepgen.instance[i].written_dirhold) {
            hm2_stepgen_update_dir_hold_time(hm2, i);
            hm2->llio->write(hm2->llio, hm2->stepgen.dir_hold_time_addr + (i * sizeof(u32)), &hm2->stepgen.dir_hold_time_reg[i], sizeof(u32));
        }

        if (hm2->stepgen.instance[i].hal.param.steplen != hm2->stepgen.instance[i].written_steplen) {
            hm2_stepgen_update_pulse_width(hm2, i);
            hm2->llio->write(hm2->llio, hm2->stepgen.pulse_width_addr + (i * sizeof(u32)), &hm2->stepgen.pulse_width_reg[i], sizeof(u32));
        }

        if (hm2->stepgen.instance[i].hal.param.stepspace != hm2->stepgen.instance[i].written_stepspace) {
            hm2_stepgen_update_pulse_idle_width(hm2, i);
            hm2->llio->write(hm2->llio, hm2->stepgen.pulse_idle_width_addr + (i * sizeof(u32)), &hm2->stepgen.pulse_idle_width_reg[i], sizeof(u32));
        }

        if (hm2->stepgen.instance[i].hal.param.step_type != hm2->stepgen.instance[i].written_stepspace) {
            hm2_stepgen_update_mode(hm2, i);
            hm2->llio->write(hm2->llio, hm2->stepgen.mode_addr + (i * sizeof(u32)), &hm2->stepgen.mode_reg[i], sizeof(u32));
        }
    }
}


static void hm2_stepgen_force_write_mode(hostmot2_t *hm2) {
    int size;

    if (hm2->stepgen.num_instances == 0) return;

    size = hm2->stepgen.num_instances * sizeof(u32);
    hm2->llio->write(hm2->llio, hm2->stepgen.mode_addr, hm2->stepgen.mode_reg, size);
}


static void hm2_stepgen_force_write_dir_setup_time(hostmot2_t *hm2) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        hm2_stepgen_update_dir_setup_time(hm2, i);
    }
    hm2->llio->write(
        hm2->llio,
        hm2->stepgen.dir_setup_time_addr,
        hm2->stepgen.dir_setup_time_reg,
        (hm2->stepgen.num_instances * sizeof(u32))
    );
    
}


static void hm2_stepgen_force_write_dir_hold_time(hostmot2_t *hm2) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        hm2_stepgen_update_dir_hold_time(hm2, i);
    }
    hm2->llio->write(
        hm2->llio,
        hm2->stepgen.dir_hold_time_addr,
        hm2->stepgen.dir_hold_time_reg,
        (hm2->stepgen.num_instances * sizeof(u32))
    );
}


static void hm2_stepgen_force_write_pulse_idle_width(hostmot2_t *hm2) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        hm2_stepgen_update_pulse_idle_width(hm2, i);
    }
    hm2->llio->write(
        hm2->llio,
        hm2->stepgen.pulse_idle_width_addr,
        hm2->stepgen.pulse_idle_width_reg,
        (hm2->stepgen.num_instances * sizeof(u32))
    );
}


static void hm2_stepgen_force_write_pulse_width_time(hostmot2_t *hm2) {
    int i;
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        hm2_stepgen_update_pulse_width(hm2, i);
    }
    hm2->llio->write(
        hm2->llio,
        hm2->stepgen.pulse_width_addr,
        hm2->stepgen.pulse_width_reg,
        (hm2->stepgen.num_instances * sizeof(u32))
    );
}


static void hm2_stepgen_force_write_master_dds(hostmot2_t *hm2) {
    u32 val = 0xffffffff;
    hm2->llio->write(
        hm2->llio,
        hm2->stepgen.master_dds_addr,
        &val,
        sizeof(u32)
    );
}


void hm2_stepgen_force_write(hostmot2_t *hm2) {
    if (hm2->stepgen.num_instances == 0) return;
    hm2_stepgen_force_write_mode(hm2);
    hm2_stepgen_force_write_dir_setup_time(hm2);
    hm2_stepgen_force_write_dir_hold_time(hm2);
    hm2_stepgen_force_write_pulse_width_time(hm2);
    hm2_stepgen_force_write_pulse_idle_width(hm2);
    hm2_stepgen_force_write_master_dds(hm2);
}




void hm2_stepgen_tram_init(hostmot2_t *hm2) {
    int i;

    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        hm2->stepgen.instance[i].prev_accumulator = hm2->stepgen.accumulator_reg[i];
        hm2->stepgen.instance[i].old_position_cmd = *hm2->stepgen.instance[i].hal.pin.position_cmd;
    }
}




void hm2_stepgen_allocate_pins(hostmot2_t *hm2) {
    int i;

    for (i = 0; i < hm2->num_pins; i ++) {
        if (
            (hm2->pin[i].sec_tag != HM2_GTAG_STEPGEN)
            || (hm2->pin[i].sec_unit >= hm2->stepgen.num_instances)
        ) {
            continue;
        }

        // the hm2 stepgen driver only does two-pin output, step/dir etc
        if ((hm2->pin[i].sec_pin & 0x7f) > 2) continue;

        hm2_set_pin_source(hm2, i, HM2_PIN_SOURCE_IS_SECONDARY);
        if (hm2->pin[i].sec_pin & 0x80){
            hm2_set_pin_direction(hm2, i, HM2_PIN_DIR_IS_OUTPUT);
        }
    }
}




int hm2_stepgen_parse_md(hostmot2_t *hm2, int md_index) {
    hm2_module_descriptor_t *md = &hm2->md[md_index];
    int r;


    // 
    // some standard sanity checks
    //

    if (!hm2_md_is_consistent(hm2, md_index, 1, 10, 4, 0x01FF)) {
        if (!hm2_md_is_consistent(hm2, md_index, 0, 10, 4, 0x01FF)) {
            HM2_ERR("unknown stepgen MD:\n");
            HM2_ERR("    Version = %d, expected 0 or 1\n", md->version);
            HM2_ERR("    NumRegisters = %d, expected 10\n", md->num_registers);
            HM2_ERR("    InstanceStride = 0x%08X, expected 4\n", md->instance_stride);
            HM2_ERR("    MultipleRegisters = 0x%08X, expected 0x000001FF\n", md->multiple_registers);
            return -EINVAL;
        }
        HM2_PRINT("WARNING: This firmware has stepgen v0, high step rates require zero stepspace!  upgrade your firmware!\n");
    }

    if (hm2->stepgen.num_instances != 0) {
        HM2_ERR(
            "found duplicate Module Descriptor for %s (inconsistent firmware), not loading driver\n",
            hm2_get_general_function_name(md->gtag)
        );
        return -EINVAL;
    }

    if (hm2->config.num_stepgens > md->instances) {
        HM2_ERR(
            "config.num_stepgens=%d, but only %d are available, not loading driver\n",
            hm2->config.num_stepgens,
            md->instances
        );
        return -EINVAL;
    }

    if (hm2->config.num_stepgens == 0) {
        return 0;
    }


    // 
    // looks good, start initializing
    // 


    if (hm2->config.num_stepgens == -1) {
        hm2->stepgen.num_instances = md->instances;
    } else {
        hm2->stepgen.num_instances = hm2->config.num_stepgens;
    }


    hm2->stepgen.instance = (hm2_stepgen_instance_t *)hal_malloc(hm2->stepgen.num_instances * sizeof(hm2_stepgen_instance_t));
    if (hm2->stepgen.instance == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }

    hm2->stepgen.clock_frequency = md->clock_freq;
    hm2->stepgen.version = md->version;

    hm2->stepgen.step_rate_addr = md->base_address + (0 * md->register_stride);
    hm2->stepgen.accumulator_addr = md->base_address + (1 * md->register_stride);
    hm2->stepgen.mode_addr = md->base_address + (2 * md->register_stride);
    hm2->stepgen.dir_setup_time_addr = md->base_address + (3 * md->register_stride);
    hm2->stepgen.dir_hold_time_addr = md->base_address + (4 * md->register_stride);
    hm2->stepgen.pulse_width_addr = md->base_address + (5 * md->register_stride);
    hm2->stepgen.pulse_idle_width_addr = md->base_address + (6 * md->register_stride);
    hm2->stepgen.table_sequence_data_setup_addr = md->base_address + (7 * md->register_stride);
    hm2->stepgen.table_sequence_length_addr = md->base_address + (8 * md->register_stride);
    hm2->stepgen.master_dds_addr = md->base_address + (9 * md->register_stride);

    r = hm2_register_tram_write_region(hm2, hm2->stepgen.step_rate_addr, (hm2->stepgen.num_instances * sizeof(u32)), &hm2->stepgen.step_rate_reg);
    if (r < 0) {
        HM2_ERR("error registering tram write region for StepGen Step Rate register (%d)\n", r);
        goto fail0;
    }

    r = hm2_register_tram_read_region(hm2, hm2->stepgen.accumulator_addr, (hm2->stepgen.num_instances * sizeof(u32)), &hm2->stepgen.accumulator_reg);
    if (r < 0) {
        HM2_ERR("error registering tram read region for StepGen Accumulator register (%d)\n", r);
        goto fail0;
    }

    hm2->stepgen.mode_reg = (u32 *)kmalloc(hm2->stepgen.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->stepgen.mode_reg == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail0;
    }

    hm2->stepgen.dir_setup_time_reg = (u32 *)kmalloc(hm2->stepgen.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->stepgen.dir_setup_time_reg == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail1;
    }

    hm2->stepgen.dir_hold_time_reg = (u32 *)kmalloc(hm2->stepgen.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->stepgen.dir_hold_time_reg == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail2;
    }

    hm2->stepgen.pulse_width_reg = (u32 *)kmalloc(hm2->stepgen.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->stepgen.pulse_width_reg == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail3;
    }

    hm2->stepgen.pulse_idle_width_reg = (u32 *)kmalloc(hm2->stepgen.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->stepgen.pulse_idle_width_reg == NULL) {
        HM2_ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail4;
    }


    // export to HAL
    {
        int i;
        char name[HAL_NAME_LEN + 2];

        for (i = 0; i < hm2->stepgen.num_instances; i ++) {
            // pins
            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.position-cmd", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_IN, &(hm2->stepgen.instance[i].hal.pin.position_cmd), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.velocity-cmd", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_IN, &(hm2->stepgen.instance[i].hal.pin.velocity_cmd), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.velocity-fb", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.velocity_fb), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.position-fb", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.position_fb), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.counts", hm2->llio->name, i);
            r = hal_pin_s32_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.counts), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.enable", hm2->llio->name, i);
            r = hal_pin_bit_new(name, HAL_IN, &(hm2->stepgen.instance[i].hal.pin.enable), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.control-type", hm2->llio->name, i);
            r = hal_pin_bit_new(name, HAL_IN, &(hm2->stepgen.instance[i].hal.pin.control_type), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            // debug pins

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_pos_minus_prev_cmd", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_pos_minus_prev_cmd), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_ff_vel", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_ff_vel), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_s_to_match", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_s_to_match), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_vel_error", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_vel_error), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_err_at_match", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_err_at_match), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dbg_step_rate", hm2->llio->name, i);
            r = hal_pin_s32_new(name, HAL_OUT, &(hm2->stepgen.instance[i].hal.pin.dbg_step_rate), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding pin '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }


            // parameters
            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.position-scale", hm2->llio->name, i);
            r = hal_param_float_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.position_scale), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.maxvel", hm2->llio->name, i);
            r = hal_param_float_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.maxvel), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.maxaccel", hm2->llio->name, i);
            r = hal_param_float_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.maxaccel), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.steplen", hm2->llio->name, i);
            r = hal_param_u32_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.steplen), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.stepspace", hm2->llio->name, i);
            r = hal_param_u32_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.stepspace), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dirsetup", hm2->llio->name, i);
            r = hal_param_u32_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.dirsetup), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.dirhold", hm2->llio->name, i);
            r = hal_param_u32_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.dirhold), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.stepgen.%02d.step_type", hm2->llio->name, i);
            r = hal_param_u32_new(name, HAL_RW, &(hm2->stepgen.instance[i].hal.param.step_type), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                HM2_ERR("error adding param '%s', aborting\n", name);
                r = -ENOMEM;
                goto fail5;
            }

            // init
            *(hm2->stepgen.instance[i].hal.pin.position_cmd) = 0.0;
            *(hm2->stepgen.instance[i].hal.pin.counts) = 0;
            *(hm2->stepgen.instance[i].hal.pin.position_fb) = 0.0;
            *(hm2->stepgen.instance[i].hal.pin.velocity_fb) = 0.0;
            *(hm2->stepgen.instance[i].hal.pin.enable) = 0;
            *(hm2->stepgen.instance[i].hal.pin.control_type) = 0;

            hm2->stepgen.instance[i].hal.param.position_scale = 1.0;
            hm2->stepgen.instance[i].hal.param.maxvel = 0.0;
            hm2->stepgen.instance[i].hal.param.maxaccel = 1.0;

            hm2->stepgen.instance[i].subcounts = 0;

            // start out the slowest possible, let the user speed up if they want
            hm2->stepgen.instance[i].hal.param.steplen   = (double)0x3FFF * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
            hm2->stepgen.instance[i].hal.param.stepspace = (double)0x3FFF * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
            hm2->stepgen.instance[i].hal.param.dirsetup  = (double)0x3FFF * ((double)1e9 / (double)hm2->stepgen.clock_frequency);
            hm2->stepgen.instance[i].hal.param.dirhold   = (double)0x3FFF * ((double)1e9 / (double)hm2->stepgen.clock_frequency);

            hm2->stepgen.instance[i].hal.param.step_type = 0;  // step & dir

            hm2->stepgen.instance[i].written_steplen = 0;
            hm2->stepgen.instance[i].written_stepspace = 0;
            hm2->stepgen.instance[i].written_dirsetup = 0;
            hm2->stepgen.instance[i].written_dirhold = 0;

            hm2->stepgen.instance[i].written_step_type = 0xffffffff;

            hm2->stepgen.instance[i].prev_accumulator = 0;
        }
    }


    return hm2->stepgen.num_instances;


fail5:
    kfree(hm2->stepgen.pulse_idle_width_reg);

fail4:
    kfree(hm2->stepgen.pulse_width_reg);

fail3:
    kfree(hm2->stepgen.dir_hold_time_reg);

fail2:
    kfree(hm2->stepgen.dir_setup_time_reg);

fail1:
    kfree(hm2->stepgen.mode_reg);

fail0:
    hm2->stepgen.num_instances = 0;
    return r;
}




void hm2_stepgen_print_module(hostmot2_t *hm2) {
    int i;
    HM2_PRINT("StepGen: %d\n", hm2->stepgen.num_instances);
    if (hm2->stepgen.num_instances <= 0) return;
    HM2_PRINT("    clock_frequency: %d Hz (%s MHz)\n", hm2->stepgen.clock_frequency, hm2_hz_to_mhz(hm2->stepgen.clock_frequency));
    HM2_PRINT("    version: %d\n", hm2->stepgen.version);
    HM2_PRINT("    step_rate_addr: 0x%04X\n", hm2->stepgen.step_rate_addr);
    HM2_PRINT("    accumulator_addr: 0x%04X\n", hm2->stepgen.accumulator_addr);
    HM2_PRINT("    mode_addr: 0x%04X\n", hm2->stepgen.mode_addr);
    HM2_PRINT("    dir_setup_time_addr: 0x%04X\n", hm2->stepgen.dir_setup_time_addr);
    HM2_PRINT("    dir_hold_time_addr: 0x%04X\n", hm2->stepgen.dir_hold_time_addr);
    HM2_PRINT("    pulse_width_addr: 0x%04X\n", hm2->stepgen.pulse_width_addr);
    HM2_PRINT("    pulse_idle_width_addr: 0x%04X\n", hm2->stepgen.pulse_idle_width_addr);
    HM2_PRINT("    table_sequence_data_setup_addr: 0x%04X\n", hm2->stepgen.table_sequence_data_setup_addr);
    HM2_PRINT("    table_sequence_length_addr: 0x%04X\n", hm2->stepgen.table_sequence_length_addr);
    HM2_PRINT("    master_dds_addr: 0x%04X\n", hm2->stepgen.master_dds_addr);
    for (i = 0; i < hm2->stepgen.num_instances; i ++) {
        HM2_PRINT("    instance %d:\n", i);
        HM2_PRINT("        enable = %d\n", *hm2->stepgen.instance[i].hal.pin.enable);
        HM2_PRINT("        hw:\n");
        HM2_PRINT("            step_rate = 0x%08X\n", hm2->stepgen.step_rate_reg[i]);
        HM2_PRINT("            accumulator = 0x%08X\n", hm2->stepgen.accumulator_reg[i]);
        HM2_PRINT("            mode = 0x%08X\n", hm2->stepgen.mode_reg[i]);
        HM2_PRINT("            dir_setup_time = 0x%08X (%u ns)\n", hm2->stepgen.dir_setup_time_reg[i], hm2->stepgen.instance[i].hal.param.dirsetup);
        HM2_PRINT("            dir_hold_time = 0x%08X (%u ns)\n", hm2->stepgen.dir_hold_time_reg[i], hm2->stepgen.instance[i].hal.param.dirhold);
        HM2_PRINT("            pulse_width = 0x%08X (%u ns)\n", hm2->stepgen.pulse_width_reg[i], hm2->stepgen.instance[i].hal.param.steplen);
        HM2_PRINT("            pulse_idle_width = 0x%08X (%u ns)\n", hm2->stepgen.pulse_idle_width_reg[i], hm2->stepgen.instance[i].hal.param.stepspace);
    }
}

