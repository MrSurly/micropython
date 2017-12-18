/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
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


// Free RTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Generic
#include <string.h>
#include <unistd.h>

// MicroPython
#include "py/runtime.h"
#include "py/runtime0.h"

// IDF
#include "bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"

extern bool bluetooth_enabled;

#define MSEC_0_625_TO_UNIT(time) ((time) * 1000 / (625))


#define CALLBACK_QUEUE_SIZE 10

typedef enum {

    // GATTS
    NETWORK_BT_GATTS_CONNECT,
    NETWORK_BT_GATTS_DISCONNECT,

    NETWORK_BT_GATTS_CREATE,
    NETWORK_BT_GATTS_START,
    NETWORK_BT_GATTS_STOP,
    NETWORK_BT_GATTS_ADD_CHAR, // Add char
    NETWORK_BT_GATTS_ADD_CHAR_DESCR, // Add descriptor

    // GAP / GATTC events
    NETWORK_BT_GATTC_SCAN_RES,
    NETWORK_BT_GATTC_SCAN_CMPL, // Found GATT servers
    NETWORK_BT_GATTC_SEARCH_RES, // Found GATTS services

    NETWORK_BT_GATTC_GET_CHAR,
    NETWORK_BT_GATTC_GET_DESCR,

    NETWORK_BT_GATTC_OPEN,
    NETWORK_BT_GATTC_CLOSE,

    // characteristic events
    NETWORK_BT_READ,
    NETWORK_BT_WRITE,
    NETWORK_BT_NOTIFY,

} network_bt_event_t;

typedef struct {
    network_bt_event_t event;
    union {

        struct {
            uint16_t                handle;
            esp_gatt_srvc_id_t      service_id;
        } gatts_create;
        /*

        struct {
            uint16_t                service_handle;
        } gatts_start_stop;

        struct {
            uint16_t                service_handle;
            uint16_t                handle;
            esp_bt_uuid_t           uuid;
        } gatts_add_char_descr;

        struct {
            uint16_t                conn_id;
            esp_bd_addr_t           bda;
        } gatts_connect_disconnect;

        struct {
            esp_bd_addr_t           bda;
            uint8_t*                adv_data; // Need to free this!
            uint8_t                 adv_data_len;
            int                     rssi;
        } gattc_scan_res;

        struct {
            uint16_t                conn_id;
            esp_gatt_id_t      service_id;
        } gattc_search_res;

        struct {
            uint16_t                conn_id;
            esp_gatt_srvc_id_t      service_id;
            esp_gatt_id_t           char_id;
            esp_gatt_char_prop_t    props;
        } gattc_get_char;

        struct {
            uint16_t                conn_id;
            esp_gatt_srvc_id_t      service_id;
            esp_gatt_id_t           char_id;
            esp_gatt_id_t           descr_id;
        } gattc_get_descr;

        struct {
            uint16_t                conn_id;
            uint16_t                mtu;
            esp_bd_addr_t           bda;

        } gattc_open_close;

        struct {
            uint16_t                conn_id;
            uint16_t                handle;
            uint32_t                trans_id;
            bool                    need_rsp;
        } read;

        struct {
            uint16_t                conn_id;
            uint16_t                handle;
            uint32_t                trans_id;
            bool                    need_rsp;

            // Following fields _must_
            // come after the first four above,
            // which _must_ match the read struct

            uint8_t*                value; // Need to free this!
            size_t                  value_len;
        } write;

        struct {
            uint16_t                conn_id;
            esp_bd_addr_t           remote_bda;
            uint16_t                handle;

            bool                    need_rsp;

            uint8_t*                value; // Need to free this!
            size_t                  value_len;
        } notify;
    */
    };

} callback_data_t;

typedef struct {
    uint8_t*                value; // Need to free this!
    size_t                  value_len;
} read_write_data_t;

const mp_obj_type_t network_bt_type;

STATIC SemaphoreHandle_t item_mut;
STATIC QueueHandle_t callback_q = NULL;
STATIC QueueHandle_t read_write_q = NULL;

STATIC void dumpBuf(const uint8_t *buf, size_t len) {
    while (len--) {
        printf("%02X ", *buf++);
    }
}

