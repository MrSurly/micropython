/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Nick Moore
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


#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/dac.h"

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objstr.h"

#include "modmachine.h"
#include "machine_timer.h"

#include <stdio.h>

typedef enum {
    CIRCULAR,
    SEQUENTIAL
} dac_mode_t;

typedef struct Qitem {
  struct Qitem* next;
  mp_obj_str_t* item; 
  size_t   index;
} Qitem;

typedef struct {
  Qitem* head;
  Qitem* tail;
  size_t size;
} Qhead;

void queue_init(Qhead* q) {
  q->head = q->tail = NULL;
  q->size = 0;
}

int queue_empty(Qhead* q) {
  return q->size == 0;
}
int queue_size(Qhead* q) {
  return q->size;
}

// Push item onto tail
int queue_push(Qhead* q, mp_obj_t* item_in) {
    mp_obj_str_t *item = (mp_obj_str_t*)item_in;
    Qitem *i = (Qitem *)malloc(sizeof(Qitem));
    if (i == NULL)
    {
        return 0;
  }
  i->item = item;
  i->index = 0;
  i->next = NULL;
  if (queue_empty(q)) {
    q->head = q->tail = i;
  } else {
    q->tail->next = i;
    q->tail = i;
  }
  size_t s = ++q->size;
  return s;
}

// pop item from head
mp_obj_str_t* queue_pop(Qhead* q) {
  if (queue_empty(q)) {
    return NULL;
  }
  --q->size;

  Qitem* i = q->head;
  q->head = i->next;
  i->next = NULL;
  if (q->head == NULL) {
    q->tail = NULL;
  }
  mp_obj_str_t* r = i->item;
  free(i);
  return r;
}

typedef struct _mdac_obj_t {
    mp_obj_base_t base;
    gpio_num_t gpio_id;
    dac_channel_t dac_id;
    machine_timer_obj_t* timer;
    SemaphoreHandle_t* mutex;
    Qhead queue;
    Qitem* cur_queue_item;
    dac_mode_t mode;
} mdac_obj_t;

STATIC mdac_obj_t mdac_obj[] = {
    {{&machine_dac_type}, GPIO_NUM_25, DAC_CHANNEL_1, NULL, NULL, {}, NULL, CIRCULAR},
    {{&machine_dac_type}, GPIO_NUM_26, DAC_CHANNEL_2, NULL, NULL, {}, NULL, CIRCULAR},
};


void queue_dump(Qhead* q) {
  printf("---\n");
  printf("head: %p\n", q->head);
  printf("tail: %p\n", q->tail);
  printf("size: %d\n", q->size);
  Qitem *i = q->head;
  while(i) {
    printf("%p --> %p (value: %p)\n", i, i->next, i->item);
    i = i->next;
  }
}

STATIC mp_obj_t mdac_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
        const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, 1, true);
    gpio_num_t pin_id = machine_pin_get_id(args[0]);
    mdac_obj_t *self = NULL;
    for (int i = 0; i < MP_ARRAY_SIZE(mdac_obj); i++) {
        if (pin_id == mdac_obj[i].gpio_id) { self = &mdac_obj[i]; break; }
    }
    if (!self) mp_raise_ValueError("invalid Pin for DAC");

    esp_err_t err = dac_output_enable(self->dac_id);
    if (err == ESP_OK) {
        err = dac_output_voltage(self->dac_id, 0);
    }
    if (err == ESP_OK) {
      if (self->mutex == NULL) {
        self->mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(self->mutex);
        queue_init(&self->queue);
      }
      return MP_OBJ_FROM_PTR(self);
    }
    mp_raise_ValueError("Parameter Error");
}

STATIC void mdac_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mdac_obj_t *self = self_in;
    mp_printf(print, "DAC(Pin(%u))", self->gpio_id);
}

