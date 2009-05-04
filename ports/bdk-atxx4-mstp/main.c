/**************************************************************************
*
* Copyright (C) 2009 Steve Karg <skarg@users.sourceforge.net>
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*********************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include "hardware.h"
#include "init.h"
#include "timer.h"
#include "input.h"
#include "led.h"
#include "seeprom.h"
#include "timer.h"
#include "dcc.h"
#include "rs485.h"
#include "serial.h"
#include "datalink.h"
#include "npdu.h"
#include "handlers.h"
#include "client.h"
#include "txbuf.h"
#include "iam.h"
#include "device.h"
#include "ai.h"
#include "bi.h"
#include "bo.h"

/* local version override */
const char *BACnet_Version = "1.0";
/* MAC Address of MS/TP */
static uint8_t MSTP_MAC_Address;

/* For porting to IAR, see:
   http://www.avrfreaks.net/wiki/index.php/Documentation:AVR_GCC/IarToAvrgcc*/

static void bacnet_init(
    void)
{
#if defined(BACDL_MSTP)
    MSTP_MAC_Address = input_address();
    /* configure the BACnet Datalink */
    rs485_baud_rate_set(9600);
    dlmstp_set_max_master(127);
    dlmstp_set_max_info_frames(1);
    dlmstp_set_mac_address(MSTP_MAC_Address);
    dlmstp_init(NULL);
#endif
    Device_Set_Object_Instance_Number(4194303);
    /* we need to handle who-is to support dynamic device binding */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    /* Set the handlers for any confirmed services that we support. */
    /* We must implement read property - it's required! */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,
        handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE,
        handler_reinitialize_device);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,
        handler_write_property);
    /* handle communication so we can shutup when asked */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL,
        handler_device_communication_control);

    Send_I_Am(&Handler_Transmit_Buffer[0]);            
}

static uint8_t PDUBuffer[MAX_MPDU];
static void bacnet_task(void)
{
    uint8_t mstp_mac_address = 0;
    uint16_t pdu_len = 0;
    BACNET_ADDRESS src; /* source address */
    
    mstp_mac_address = input_address();
    if (MSTP_MAC_Address != mstp_mac_address) {
        MSTP_MAC_Address = mstp_mac_address;
        Send_I_Am(&Handler_Transmit_Buffer[0]);            
    }
    if (timer_elapsed_seconds(TIMER_DCC, 1)) {
        dcc_timer_seconds(1);
    }
    pdu_len = datalink_receive(&src, &PDUBuffer[0], sizeof(PDUBuffer), 0);
    if (pdu_len) {
        npdu_handler(&src, &PDUBuffer[0], pdu_len);
    }
}

void idle_init(void)
{
    timer_reset(TIMER_LED_3);
    timer_reset(TIMER_LED_4);
}

void idle_task(void)
{
    if (timer_elapsed_seconds(TIMER_LED_3, 1)) {
        timer_reset(TIMER_LED_3);
        led_toggle(LED_3);
    }
    if (timer_elapsed_milliseconds(TIMER_LED_4, 125)) {
        timer_reset(TIMER_LED_4);
        led_toggle(LED_4);
    }
}

int main(
    void)
{
    init();
    led_init();
    input_init();
    timer_init();
    seeprom_init();
    rs485_init();
    serial_init();
    bacnet_init();
    idle_init();
    /* Enable global interrupts */
    __enable_interrupt();
    for (;;) {
        input_task();
        bacnet_task();
        led_task();
        idle_task();
    }
}