typedef struct {
    mp_obj_base_t           base;
    enum {
        NETWORK_BT_STATE_DEINIT,
        NETWORK_BT_STATE_INIT
    }                       state;
    bool                    advertising;
    bool                    scanning;
    bool                    gatts_connected;

    uint16_t                conn_id;
    esp_gatt_if_t           gatts_interface;
    esp_gatt_if_t           gattc_interface;

    esp_ble_adv_params_t    adv_params;
    esp_ble_adv_data_t      adv_data;

    mp_obj_t                services;       // GATTS, implemented as a list

    mp_obj_t                connections;    // GATTC, implemented as a list

    mp_obj_t                callback;
    mp_obj_t                callback_userdata;
} network_bt_obj_t;

// Singleton
STATIC network_bt_obj_t* network_bt_singleton = NULL;

STATIC network_bt_obj_t* network_bt_get_singleton() {

    if (network_bt_singleton == NULL) {
        network_bt_singleton = m_new_obj(network_bt_obj_t);

        network_bt_singleton->base.type = &network_bt_type;

        network_bt_singleton->state = NETWORK_BT_STATE_DEINIT;
        network_bt_singleton->advertising = false;
        network_bt_singleton->scanning = false;
        network_bt_singleton->gatts_connected = false;
        network_bt_singleton->conn_id = 0;
        network_bt_singleton->gatts_interface = ESP_GATT_IF_NONE;
        network_bt_singleton->gattc_interface = ESP_GATT_IF_NONE;
        network_bt_singleton->adv_params.adv_int_min = 1280 * 1.6;
        network_bt_singleton->adv_params.adv_int_max = 1280 * 1.6;
        network_bt_singleton->adv_params.adv_type = ADV_TYPE_IND;
        network_bt_singleton->adv_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
        network_bt_singleton->adv_params.peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
        network_bt_singleton->adv_params.channel_map = ADV_CHNL_ALL;
        network_bt_singleton->adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
        network_bt_singleton->adv_data.set_scan_rsp = false;
        network_bt_singleton->adv_data.include_name = false;
        network_bt_singleton->adv_data.include_txpower = false;
        network_bt_singleton->adv_data.min_interval = 1280 * 1.6;
        network_bt_singleton->adv_data.max_interval = 1280 * 1.6;
        network_bt_singleton->adv_data.appearance = 0;
        network_bt_singleton->adv_data.p_manufacturer_data = NULL;
        network_bt_singleton->adv_data.manufacturer_len = 0;
        network_bt_singleton->adv_data.p_service_data = NULL;
        network_bt_singleton->adv_data.service_data_len = 0;
        network_bt_singleton->adv_data.p_service_uuid = 0;
        network_bt_singleton->adv_data.service_uuid_len = 0;
        network_bt_singleton->adv_data.flag = 0;

        network_bt_singleton->callback = mp_const_none;
        network_bt_singleton->callback_userdata = mp_const_none;

        network_bt_singleton->services = mp_obj_new_list(0, NULL);
        network_bt_singleton->connections = mp_obj_new_list(0, NULL);
        memset(network_bt_singleton->adv_params.peer_addr, 0, sizeof(network_bt_singleton->adv_params.peer_addr));
    }
    return network_bt_singleton;
}

#if CONFIG_GATTS_ENABLE
STATIC void network_bt_gatts_event_handler(
    esp_gatts_cb_event_t event,
    esp_gatt_if_t gatts_if,
    esp_ble_gatts_cb_param_t *param) {

    network_bt_obj_t* bluetooth = network_bt_get_singleton();
    if (bluetooth->state != NETWORK_BT_STATE_INIT) {
        return;
    }

#ifdef EVENT_DEBUG_GATTS
    gatts_event_dump(event, gatts_if, param);
#endif
}
#endif

#if CONFIG_GATTC_ENABLE
STATIC void network_bt_gattc_event_handler(
    esp_gattc_cb_event_t event,
    esp_gatt_if_t gattc_if,
    esp_ble_gattc_cb_param_t *param) {

    network_bt_obj_t* bluetooth = network_bt_get_singleton();
    if (bluetooth->state != NETWORK_BT_STATE_INIT) {
        return;
    }
#ifdef EVENT_DEBUG_GATTC
    gattc_event_dump(event, gattc_if, param);
#endif
}
#endif

