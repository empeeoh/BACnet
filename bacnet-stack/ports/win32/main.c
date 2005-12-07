/**************************************************************************
*
* Copyright (C) 2005 Steve Karg <skarg@users.sourceforge.net>
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

// This is one way to use the embedded BACnet stack under Win32
// compiled with Borland C++ 5.02
#include <winsock2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <conio.h> /* for kbhit and getch */
#include "iam.h"
#include "address.h"
#include "config.h"
#include "bacdef.h"
#include "npdu.h"
#include "apdu.h"
#include "device.h"
#include "handlers.h"
#include "datalink.h"

// buffer used for receive
static uint8_t Rx_Buf[MAX_MPDU] = {0};

static void Read_Properties(void)
{
  uint32_t device_id = 0;
  unsigned max_apdu = 0;
  BACNET_ADDRESS src;
  bool next_device = false;
  static unsigned index = 0;
  static unsigned property = 0;
  // list of required (and some optional) properties in the
  // Device Object
  const int object_props[] =
  {
    75,77,79,112,121,120,70,44,12,98,95,97,96,
    62,107,57,56,119,24,10,11,73,116,64,63,30,
    514,515,
    // note: 76 is missing cause we get it special below
    -1
  };

  if (address_count())
  {
    if (address_get_by_index(index, &device_id, &max_apdu, &src))
    {
      if (object_props[property] < 0)
        next_device = true;
      else
      {
        (void)Send_Read_Property_Request(
          device_id, // destination device
          OBJECT_DEVICE,
          device_id,
          object_props[property],
          BACNET_ARRAY_ALL);
        property++;
      }
    }
    else
      next_device = true;
    if (next_device)
    {
      index++;
      if (index >= MAX_ADDRESS_CACHE)
        index = 0;
      property = 0;
    }
  }

  return;
}

static void LocalIAmHandler(
  uint8_t *service_request,
  uint16_t service_len,
  BACNET_ADDRESS *src)
{
  int len = 0;
  uint32_t device_id = 0;
  unsigned max_apdu = 0;
  int segmentation = 0;
  uint16_t vendor_id = 0;

  (void)src;
  (void)service_len;
  len = iam_decode_service_request(
    service_request,
    &device_id,
    &max_apdu,
    &segmentation,
    &vendor_id);
  fprintf(stderr,"Received I-Am Request");
  if (len != -1)
  {
    fprintf(stderr," from %u!\n",device_id);
    address_add(device_id,
      max_apdu,
      src);
  }
  else
    fprintf(stderr,"!\n");

  return;  
}

static void Init_Device_Parameters(void)
{
  // configure my initial values
  Device_Set_Object_Instance_Number(124);
  Device_Set_Vendor_Name("Lithonia Lighting");
  Device_Set_Vendor_Identifier(42);
  Device_Set_Model_Name("Simple BACnet Client");
  Device_Set_Firmware_Revision("1.00");
  Device_Set_Application_Software_Version("win32");
  Device_Set_Description("Example of a simple BACnet client/server");

  return;
}

static void Init_Service_Handlers(void)
{
  // we need to handle who-is to support dynamic device binding
  apdu_set_unconfirmed_handler(
    SERVICE_UNCONFIRMED_WHO_IS,
    WhoIsHandler);
  apdu_set_unconfirmed_handler(
    SERVICE_UNCONFIRMED_I_AM,
    LocalIAmHandler);

  // set the handler for all the services we don't implement
  // It is required to send the proper reject message...
  apdu_set_unrecognized_service_handler_handler(
    UnrecognizedServiceHandler);
  // we must implement read property - it's required!
  apdu_set_confirmed_handler(
    SERVICE_CONFIRMED_READ_PROPERTY,
    ReadPropertyHandler);
  apdu_set_confirmed_handler(
    SERVICE_CONFIRMED_WRITE_PROPERTY,
    WritePropertyHandler);
  // handle the data coming back from confirmed requests
  apdu_set_confirmed_ack_handler(
    SERVICE_CONFIRMED_READ_PROPERTY,
    ReadPropertyAckHandler);
}

static void print_address(
  char *name,
  BACNET_ADDRESS *dest) // destination address
{
  int i = 0; // counter

  if (dest)
  {
    printf("%s: ",name);
    for (i = 0; i < dest->mac_len; i++)
    {
      printf("%02X",dest->mac[i]);
    }
    printf("\n");
  }
}

static void print_address_cache(void)
{
  unsigned i,j;
  BACNET_ADDRESS address;
  uint32_t device_id = 0;
  unsigned max_apdu = 0;

  fprintf(stderr,"Device\tMAC\tMaxAPDU\tNet\n");
  for (i = 0; i < MAX_ADDRESS_CACHE; i++)
  {
    if (address_get_by_index(i,&device_id, &max_apdu, &address))
    {
      fprintf(stderr,"%u\t",device_id);
      for (j = 0; j < address.mac_len; j++)
      {
        fprintf(stderr,"%02X",address.mac[j]);
      }
      fprintf(stderr,"\t");
      fprintf(stderr,"%hu\t",max_apdu);
      fprintf(stderr,"%hu\n",address.net);
    }
  }
}

int main(int argc, char *argv[])
{
  BACNET_ADDRESS src = {0};  // address where message came from
  uint16_t pdu_len = 0;
  unsigned timeout = 100; // milliseconds
  BACNET_ADDRESS my_address, broadcast_address;

  (void)argc;
  (void)argv;
  Init_Device_Parameters();
  Init_Service_Handlers();
  // init the data link layer
  /* configure standard BACnet/IP port */
  bip_set_port(0xBAC0);
  if (!bip_init())
    return 1;

  datalink_get_broadcast_address(&broadcast_address);
  print_address("Broadcast",&broadcast_address);
  datalink_get_my_address(&my_address);
  print_address("Address",&my_address);

  printf("BACnet stack running...\n");
  // loop forever
  for (;;)
  {
    // input
    //Read_Properties();

    // returns 0 bytes on timeout
    pdu_len = bip_receive(
      &src,
      &Rx_Buf[0],
      MAX_MPDU,
      timeout);

    // process

    if (pdu_len)
    {
      npdu_handler(
        &src,
        &Rx_Buf[0],
        pdu_len);
    }
    if (I_Am_Request)
    {
      I_Am_Request = false;
      Send_IAm();
    } else if (Who_Is_Request)
    {
      Who_Is_Request = false;
      Send_WhoIs();
    }
    else
      Read_Properties();

    // output

    // blink LEDs, Turn on or off outputs, etc

    /* wait for ESC from keyboard before quitting */
    if (kbhit() && (getch() == 0x1B))
      break;
  }

  print_address_cache();

  return 0;
}
