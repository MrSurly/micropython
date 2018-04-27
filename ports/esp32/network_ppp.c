/* * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 "Eric Poulsen" <eric@zyxod.com>
 *
 * Based on the ESP IDF example code which is Public Domain / CC0
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


#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objtype.h"
#include "netutils.h"

#include "modmachine.h"
#include "machine_uart.h"

#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/pppapi.h"

extern const mp_obj_type_t machine_uart_type;

typedef struct _ppp_if_obj_t {
    mp_obj_base_t base;
    bool initialized;
    bool active;
    bool connected;
    ppp_pcb *pcb;
    machine_uart_obj_t* uart;
    struct netif ppp_netif;
    TaskHandle_t client_task_handle;

} ppp_if_obj_t;

const mp_obj_type_t ppp_if_type;
STATIC ppp_if_obj_t ppp_obj = {{&ppp_if_type}, false, false, false, NULL, NULL};


/* PPP status callback example */
static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx)
{
    ppp_if_obj_t* self = &ppp_obj;
    struct netif *pppif = ppp_netif(self->pcb);
    LWIP_UNUSED_ARG(ctx);

    switch (err_code) {
        case PPPERR_NONE: {
                              printf("status_cb: Connected\n");
#if PPP_IPV4_SUPPORT
                              printf("   our_ipaddr  = %s\n", ipaddr_ntoa(&pppif->ip_addr));
                              printf("   his_ipaddr  = %s\n", ipaddr_ntoa(&pppif->gw));
                              printf("   netmask     = %s\n", ipaddr_ntoa(&pppif->netmask));
#endif /* PPP_IPV4_SUPPORT */
#if PPP_IPV6_SUPPORT
                              printf("   our6_ipaddr = %s\n", ip6addr_ntoa(netif_ip6_addr(pppif, 0)));
#endif /* PPP_IPV6_SUPPORT */
                              break;
                          }
        case PPPERR_PARAM: {
                               printf("status_cb: Invalid parameter\n");
                               break;
                           }
        case PPPERR_OPEN: {
                              printf("status_cb: Unable to open PPP session\n");
                              break;
                          }
        case PPPERR_DEVICE: {
                                printf("status_cb: Invalid I/O device for PPP\n");
                                break;
                            }
        case PPPERR_ALLOC: {
                               printf("status_cb: Unable to allocate resources\n");
                               break;
                           }
        case PPPERR_USER: {
                              printf("status_cb: User interrupt\n");
                              break;
                          }
        case PPPERR_CONNECT: {
                                 printf("status_cb: Connection lost\n");
                                 break;
                             }
        case PPPERR_AUTHFAIL: {
                                  printf("status_cb: Failed authentication challenge\n");
                                  break;
                              }
        case PPPERR_PROTOCOL: {
                                  printf("status_cb: Failed to meet protocol\n");
                                  break;
                              }
        case PPPERR_PEERDEAD: {
                                  printf("status_cb: Connection timeout\n");
                                  break;
                              }
        case PPPERR_IDLETIMEOUT: {
                                     printf("status_cb: Idle Timeout\n");
                                     break;
                                 }
        case PPPERR_CONNECTTIME: {
                                     printf("status_cb: Max connect time reached\n");
                                     break;
                                 }
        case PPPERR_LOOPBACK: {
                                  printf("status_cb: Loopback detected\n");
                                  break;
                              }
        default: {
                     printf("status_cb: Unknown error code %d\n", err_code);
                     break;
                 }
    }

    /*
     * This should be in the switch case, this is put outside of the switch
     * case for example readability.
     */

    if (err_code == PPPERR_NONE) {
        return;
    }

    /* ppp_close() was previously called, don't reconnect */
    if (err_code == PPPERR_USER) {
        /* ppp_free(); -- can be called here */
        return;
    }


    /*
     * Try to reconnect in 30 seconds, if you need a modem chatscript you have
     * to do a much better signaling here ;-)
     */
    //ppp_connect(pcb, 30);
    /* OR ppp_listen(pcb); */
}