STATIC void network_bt_gap_event_handler(
    esp_gap_ble_cb_event_t event,
    esp_ble_gap_cb_param_t *param) {

#ifdef EVENT_DEBUG_GAP
    gap_event_dump(event, param);
#endif

    network_bt_obj_t* bluetooth = network_bt_get_singleton();
    if (bluetooth->state != NETWORK_BT_STATE_INIT) {
        return;
    }

}

STATIC void network_bt_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    network_bt_obj_t *self = MP_OBJ_TO_PTR(self_in);
    //#define NETWORK_BT_LF "\n"
#define NETWORK_BT_LF
    mp_printf(print,
              "Bluetooth(params=(conn_id=%04X" NETWORK_BT_LF
              ", gatts_connected=%s" NETWORK_BT_LF
              ", gatts_if=%u" NETWORK_BT_LF
              ", gattc_if=%u" NETWORK_BT_LF
              ", adv_params=("
              "adv_int_min=%u, " NETWORK_BT_LF
              "adv_int_max=%u, " NETWORK_BT_LF
              "adv_type=%u, " NETWORK_BT_LF
              "own_addr_type=%u, " NETWORK_BT_LF
              "peer_addr=%02X:%02X:%02X:%02X:%02X:%02X, " NETWORK_BT_LF
              "peer_addr_type=%u, " NETWORK_BT_LF
              "channel_map=%u, " NETWORK_BT_LF
              "adv_filter_policy=%u" NETWORK_BT_LF
              "state=%d" NETWORK_BT_LF
              ")"
              ,
              self->conn_id,
              self->gatts_connected ? "True" : "False",
              self->gatts_interface,
              self->gattc_interface,
              (unsigned int)(self->adv_params.adv_int_min / 1.6),
              (unsigned int)(self->adv_params.adv_int_max / 1.6),
              self->adv_params.adv_type,
              self->adv_params.own_addr_type,
              self->adv_params.peer_addr[0],
              self->adv_params.peer_addr[1],
              self->adv_params.peer_addr[2],
              self->adv_params.peer_addr[3],
              self->adv_params.peer_addr[4],
              self->adv_params.peer_addr[5],
              self->adv_params.peer_addr_type,
              self->adv_params.channel_map,
              self->adv_params.adv_filter_policy,
              self->state
             );
    mp_printf(print,
              ", data=("
              "set_scan_rsp=%s, " NETWORK_BT_LF
              "include_name=%s, " NETWORK_BT_LF
              "include_txpower=%s, " NETWORK_BT_LF
              "min_interval=%d, " NETWORK_BT_LF
              "max_interval=%d, " NETWORK_BT_LF
              "appearance=%d, " NETWORK_BT_LF
              "manufacturer_len=%d, " NETWORK_BT_LF
              "p_manufacturer_data=%s, " NETWORK_BT_LF
              "service_data_len=%d, " NETWORK_BT_LF
              "p_service_data=",
              self->adv_data.set_scan_rsp ? "True" : "False",
              self->adv_data.include_name ? "True" : "False",
              self->adv_data.include_txpower ? "True" : "False",
              self->adv_data.min_interval,
              self->adv_data.max_interval,
              self->adv_data.appearance,
              self->adv_data.manufacturer_len,
              self->adv_data.p_manufacturer_data ? (const char *)self->adv_data.p_manufacturer_data : "nil",
              self->adv_data.service_data_len);
    if (self->adv_data.p_service_data != NULL) {
        dumpBuf(self->adv_data.p_service_data, self->adv_data.service_data_len);
    }
    mp_printf(print, ", " NETWORK_BT_LF "flag=%d" NETWORK_BT_LF , self->adv_data.flag);
    mp_printf(print, ")");
}

// bluetooth.init(...)