// returns true if DAC output was updated
STATIC bool mdac_output_next (mdac_obj_t* self, uint8_t* byte_out) {
    if (self->cur_queue_item == NULL) {
      if (queue_empty(&self->queue)) {
        return false;
      }
      self->cur_queue_item = self->queue.head;
      self->cur_queue_item->index = 0;
    } 

    if(self->cur_queue_item->index >= self->cur_queue_item->item->len) {
      // If CIRCULAR, and
      // all queue entries are empty arrays
      // this while loop will never exit.
      self->cur_queue_item = self->cur_queue_item->next;
      if (self->mode == SEQUENTIAL) {
        queue_pop(&self->queue);
      }
      if (self->cur_queue_item == NULL && self->mode == CIRCULAR) {
          self->cur_queue_item = self->queue.head;
      }
      if (self->cur_queue_item != NULL) {
        self->cur_queue_item->index = 0;
      }
    }

  if (self->cur_queue_item != NULL) {
    // we don't trap any error here
    // not sure what to do if we did 
    // have an error
    uint8_t byte = self->cur_queue_item->item->data[self->cur_queue_item->index];
    dac_output_voltage(self->dac_id, byte);
    if (byte_out != NULL) {
      *byte_out = byte;
    }
    ++self->cur_queue_item->index;
    return true;
  }
  return false;
}

STATIC mp_obj_t mdac_step(mp_obj_t self_in) {
  mdac_obj_t* self = self_in;
  uint8_t byte_out;
  if (self->timer) {
    machine_timer_disable(self->timer);
  }
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  bool data_output = mdac_output_next(self, &byte_out);
  xSemaphoreGive(self->mutex);
  return data_output ? MP_OBJ_NEW_SMALL_INT(byte_out) : mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_step_obj, mdac_step);

STATIC mp_obj_t mdac_queue_size(mp_obj_t self_in) {
  mdac_obj_t* self = self_in;
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  size_t size = self->queue.size;
  xSemaphoreGive(self->mutex);
  return mp_obj_new_int(size);
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_queue_size_obj, mdac_queue_size);