STATIC mp_obj_t get_ppp(mp_obj_t uart) {
    ppp_if_obj_t* self = &ppp_obj;

    if (self->initialized) {
        return MP_OBJ_FROM_PTR(&ppp_obj);
    }

    if (! MP_OBJ_IS_TYPE(uart, &machine_uart_type)) {
        mp_raise_TypeError("requires machine.UART type");
    }

    self->uart = (machine_uart_obj_t*) uart;
    return MP_OBJ_FROM_PTR(&ppp_obj);
}
MP_DEFINE_CONST_FUN_OBJ_1(get_ppp_obj, get_ppp);

static u32_t ppp_output_callback(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx)
{
    ppp_if_obj_t* self = &ppp_obj;
    mp_stream_p_t* uart_stream = (mp_stream_p_t* )self->uart->base.type->protocol; 
    int err;

    return uart_stream->write(self->uart, data, len, &err);
}

static void pppos_client_task() {
    ppp_if_obj_t* self = &ppp_obj;
    self->uart->timeout = 100;
    mp_stream_p_t* uart_stream = (mp_stream_p_t* )self->uart->base.type->protocol; 
    uint8_t buf[1024];
    int err;
    while (ulTaskNotifyTake(pdTRUE, 0) == 0) {
        int len = uart_stream->read(self->uart, buf, 1024, &err);
        if (len > 0) {
            pppos_input_tcpip(self->pcb, (u8_t *)buf, len);
        }
    }
}


STATIC mp_obj_t ppp_active(size_t n_args, const mp_obj_t *args) {
    ppp_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (n_args > 1) {
        if (mp_obj_is_true(args[1])) {
            if (self->active) {
                return mp_const_true;
            }

            self->pcb = pppapi_pppos_create(&self->ppp_netif, ppp_output_callback, ppp_status_cb, NULL);

            if (self->pcb == NULL) {
                mp_raise_msg(&mp_type_RuntimeError, "init failed");
            }
            pppapi_set_default(self->pcb);
            //pppapi_set_auth(ppp, PPPAUTHTYPE_PAP, PPP_User, PPP_Pass);
            pppapi_connect(self->pcb, 0);

            xTaskCreate(pppos_client_task, "ppp", 2048, NULL, 1, &self->client_task_handle);
            self->active = true;

        } else {
            if (!self->active) {
                mp_raise_msg(&mp_type_RuntimeError, "already inactive");
            }
            xTaskNotifyGive(self->client_task_handle);
            mp_hal_delay_ms(100);
            self->active = false;
        }
    }
    return mp_obj_new_bool(self->active);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ppp_active_obj, 1, 2, ppp_active);

STATIC mp_obj_t ppp_ifconfig(size_t n_args, const mp_obj_t *args) {
    ppp_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    struct netif *pppif = ppp_netif(self->pcb);
    if (n_args == 1) {
        // get
        ip_addr_t dns = dns_getserver(0);
        mp_obj_t tuple[4] = {
            netutils_format_ipv4_addr((uint8_t*)&pppif->ip_addr, NETUTILS_BIG),
            netutils_format_ipv4_addr((uint8_t*)&pppif->gw, NETUTILS_BIG),
            netutils_format_ipv4_addr((uint8_t*)&pppif->netmask, NETUTILS_BIG),
            netutils_format_ipv4_addr((uint8_t*)&dns, NETUTILS_BIG),
        };
        return mp_obj_new_tuple(4, tuple);
    } else {
        mp_raise_msg(&mp_type_RuntimeError, "cannot set ifconfig for PPP");
    }
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ppp_ifconfig_obj, 1, 2, ppp_ifconfig);

STATIC mp_obj_t ppp_status(mp_obj_t self_in) {
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(ppp_status_obj, ppp_status);

STATIC mp_obj_t ppp_isconnected(mp_obj_t self_in) {
    ppp_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->connected);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(ppp_isconnected_obj, ppp_isconnected);

STATIC const mp_rom_map_elem_t ppp_if_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&ppp_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&ppp_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&ppp_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&ppp_ifconfig_obj) },
};

STATIC MP_DEFINE_CONST_DICT(ppp_if_locals_dict, ppp_if_locals_dict_table);

const mp_obj_type_t ppp_if_type = {
    { &mp_type_type },
    .name = MP_QSTR_LAN,
    .locals_dict = (mp_obj_dict_t*)&ppp_if_locals_dict,
};