STATIC mp_obj_t network_bt_init(mp_obj_t self_in) {
    network_bt_obj_t * self = (network_bt_obj_t*)self_in;

    if (!bluetooth_enabled) {
        mp_raise_msg(&mp_type_MemoryError, "Bluetooth memory not allocated");
    }

    if (item_mut == NULL) {
        item_mut = xSemaphoreCreateMutex();
        xSemaphoreGive(item_mut);
    }

    if (callback_q == NULL) {
        callback_q = xQueueCreate(CALLBACK_QUEUE_SIZE, sizeof(callback_data_t));
        if (callback_q == NULL) {
            mp_raise_msg(&mp_type_OSError, "unable to create callback queue");
        }
    } else {
        xQueueReset(callback_q);
    }

    if (read_write_q == NULL) {
        read_write_q = xQueueCreate(1, sizeof(read_write_data_t));
        if (read_write_q == NULL) {
            mp_raise_msg(&mp_type_OSError, "unable to create read queue");
        }
    } else {
        xQueueReset(read_write_q);
    }
    printf("past queue initializations\n");

    if (self->state == NETWORK_BT_STATE_DEINIT) {

        self->state = NETWORK_BT_STATE_INIT;

        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        printf("before esp_bt_controller_init\n");
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_bt_controller_init() failed");
        }
        printf("after esp_bt_controller_init\n");
        sleep(1);

        printf("before esp_bt_controller_enable\n");
        if (esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_bt_controller_enable() failed");
        }

        printf("before esp_bluedroid_init\n");
        if (esp_bluedroid_init() != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_bluedroid_init() failed");
        }

        printf("before esp_bluedroid_enable\n");
        if (esp_bluedroid_enable() != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_bluedroid_enable() failed");
        }


#if CONFIG_GATTS_ENABLE
        printf("before esp_ble_gatts_register_callback\n");
        if (esp_ble_gatts_register_callback(network_bt_gatts_event_handler) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_ble_gatts_register_callback() failed");
        }
#endif

#if CONFIG_GATTC_ENABLE
        printf("before esp_ble_gattc_register_callback\n");
        if (esp_ble_gattc_register_callback(network_bt_gattc_event_handler) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_ble_gattc_register_callback() failed");
        }
#endif

        printf("before esp_ble_gap_register_callback\n");
        if (esp_ble_gap_register_callback(network_bt_gap_event_handler) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_ble_gap_register_callback() failed");
        }

#if CONFIG_GATTS_ENABLE
        printf("before esp_ble_gatts_app_register\n");
        if (esp_ble_gatts_app_register(0) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_ble_gatts_app_register() failed");
        }
#endif

#if CONFIG_GATTC_ENABLE
        printf("before esp_ble_gattc_app_register\n");
        if (esp_ble_gattc_app_register(1) != ESP_OK) {
            mp_raise_msg(&mp_type_OSError, "esp_ble_gattc_app_register() failed");
        }
#endif

    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(network_bt_init_obj, network_bt_init);

STATIC mp_obj_t network_bt_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    printf("enter network_bt_make_new\n");
    network_bt_obj_t *self = network_bt_get_singleton();
    if (n_args != 0 || n_kw != 0) {
        mp_raise_TypeError("Constructor takes no arguments");
    }

    printf("before network_bt_init\n");
    network_bt_init(self);
    printf("after network_bt_init\n");
    return MP_OBJ_FROM_PTR(self);
}