STATIC mp_obj_t mdac_dump(mp_obj_t self_in) {
  mdac_obj_t* self = self_in;
  printf("===\n");
  queue_dump(&self->queue);
  printf("---\n");
  Qitem* cur = self->queue.head;
  while(cur) {
    printf("%s %p:\n", self->cur_queue_item == cur ? ">>" : "  ", cur->item);
    mp_obj_t iter = mp_getiter(cur->item, NULL);
    mp_obj_t item;
    while((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
      printf("%02X ", mp_obj_get_int(item));
    }
    printf("\n");
    cur = cur->next;
  }
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_dump_obj, mdac_dump);

// called by the timer
STATIC void mdac_callback(void *self_in) {
    mdac_obj_t *self = self_in;
    xSemaphoreTakeFromISR(self->mutex, NULL);
    bool data_output = mdac_output_next(self, NULL);
    if (!data_output) {
        machine_timer_disable(self->timer);
    }
    xSemaphoreGiveFromISR(self->mutex, NULL);
}

STATIC mp_obj_t mdac_enqueue_data(mp_obj_t self_in, mp_obj_t data) {
  mdac_obj_t* self = self_in;
  if(!MP_OBJ_IS_TYPE(data, &mp_type_bytes)) {
    mp_raise_ValueError("must be bytes object");
  }
  if(MP_ARRAY_SIZE(data) == 0) {
    // empty arrays are pointless, 
    // and they mess up the queue increment logic 
    // in the ISR
    return mp_const_none;
  }
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  queue_push(&self->queue, data);
  xSemaphoreGive(self->mutex);
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(mdac_enqueue_data_obj, mdac_enqueue_data);

STATIC mp_obj_t mdac_clear_queue(mp_obj_t self_in) {
  mdac_obj_t* self = self_in;
  machine_timer_disable(self->timer);
  xSemaphoreTake(self->mutex, portMAX_DELAY);
  self->cur_queue_item = NULL;
  while(queue_pop(&self->queue)!= NULL) {}
  xSemaphoreGive(self->mutex);
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_clear_queue_obj, mdac_clear_queue);

STATIC mp_obj_t mdac_start(mp_obj_t self_in) {
  mdac_obj_t *self = self_in;
  if (self->timer != NULL) {
    machine_timer_enable(self->timer, (void *)self);
  }
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_start_obj, mdac_start);

STATIC mp_obj_t mdac_stop(mp_obj_t self_in) {
    mdac_obj_t *self = self_in;
    machine_timer_disable(self->timer);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mdac_stop_obj, mdac_stop);

STATIC mp_obj_t mdac_set_freq(mp_obj_t self_in, mp_obj_t value_in) {
    mdac_obj_t *self = self_in;
    bool isInt = mp_obj_is_integer(value_in);
    bool isFloat = mp_obj_is_float(value_in);

    if(isInt || isFloat) {
      float freq = isFloat ? mp_obj_get_float(value_in) : mp_obj_get_int(value_in);
      if (self->timer == NULL) {
        self->timer = m_new_obj(machine_timer_obj_t);
        self->timer->base.type = &machine_timer_type;
        self->timer->group = 0;
        self->timer->index = 0;
        self->timer->handle = NULL;
      }
      self->timer->period = (uint64_t)(TIMER_SCALE / freq);
    } else if (MP_OBJ_IS_TYPE(value_in, &machine_timer_type)) {
      self->timer = value_in;
    }
    bool wasRunning = machine_timer_is_running(self->timer);
    machine_timer_disable(self->timer); // necessary to reset freq, ISR, etc.
    if (wasRunning) {
      mdac_start(self);
    }
    self->timer->repeat = 1;
    self->timer->c_callback = mdac_callback;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(mdac_set_freq_obj, mdac_set_freq);

STATIC mp_obj_t mdac_mode(mp_uint_t n_args, const mp_obj_t *args) {
  mdac_obj_t *self = args[0];
  if (n_args == 1) {
    return MP_OBJ_NEW_SMALL_INT(self->mode);
  } else {
    mp_int_t value;
    bool ok = mp_obj_get_int_maybe(args[1], &value);
    if(ok && (value == CIRCULAR || value == SEQUENTIAL)){
      self->mode = value;
      return mp_const_none;
    }
    mp_raise_ValueError(NULL);

  }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mdac_mode_obj, 1, 2, mdac_mode);

STATIC mp_obj_t mdac_write(mp_obj_t self_in, mp_obj_t value_in) {
    mdac_obj_t *self = self_in;
    printf("self: %p\n", self);
    if (self->timer != NULL) {
      machine_timer_disable(self->timer);
    }
    int value = mp_obj_get_int(value_in);
    if (value < 0 || value > 255) mp_raise_ValueError("Value out of range");

    esp_err_t err = dac_output_voltage(self->dac_id, value);
    if (err == ESP_OK) return mp_const_none;
    mp_raise_ValueError("Parameter Error");
}
MP_DEFINE_CONST_FUN_OBJ_2(mdac_write_obj, mdac_write);

STATIC const mp_rom_map_elem_t mdac_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mdac_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_freq), MP_ROM_PTR(&mdac_set_freq_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&mdac_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&mdac_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&mdac_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_dump), MP_ROM_PTR(&mdac_dump_obj) },
    { MP_ROM_QSTR(MP_QSTR_enqueue_data), MP_ROM_PTR(&mdac_enqueue_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_queue), MP_ROM_PTR(&mdac_clear_queue_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue_size), MP_ROM_PTR(&mdac_queue_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_step), MP_ROM_PTR(&mdac_step_obj) },
    { MP_ROM_QSTR(MP_QSTR_CIRCULAR), MP_ROM_INT(CIRCULAR) },
    { MP_ROM_QSTR(MP_QSTR_SEQUENTIAL), MP_ROM_INT(SEQUENTIAL) },
};

STATIC MP_DEFINE_CONST_DICT(mdac_locals_dict, mdac_locals_dict_table);

const mp_obj_type_t machine_dac_type = {
    { &mp_type_type },
    .name = MP_QSTR_DAC,
    .print = mdac_print,
    .make_new = mdac_make_new,
    .locals_dict = (mp_obj_t)&mdac_locals_dict,
};
