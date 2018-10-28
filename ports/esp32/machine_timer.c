/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2015 Damien P. George
 * Copyright (c) 2016 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "modmachine.h"
#include "mphalport.h"
#include "machine_timer.h"

#define TIMER_INTR_SEL TIMER_INTR_LEVEL
#define TIMER_FLAGS    0

STATIC esp_err_t check_esp_err(esp_err_t code) {
    if (code) {
        mp_raise_OSError(code);
    }

    return code;
}

STATIC void machine_timer_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_timer_obj_t *self = self_in;

    timer_config_t config;
    mp_printf(print, "Timer(%p; ", self);

    timer_get_config(self->group, self->index, &config);

    mp_printf(print, "alarm_en=%d, ", config.alarm_en);
    mp_printf(print, "auto_reload=%d, ", config.auto_reload);
    mp_printf(print, "counter_en=%d)", config.counter_en);
}

STATIC mp_obj_t machine_timer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    machine_timer_obj_t *self = m_new_obj(machine_timer_obj_t);
    self->base.type = &machine_timer_type;

    self->group = (mp_obj_get_int(args[0]) >> 1) & 1;
    self->index = mp_obj_get_int(args[0]) & 1;

    return self;
}

bool machine_timer_is_running(machine_timer_obj_t* timer) {
    return timer->handle != NULL;
}

void machine_timer_disable(machine_timer_obj_t *timer) {
    if (timer->handle) {
        timer_pause(timer->group, timer->index);
        esp_intr_free(timer->handle);
        timer->handle = NULL;
    }
}

STATIC void machine_timer_isr(void *self_in) {
    machine_timer_obj_t *self = self_in;
    timg_dev_t *device = self->group ? &(TIMERG1) : &(TIMERG0);

    device->hw_timer[self->index].update = 1;
    if (self->index) {
        device->int_clr_timers.t1 = 1;
    } else {
        device->int_clr_timers.t0 = 1;
    }
    device->hw_timer[self->index].config.alarm_en = self->repeat;
    if (self->c_callback != NULL) {
        self->c_callback(self->c_callback_param);
    } else {
        mp_sched_schedule(self->callback, self);
        mp_hal_wake_main_task_from_isr();
    }
}

void machine_timer_enable(machine_timer_obj_t *timer, void* c_callback_param) {
    timer_config_t config;
    config.alarm_en = TIMER_ALARM_EN;
    config.auto_reload = timer->repeat;
    config.counter_dir = TIMER_COUNT_UP;
    config.divider = TIMER_DIVIDER;
    config.intr_type = TIMER_INTR_LEVEL;
    config.counter_en = TIMER_PAUSE;
    timer->c_callback_param = c_callback_param;

    check_esp_err(timer_init(timer->group, timer->index, &config));
    check_esp_err(timer_set_counter_value(timer->group, timer->index, 0x00000000));
    check_esp_err(timer_set_alarm_value(timer->group, timer->index, timer->period));
    check_esp_err(timer_enable_intr(timer->group, timer->index));
    check_esp_err(timer_isr_register(timer->group, timer->index, machine_timer_isr, timer, TIMER_FLAGS, &timer->handle));
    check_esp_err(timer_start(timer->group, timer->index));
}

STATIC mp_obj_t machine_timer_init_helper(machine_timer_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum {
        ARG_mode,
        ARG_callback,
        ARG_period,
        ARG_tick_hz,
        ARG_freq,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,         MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_callback,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_period,       MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0xffffffff} },
        { MP_QSTR_tick_hz,      MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1000} },
#if MICROPY_PY_BUILTINS_FLOAT
        { MP_QSTR_freq,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
#else
        { MP_QSTR_freq,         MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0xffffffff} },
#endif
    };

    machine_timer_disable(self);

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

#if MICROPY_PY_BUILTINS_FLOAT
    if (args[ARG_freq].u_obj != mp_const_none) {
        self->period = (uint64_t)(TIMER_SCALE / mp_obj_get_float(args[ARG_freq].u_obj));
    }
#else
    if (args[ARG_freq].u_int != 0xffffffff) {
        self->period = TIMER_SCALE / ((uint64_t)args[ARG_freq].u_int);
    }
#endif
    else {
        self->period = (((uint64_t)args[ARG_period].u_int) * TIMER_SCALE) / args[ARG_tick_hz].u_int;
    }

    self->repeat = args[ARG_mode].u_int;
    self->callback = args[ARG_callback].u_obj;
    self->c_callback = NULL;
    self->handle = NULL;

    machine_timer_enable(self, NULL);

    return mp_const_none;
}

STATIC mp_obj_t machine_timer_deinit(mp_obj_t self_in) {
    machine_timer_disable(self_in);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_deinit_obj, machine_timer_deinit);

STATIC mp_obj_t machine_timer_init(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_timer_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_timer_init_obj, 1, machine_timer_init);

STATIC mp_obj_t machine_timer_value(mp_obj_t self_in) {
    machine_timer_obj_t *self = self_in;
    double result;

    timer_get_counter_time_sec(self->group, self->index, &result);

    return MP_OBJ_NEW_SMALL_INT((mp_uint_t)(result * 1000));  // value in ms
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_value_obj, machine_timer_value);

STATIC const mp_rom_map_elem_t machine_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_timer_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_timer_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_ONE_SHOT), MP_ROM_INT(false) },
    { MP_ROM_QSTR(MP_QSTR_PERIODIC), MP_ROM_INT(true) },
};
STATIC MP_DEFINE_CONST_DICT(machine_timer_locals_dict, machine_timer_locals_dict_table);

const mp_obj_type_t machine_timer_type = {
    { &mp_type_type },
    .name = MP_QSTR_Timer,
    .print = machine_timer_print,
    .make_new = machine_timer_make_new,
    .locals_dict = (mp_obj_t)&machine_timer_locals_dict,
};