STATIC mp_obj_t network_bt_ble_adv_enable(mp_obj_t self_in, mp_obj_t enable_in) {
    network_bt_obj_t * self = MP_OBJ_TO_PTR(self_in);
    bool enable = mp_obj_is_true(enable_in);
    if (enable) {
        esp_ble_adv_params_t params = {
            .adv_int_min       = MSEC_0_625_TO_UNIT(100), // 100ms
            .adv_int_max       = MSEC_0_625_TO_UNIT(100), // 100ms
            .adv_type          = ADV_TYPE_IND,
            .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
            .channel_map       = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };
        esp_ble_gap_start_advertising(&params);
    } else {
        esp_ble_gap_stop_advertising();
    }
    self->advertising = enable;
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(network_bt_ble_adv_enable_obj, network_bt_ble_adv_enable);


STATIC const mp_rom_map_elem_t network_bt_locals_dict_table[] = {
    // instance methods

    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&network_bt_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_adv_enable), MP_ROM_PTR(&network_bt_ble_adv_enable_obj) },
    /*
       { MP_ROM_QSTR(MP_QSTR_ble_settings), MP_ROM_PTR(&network_bt_ble_settings_obj) },
       { MP_ROM_QSTR(MP_QSTR_ble_adv_enable), MP_ROM_PTR(&network_bt_ble_adv_enable_obj) },
       { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&network_bt_deinit_obj) },
       { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_bt_connect_obj) },
       { MP_ROM_QSTR(MP_QSTR_Service), MP_ROM_PTR(&network_bt_gatts_service_type) },
       { MP_ROM_QSTR(MP_QSTR_services), MP_ROM_PTR(&network_bt_services_obj) },
       { MP_ROM_QSTR(MP_QSTR_conns), MP_ROM_PTR(&network_bt_conns_obj) },
       { MP_ROM_QSTR(MP_QSTR_callback), MP_ROM_PTR(&network_bt_callback_obj) },
       { MP_ROM_QSTR(MP_QSTR_scan_start), MP_ROM_PTR(&network_bt_scan_start_obj) },
       { MP_ROM_QSTR(MP_QSTR_scan_stop), MP_ROM_PTR(&network_bt_scan_stop_obj) },
       { MP_ROM_QSTR(MP_QSTR_is_scanning), MP_ROM_PTR(&network_bt_is_scanning_obj) },

    // class constants

    // Callback types
    { MP_ROM_QSTR(MP_QSTR_CONNECT),                     MP_ROM_INT(NETWORK_BT_GATTS_CONNECT) },
    { MP_ROM_QSTR(MP_QSTR_DISCONNECT),                  MP_ROM_INT(NETWORK_BT_GATTS_DISCONNECT) },
    { MP_ROM_QSTR(MP_QSTR_READ),                        MP_ROM_INT(NETWORK_BT_READ) },
    { MP_ROM_QSTR(MP_QSTR_WRITE),                       MP_ROM_INT(NETWORK_BT_WRITE) },
    { MP_ROM_QSTR(MP_QSTR_NOTIFY),                      MP_ROM_INT(NETWORK_BT_NOTIFY) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_RES),                    MP_ROM_INT(NETWORK_BT_GATTC_SCAN_RES) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_CMPL),                   MP_ROM_INT(NETWORK_BT_GATTC_SCAN_CMPL) },

    // esp_ble_adv_type_t
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_IND),                MP_ROM_INT(ADV_TYPE_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_DIRECT_IND_HIGH),    MP_ROM_INT(ADV_TYPE_DIRECT_IND_HIGH) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_SCAN_IND),           MP_ROM_INT(ADV_TYPE_SCAN_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_NONCONN_IND),        MP_ROM_INT(ADV_TYPE_NONCONN_IND) },
    { MP_ROM_QSTR(MP_QSTR_ADV_TYPE_DIRECT_IND_LOW),     MP_ROM_INT(ADV_TYPE_DIRECT_IND_LOW) },

    // esp_ble_addr_type_t
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_PUBLIC),        MP_ROM_INT(BLE_ADDR_TYPE_PUBLIC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RANDOM),        MP_ROM_INT(BLE_ADDR_TYPE_RANDOM) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RPA_PUBLIC),    MP_ROM_INT(BLE_ADDR_TYPE_RPA_PUBLIC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADDR_TYPE_RPA_RANDOM),    MP_ROM_INT(BLE_ADDR_TYPE_RPA_RANDOM) },

    // esp_ble_adv_channel_t
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_37),                 MP_ROM_INT(ADV_CHNL_37) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_38),                 MP_ROM_INT(ADV_CHNL_38) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_39),                 MP_ROM_INT(ADV_CHNL_39) },
    { MP_ROM_QSTR(MP_QSTR_ADV_CHNL_ALL),                MP_ROM_INT(ADV_CHNL_ALL) },

    // BLE_ADV_DATA_FLAG

    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_LIMIT_DISC),         MP_ROM_INT(ESP_BLE_ADV_FLAG_LIMIT_DISC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_GEN_DISC),           MP_ROM_INT(ESP_BLE_ADV_FLAG_GEN_DISC) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_BREDR_NOT_SPT),      MP_ROM_INT(ESP_BLE_ADV_FLAG_BREDR_NOT_SPT) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_DMT_CONTROLLER_SPT), MP_ROM_INT(ESP_BLE_ADV_FLAG_DMT_CONTROLLER_SPT) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_DMT_HOST_SPT),       MP_ROM_INT(ESP_BLE_ADV_FLAG_DMT_HOST_SPT) },
    { MP_ROM_QSTR(MP_QSTR_BLE_ADV_FLAG_NON_LIMIT_DISC),     MP_ROM_INT(ESP_BLE_ADV_FLAG_NON_LIMIT_DISC) },

    // Scan param constants
    { MP_ROM_QSTR(MP_QSTR_SCAN_TYPE_PASSIVE),               MP_ROM_INT(BLE_SCAN_TYPE_PASSIVE) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_TYPE_ACTIVE),                MP_ROM_INT(BLE_SCAN_TYPE_ACTIVE) },

    { MP_ROM_QSTR(MP_QSTR_SCAN_FILTER_ALLOW_ALL),           MP_ROM_INT(BLE_SCAN_FILTER_ALLOW_ALL) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_FILTER_ALLOW_ONLY_WLST),     MP_ROM_INT(BLE_SCAN_FILTER_ALLOW_ONLY_WLST) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_FILTER_ALLOW_UND_RPA_DIR),   MP_ROM_INT(BLE_SCAN_FILTER_ALLOW_UND_RPA_DIR) },
    { MP_ROM_QSTR(MP_QSTR_SCAN_FILTER_ALLOW_WLIST_PRA_DIR), MP_ROM_INT(BLE_SCAN_FILTER_ALLOW_WLIST_PRA_DIR) },


    // exp_gatt_perm_t
    { MP_ROM_QSTR(MP_QSTR_PERM_READ),                   MP_ROM_INT(ESP_GATT_PERM_READ) },
    { MP_ROM_QSTR(MP_QSTR_PERM_READ_ENCRYPTED),         MP_ROM_INT(ESP_GATT_PERM_READ_ENCRYPTED) },
    { MP_ROM_QSTR(MP_QSTR_PERM_READ_ENC_MITM),          MP_ROM_INT(ESP_GATT_PERM_READ_ENC_MITM) },
    { MP_ROM_QSTR(MP_QSTR_PERM_WRITE),                  MP_ROM_INT(ESP_GATT_PERM_WRITE) },
    { MP_ROM_QSTR(MP_QSTR_PERM_WRITE_ENCRYPTED),        MP_ROM_INT(ESP_GATT_PERM_WRITE_ENCRYPTED) },
    { MP_ROM_QSTR(MP_QSTR_PERM_WRITE_ENC_MITM),         MP_ROM_INT(ESP_GATT_PERM_WRITE_ENC_MITM) },
    { MP_ROM_QSTR(MP_QSTR_PERM_WRITE_SIGNED),           MP_ROM_INT(ESP_GATT_PERM_WRITE_SIGNED) },
    { MP_ROM_QSTR(MP_QSTR_PERM_WRITE_SIGNED_MITM),      MP_ROM_INT(ESP_GATT_PERM_WRITE_SIGNED_MITM) },

    // esp_gatt_char_prop_t

    { MP_ROM_QSTR(MP_QSTR_PROP_BROADCAST),              MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_BROADCAST) },
    { MP_ROM_QSTR(MP_QSTR_PROP_READ),                   MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_READ) },
    { MP_ROM_QSTR(MP_QSTR_PROP_WRITE_NR),               MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_WRITE_NR) },
    { MP_ROM_QSTR(MP_QSTR_PROP_WRITE),                  MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_WRITE) },
    { MP_ROM_QSTR(MP_QSTR_PROP_NOTIFY),                 MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_NOTIFY) },
    { MP_ROM_QSTR(MP_QSTR_PROP_INDICATE),               MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_INDICATE) },
    { MP_ROM_QSTR(MP_QSTR_PROP_AUTH),                   MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_AUTH) },
    { MP_ROM_QSTR(MP_QSTR_PROP_EXT_PROP),               MP_ROM_INT(ESP_GATT_CHAR_PROP_BIT_EXT_PROP) },

    // esp_ble_adv_filter_t
    {   MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY)
    },
    {   MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_WLST_CON_ANY),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_WLST_CON_ANY)
    },
    {   MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST)
    },
    {   MP_ROM_QSTR(MP_QSTR_ADV_FILTER_ALLOW_SCAN_WLST_CON_WLST),
        MP_ROM_INT(ADV_FILTER_ALLOW_SCAN_WLST_CON_WLST)
    },
    */
};

STATIC MP_DEFINE_CONST_DICT(network_bt_locals_dict, network_bt_locals_dict_table);

const mp_obj_type_t network_bt_type = {
    { &mp_type_type },
    .name = MP_QSTR_Bluetooth,
    .print = network_bt_print,
    .make_new = network_bt_make_new,
    .locals_dict = (mp_obj_dict_t*)&network_bt_locals_dict,
};
