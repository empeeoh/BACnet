/**************************************************************************
*
* Copyright (C) 2006 Steve Karg <skarg@users.sourceforge.net>
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

/** @file epics/main.c  Command line tool to build a full VTS3 EPICS file,
 *                          including the heading information. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>       /* for time */
#include <errno.h>
#include <assert.h>
#include "config.h"
#include "bactext.h"
#include "iam.h"
#include "arf.h"
#include "tsm.h"
#include "address.h"
#include "bacdef.h"
#include "npdu.h"
#include "apdu.h"
#include "device.h"
#include "net.h"
#include "datalink.h"
#include "whois.h"
#include "rp.h"
#include "proplist.h"
/* some demo stuff needed */
#include "filename.h"
#include "handlers.h"
#include "client.h"
#include "txbuf.h"
#include "dlenv.h"
#include "keylist.h"
#include "bacepics.h"


/* (Doxygen note: The next two lines pull all the following Javadoc
 *  into the BACEPICS module.) */
/** @addtogroup BACEPICS
 * @{ */

/** This is the souped-up version of the program for generating EPICS files.
 * It now:
 * 1) Prepends the heading information (supported services, etc)
 * 2) Determines some basic device properties for the header.
 * 3) Postpends the tail information to complete the EPICS file.
 */

/* buffer used for receive */
static uint8_t Rx_Buf[MAX_MPDU] = { 0 };

/* target information converted from command line */
static uint32_t Target_Device_Object_Instance = BACNET_MAX_INSTANCE;
static BACNET_ADDRESS Target_Address;
/* the invoke id is needed to filter incoming messages */
static uint8_t Request_Invoke_ID = 0;
/* loopback address to talk to myself */
/* = { 6, { 127, 0, 0, 1, 0xBA, 0xC0, 0 }, 0 }; */
#if defined(BACDL_BIP)
/* If set, use this as the source port. */
static uint16_t My_BIP_Port = 0;
#endif
static bool Provided_Targ_MAC = false;

/* any errors are picked up in main loop */
static bool Error_Detected = false;
static uint16_t Last_Error_Class = 0;
static uint16_t Last_Error_Code = 0;
/* Counts errors we couldn't get around */
static uint16_t Error_Count = 0;
/* Assume device can do RPM, to start */
static bool Has_RPM = true;
static EPICS_STATES myState = INITIAL_BINDING;

/* any valid RP or RPM data returned is put here */
/* Now using one structure for both RP and RPM data:
 * typedef struct BACnet_RP_Service_Data_t {
 *   bool new_data;
 *   BACNET_CONFIRMED_SERVICE_ACK_DATA service_data;
 *   BACNET_READ_PROPERTY_DATA data;
 *   } BACNET_RP_SERVICE_DATA;
 * static BACNET_RP_SERVICE_DATA Read_Property_Data;
 */

typedef struct BACnet_RPM_Service_Data_t {
    bool new_data;
    BACNET_CONFIRMED_SERVICE_ACK_DATA service_data;
    BACNET_READ_ACCESS_DATA *rpm_data;
} BACNET_RPM_SERVICE_DATA;
static BACNET_RPM_SERVICE_DATA Read_Property_Multiple_Data;

/* We get the length of the object list,
   and then get the objects one at a time */
static uint32_t Object_List_Length = 0;
static int32_t Object_List_Index = 0;
/* object that we are currently printing */
static OS_Keylist Object_List;

/* When we need to process an Object's properties one at a time,
 * then we build and use this list */
#define MAX_PROPS 128   /* Supersized so it always is big enough. */
static uint32_t Property_List_Length = 0;
static uint32_t Property_List_Index = 0;
static int32_t Property_List[MAX_PROPS + 2];

#define INIT_ID_PROPLIST_SIZE 5
/* Define Enums to match the property and value arrays below */
enum init_ids { INIT_VENDOR_NAME, INIT_MODEL_NAME, INIT_DESCRIPTION,
    INIT_OBJ_TYPES
};
static int32_t InitIdPropList[INIT_ID_PROPLIST_SIZE] = {
    PROP_VENDOR_NAME,
    PROP_MODEL_NAME,    /* Have to use this twice, for Model Name and Number */
    PROP_DESCRIPTION,   /* Optional, but hopefully available */
    PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED,
    -1
};

/* Will hold GET_HEADING_RESPONSE results here: */
static BACNET_APPLICATION_DATA_VALUE *InitIdValues[INIT_ID_PROPLIST_SIZE] =
    { NULL, NULL, NULL, NULL, NULL };


/* When we have to walk through an array of things, like ObjectIDs or
 * Subordinate_Annotations, one RP call at a time, use these for indexing.
 */
static uint32_t Walked_List_Length = 0;
static uint32_t Walked_List_Index = 0;
/* TODO: Probably should have done this as additional EPICS_STATES */
static bool Using_Walked_List = false;
/* When requesting RP for BACNET_ARRAY_ALL of what we know can be a long
 * array, then set this true in case it aborts and we need Using_Walked_List */
static bool IsLongArray = false;
/* Show value instead of '?' */
static bool ShowValues = false;
/* read required and optional properties when RPM ALL does not work */
static bool Optional_Properties = false;

#if !defined(PRINT_ERRORS)
#define PRINT_ERRORS 1
#endif

static void MyErrorHandler(
    BACNET_ADDRESS * src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code)
{
    if (address_match(&Target_Address, src) &&
        (invoke_id == Request_Invoke_ID)) {
#if PRINT_ERRORS
        if (ShowValues) {
            fprintf(stderr, "-- BACnet Error: %s: %s\r\n",
                bactext_error_class_name(error_class),
                bactext_error_code_name(error_code));
        }
#endif
        Error_Detected = true;
        Last_Error_Class = error_class;
        Last_Error_Code = error_code;
    }
}

void MyAbortHandler(
    BACNET_ADDRESS * src,
    uint8_t invoke_id,
    uint8_t abort_reason,
    bool server)
{
    (void) server;
    if (address_match(&Target_Address, src) &&
        (invoke_id == Request_Invoke_ID)) {
#if PRINT_ERRORS
        /* It is normal for this to fail, so don't print. */
        if ((myState != GET_ALL_RESPONSE) && !IsLongArray && ShowValues) {
            fprintf(stderr, "-- BACnet Abort: %s \r\n",
                bactext_abort_reason_name(abort_reason));
        }
#endif
        Error_Detected = true;
        Last_Error_Class = ERROR_CLASS_SERVICES;
        if (abort_reason < MAX_BACNET_ABORT_REASON)
            Last_Error_Code =
                (ERROR_CODE_ABORT_BUFFER_OVERFLOW - 1) + abort_reason;
        else
            Last_Error_Code = ERROR_CODE_ABORT_OTHER;
    }
}

void MyRejectHandler(
    BACNET_ADDRESS * src,
    uint8_t invoke_id,
    uint8_t reject_reason)
{
    if (address_match(&Target_Address, src) &&
        (invoke_id == Request_Invoke_ID)) {
#if PRINT_ERRORS
        if (ShowValues) {
            fprintf(stderr, "BACnet Reject: %s\r\n",
                bactext_reject_reason_name(reject_reason));
        }
#endif
        Error_Detected = true;
        Last_Error_Class = ERROR_CLASS_SERVICES;
        if (reject_reason < MAX_BACNET_REJECT_REASON)
            Last_Error_Code =
                (ERROR_CODE_REJECT_BUFFER_OVERFLOW - 1) + reject_reason;
        else
            Last_Error_Code = ERROR_CODE_REJECT_OTHER;
    }
}

void MyReadPropertyAckHandler(
    uint8_t * service_request,
    uint16_t service_len,
    BACNET_ADDRESS * src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA * service_data)
{
    int len = 0;
    BACNET_READ_ACCESS_DATA *rp_data;

    if (address_match(&Target_Address, src) &&
        (service_data->invoke_id == Request_Invoke_ID)) {
        rp_data = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
        if (rp_data) {
            len =
                rp_ack_fully_decode_service_request(service_request,
                service_len, rp_data);
        }
        if (len > 0) {
            memmove(&Read_Property_Multiple_Data.service_data, service_data,
                sizeof(BACNET_CONFIRMED_SERVICE_ACK_DATA));
            Read_Property_Multiple_Data.rpm_data = rp_data;
            Read_Property_Multiple_Data.new_data = true;
        } else {
            if (len < 0)        /* Eg, failed due to no segmentation */
                Error_Detected = true;
            free(rp_data);
        }
    }
}

void MyReadPropertyMultipleAckHandler(
    uint8_t * service_request,
    uint16_t service_len,
    BACNET_ADDRESS * src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA * service_data)
{
    int len = 0;
    BACNET_READ_ACCESS_DATA *rpm_data;

    if (address_match(&Target_Address, src) &&
        (service_data->invoke_id == Request_Invoke_ID)) {
        rpm_data = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
        if (rpm_data) {
            len =
                rpm_ack_decode_service_request(service_request, service_len,
                rpm_data);
        }
        if (len > 0) {
            memmove(&Read_Property_Multiple_Data.service_data, service_data,
                sizeof(BACNET_CONFIRMED_SERVICE_ACK_DATA));
            Read_Property_Multiple_Data.rpm_data = rpm_data;
            Read_Property_Multiple_Data.new_data = true;
            /* Will process and free the RPM data later */
        } else {
            if (len < 0)        /* Eg, failed due to no segmentation */
                Error_Detected = true;
            free(rpm_data);
        }
    }
}

static void Init_Service_Handlers(
    void)
{
    Device_Init(NULL);

#if BAC_ROUTING
    uint32_t Object_Instance;
    BACNET_CHARACTER_STRING name_string;
    /* Put this client Device into the Routing table (first entry) */
    Object_Instance = Device_Object_Instance_Number();
    Device_Object_Name(Object_Instance, &name_string);
    Add_Routed_Device(Object_Instance, &name_string, Device_Description());
#endif

    /* we need to handle who-is
       to support dynamic device binding to us */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    /* handle i-am to support binding to other devices */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
    /* set the handler for all the services we don't implement
       It is required to send the proper reject message... */
    apdu_set_unrecognized_service_handler_handler
        (handler_unrecognized_service);
    /* we must implement read property - it's required! */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,
        handler_read_property);
    /* handle the data coming back from confirmed requests */
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROPERTY,
        MyReadPropertyAckHandler);
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
        MyReadPropertyMultipleAckHandler);
    /* handle any errors coming back */
    apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROPERTY, MyErrorHandler);
    apdu_set_abort_handler(MyAbortHandler);
    apdu_set_reject_handler(MyRejectHandler);
}


/** Determine if this is a writable property, and, if so,
 * note that in the EPICS output.
 * This function may need a lot of customization for different implementations.
 *
 * @param object_type [in] The BACnet Object type of this object.
 * @note  object_instance [in] The ID number for this object.
 * @param rpm_property [in] Points to structure holding the Property,
 *                          Value, and Error information.
 */
void CheckIsWritableProperty(
    BACNET_OBJECT_TYPE object_type,
    /* uint32_t object_instance, */
    BACNET_PROPERTY_REFERENCE * rpm_property)
{
    bool bIsWritable = false;
    if ((object_type == OBJECT_ANALOG_OUTPUT) ||
        (object_type == OBJECT_BINARY_OUTPUT) ||
        (object_type == OBJECT_COMMAND) ||
        (object_type == OBJECT_MULTI_STATE_OUTPUT) ||
        (object_type == OBJECT_ACCESS_DOOR)) {
        if (rpm_property->propertyIdentifier == PROP_PRESENT_VALUE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_AVERAGING) {
        if ((rpm_property->propertyIdentifier == PROP_ATTEMPTED_SAMPLES) ||
            (rpm_property->propertyIdentifier == PROP_WINDOW_INTERVAL) ||
            (rpm_property->propertyIdentifier == PROP_WINDOW_SAMPLES)) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_FILE) {
        if (rpm_property->propertyIdentifier == PROP_ARCHIVE) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_LIFE_SAFETY_POINT) ||
        (object_type == OBJECT_LIFE_SAFETY_ZONE)) {
        if (rpm_property->propertyIdentifier == PROP_MODE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_PROGRAM) {
        if (rpm_property->propertyIdentifier == PROP_PROGRAM_CHANGE) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_PULSE_CONVERTER) {
        if (rpm_property->propertyIdentifier == PROP_ADJUST_VALUE) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_TRENDLOG) ||
        (object_type == OBJECT_EVENT_LOG) ||
        (object_type == OBJECT_TREND_LOG_MULTIPLE)) {
        if ((rpm_property->propertyIdentifier == PROP_ENABLE) ||
            (rpm_property->propertyIdentifier == PROP_RECORD_COUNT)) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_LOAD_CONTROL) {
        if ((rpm_property->propertyIdentifier == PROP_REQUESTED_SHED_LEVEL) ||
            (rpm_property->propertyIdentifier == PROP_START_TIME) ||
            (rpm_property->propertyIdentifier == PROP_SHED_DURATION) ||
            (rpm_property->propertyIdentifier == PROP_DUTY_WINDOW) ||
            (rpm_property->propertyIdentifier == PROP_SHED_LEVELS)) {
            bIsWritable = true;
        }
    } else if ((object_type == OBJECT_ACCESS_ZONE) ||
        (object_type == OBJECT_ACCESS_USER) ||
        (object_type == OBJECT_ACCESS_RIGHTS) ||
        (object_type == OBJECT_ACCESS_CREDENTIAL)) {
        if (rpm_property->propertyIdentifier == PROP_GLOBAL_IDENTIFIER) {
            bIsWritable = true;
        }
    } else if (object_type == OBJECT_NETWORK_SECURITY) {
        if ((rpm_property->propertyIdentifier == PROP_BASE_DEVICE_SECURITY_POLICY) ||
            (rpm_property->propertyIdentifier == PROP_NETWORK_ACCESS_SECURITY_POLICIES) ||
            (rpm_property->propertyIdentifier == PROP_SECURITY_TIME_WINDOW) ||
            (rpm_property->propertyIdentifier == PROP_PACKET_REORDER_TIME) ||
            (rpm_property->propertyIdentifier == PROP_LAST_KEY_SERVER) ||
            (rpm_property->propertyIdentifier == PROP_SECURITY_PDU_TIMEOUT) ||
            (rpm_property->propertyIdentifier == PROP_DO_NOT_HIDE)) {
            bIsWritable = true;
        }
    }
    /* Add more checking here, eg for Time_Synchronization_Recipients,
     * Manual_Slave_Address_Binding, Object_Property_Reference,
     * Life Safety Tracking_Value, Reliability, Mode,
     * or Present_Value when Out_Of_Service is TRUE.
     */
    if (bIsWritable)
        fprintf(stdout, " Writable");
}


/** Provide a nicer output for Supported Services and Object Types bitfields
 * and Date fields.
 * We have to override the library's normal bitfield print because the
 * EPICS format wants just T and F, and we want to provide (as comments)
 * the names of the active types.
 * These bitfields use opening and closing parentheses instead of braces.
 * We also limit the output to 4 bit fields per line.
 *
 * @param stream [in] Normally stdout
 * @param object_value [in] The structure holding this property's description
 *                          and value.
 * @return True if success.  Or otherwise.
 */

bool PrettyPrintPropertyValue(
    FILE * stream,
    BACNET_OBJECT_PROPERTY_VALUE * object_value)
{
    BACNET_APPLICATION_DATA_VALUE *value = NULL;
    bool status = true; /*return value */
    size_t len = 0, i = 0, j = 0;
    BACNET_PROPERTY_ID property = PROP_ALL;
    char short_month[4];

    value = object_value->value;
    property = object_value->object_property;
    if ((value != NULL) && (value->tag == BACNET_APPLICATION_TAG_BIT_STRING) &&
        ((property == PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED) ||
            (property == PROP_PROTOCOL_SERVICES_SUPPORTED))) {
        len = bitstring_bits_used(&value->type.Bit_String);
        fprintf(stream, "( \r\n        ");
        for (i = 0; i < len; i++) {
            fprintf(stream, "%s", bitstring_bit(&value->type.Bit_String,
                    (uint8_t) i) ? "T" : "F");
            if (i < len - 1)
                fprintf(stream, ",");
            else
                fprintf(stream, " ");
            /* Tried with 8 per line, but with the comments, got way too long. */
            if ((i == (len - 1)) || ((i % 4) == 3)) {   /* line break every 4 */
                fprintf(stream, "   -- ");      /* EPICS comments begin with "--" */
                /* Now rerun the same 4 bits, but print labels for true ones */
                for (j = i - (i % 4); j <= i; j++) {
                    if (bitstring_bit(&value->type.Bit_String, (uint8_t) j)) {
                        if (property == PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED)
                            fprintf(stream, " %s,",
                                bactext_object_type_name(j));
                        /* PROP_PROTOCOL_SERVICES_SUPPORTED */
                        else {
                            bool bIsConfirmed;
                            size_t newIndex;
                            if (apdu_service_supported_to_index(j, &newIndex,
                                    &bIsConfirmed)) {
                                if (bIsConfirmed)
                                    fprintf(stream, " %s,",
                                        bactext_confirmed_service_name
                                        (newIndex));

                                else
                                    fprintf(stream, " %s,",
                                        bactext_unconfirmed_service_name(
                                            (newIndex)));

                            }
                        }
                    } else      /* not supported */
                        fprintf(stream, ",");
                }
                fprintf(stream, "\r\n        ");
            }
        }
        fprintf(stream, ") \r\n");
    } else if ((value != NULL) && (value->tag == BACNET_APPLICATION_TAG_DATE)) {
        /* eg, property == PROP_LOCAL_DATE
         * VTS needs (3-Aug-2011,4) or (8/3/11,4), so we'll use the
         * clearer, international form. */
        strncpy(short_month, bactext_month_name(value->type.Date.month), 3);
        short_month[3] = 0;
        fprintf(stream, "(%u-%3s-%u, %u)", (unsigned) value->type.Date.day,
            short_month, (unsigned) value->type.Date.year,
            (unsigned) value->type.Date.wday);
    } else if (value != NULL) {
        assert(false);  /* How did I get here?  Fix your code. */
        /* Meanwhile, a fallback plan */
        status = bacapp_print_value(stdout, object_value);
    } else
        fprintf(stream, "? \r\n");

    return status;
}


/** Print out the value(s) for one Property.
 * This function may be called repeatedly for one property if we are walking
 * through a list (Using_Walked_List is True) to show just one value of the
 * array per call.
 *
 * @param object_type [in] The BACnet Object type of this object.
 * @param object_instance [in] The ID number for this object.
 * @param rpm_property [in] Points to structure holding the Property,
 *                          Value, and Error information.
 */
void PrintReadPropertyData(
    BACNET_OBJECT_TYPE object_type,
    uint32_t object_instance,
    BACNET_PROPERTY_REFERENCE * rpm_property)
{
    BACNET_OBJECT_PROPERTY_VALUE object_value;  /* for bacapp printing */
    BACNET_APPLICATION_DATA_VALUE *value, *old_value;
    bool print_brace = false;
    KEY object_list_element;
    bool isSequence = false;    /* Ie, will need bracketing braces {} */

    if (rpm_property == NULL) {
        fprintf(stdout, "    -- Null Property data \r\n");
        return;
    }
    value = rpm_property->value;
    if (value == NULL) {
        /* Then we print the error information */
        fprintf(stdout, "?  -- BACnet Error: %s: %s\r\n",
            bactext_error_class_name((int) rpm_property->error.error_class),
            bactext_error_code_name((int) rpm_property->error.error_code));
        return;
    }
    object_value.object_type = object_type;
    object_value.object_instance = object_instance;
    if ((value != NULL) && (value->next != NULL)) {
        /* Then this is an array of values.
         * But are we showing Values?  We (VTS3) want ? instead of {?,?} to show up. */
        switch (rpm_property->propertyIdentifier) {
                /* Screen the Properties that can be arrays or Sequences */
            case PROP_PRESENT_VALUE:
            case PROP_PRIORITY_ARRAY:
                if (!ShowValues) {
                    fprintf(stdout, "? \r\n");
                    /* We want the Values freed below, but don't want to
                     * print anything for them.  To achieve this, swap
                     * out the Property for a non-existent Property
                     * and catch that below.  */
                    rpm_property->propertyIdentifier =
                        PROP_PROTOCOL_CONFORMANCE_CLASS;
                    break;
                }
                if (object_type == OBJECT_DATETIME_VALUE)
                    break;      /* A special case - no braces for this pair */
                /* Else, fall through to normal processing. */
            default:
                /* Normal array: open brace */
                fprintf(stdout, "{ ");
                print_brace = true;     /* remember to close it */
                break;
        }
    }

    if (!Using_Walked_List)
        Walked_List_Index = Walked_List_Length = 0;     /* In case we need this. */
    /* value(s) loop until there is no "next" ... */
    while (value != NULL) {
        object_value.object_property = rpm_property->propertyIdentifier;
        object_value.array_index = rpm_property->propertyArrayIndex;
        object_value.value = value;
        switch (rpm_property->propertyIdentifier) {
                /* These are all arrays, so they open and close with braces */
            case PROP_OBJECT_LIST:
            case PROP_STATE_TEXT:
            case PROP_STRUCTURED_OBJECT_LIST:
            case PROP_SUBORDINATE_ANNOTATIONS:
            case PROP_SUBORDINATE_LIST:
                if (Using_Walked_List) {
                    if ((rpm_property->propertyArrayIndex == 0) &&
                        (value->tag == BACNET_APPLICATION_TAG_UNSIGNED_INT)) {
                        /* Grab the value of the Object List length - don't print it! */
                        Walked_List_Length = value->type.Unsigned_Int;
                        if (rpm_property->propertyIdentifier ==
                            PROP_OBJECT_LIST)
                            Object_List_Length = value->type.Unsigned_Int;
                        break;
                    } else
                        assert(Walked_List_Index == (uint32_t)
                            rpm_property->propertyArrayIndex);
                } else {
                    Walked_List_Index++;
                    /* If we got the whole Object List array in one RP call, keep
                     * the Index and List_Length in sync as we cycle through. */
                    if (rpm_property->propertyIdentifier == PROP_OBJECT_LIST)
                        Object_List_Length = ++Object_List_Index;
                }
                if (Walked_List_Index == 1) {
                    /* If the array is empty (nothing for this first entry),
                     * Make it VTS3-friendly and don't show "Null" as a value. */
                    if (value->tag == BACNET_APPLICATION_TAG_NULL) {
                        fprintf(stdout, "?\r\n        ");
                        break;
                    }

                    /* Open this Array of Objects for the first entry (unless
                     * opening brace has already printed, since this is an array
                     * of values[] ) */
                    if (value->next == NULL)
                        fprintf(stdout, "{ \r\n        ");
                    else
                        fprintf(stdout, "\r\n        ");
                }

                if (rpm_property->propertyIdentifier == PROP_OBJECT_LIST) {
                    if (value->tag != BACNET_APPLICATION_TAG_OBJECT_ID) {
                        assert(value->tag == BACNET_APPLICATION_TAG_OBJECT_ID); /* Something not right here */
                        break;
                    }
                    /* Store the object list so we can interrogate
                       each object. */
                    object_list_element =
                        KEY_ENCODE(value->type.Object_Id.type,
                        value->type.Object_Id.instance);
                    /* We don't have anything to put in the data pointer
                     * yet, so just leave it null.  The key is Key here. */
                    Keylist_Data_Add(Object_List, object_list_element, NULL);
                } else if (rpm_property->propertyIdentifier == PROP_STATE_TEXT) {
                    /* Make sure it fits within 31 chars for original VTS3 limitation.
                     * If longer, take first 15 dash, and last 15 chars. */
                    if (value->type.Character_String.length > 31) {
                        int iLast15idx =
                            value->type.Character_String.length - 15;
                        value->type.Character_String.value[15] = '-';
                        memcpy(&value->type.Character_String.value[16],
                            &value->type.Character_String.value[iLast15idx],
                            15);
                        value->type.Character_String.value[31] = 0;
                        value->type.Character_String.length = 31;
                    }
                } else if (rpm_property->propertyIdentifier ==
                    PROP_SUBORDINATE_LIST) {
                    if (value->tag != BACNET_APPLICATION_TAG_OBJECT_ID) {
                        assert(value->tag == BACNET_APPLICATION_TAG_OBJECT_ID); /* Something not right here */
                        break;
                    }
                    /* TODO: handle Sequence of { Device ObjID, Object ID }, */
                    isSequence = true;
                }

                /* If the object is a Sequence, it needs its own bracketing braces */
                if (isSequence)
                    fprintf(stdout, "{");
                bacapp_print_value(stdout, &object_value);
                if (isSequence)
                    fprintf(stdout, "}");

                if ((Walked_List_Index < Walked_List_Length) ||
                    (value->next != NULL)) {
                    /* There are more. */
                    fprintf(stdout, ", ");
                    if (!(Walked_List_Index % 4))
                        fprintf(stdout, "\r\n        ");
                } else {
                    fprintf(stdout, " } \r\n");
                }
                break;

            case PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED:
            case PROP_PROTOCOL_SERVICES_SUPPORTED:
                PrettyPrintPropertyValue(stdout, &object_value);
                break;

                /* Our special non-existent case; do nothing further here. */
            case PROP_PROTOCOL_CONFORMANCE_CLASS:
                break;

            default:
                /* First, if this is a date type, it needs a different format
                 * for VTS, so pretty print it. */
                if (ShowValues &&
                    (object_value.value->tag == BACNET_APPLICATION_TAG_DATE)) {
                    /* This would be PROP_LOCAL_DATE, or OBJECT_DATETIME_VALUE,
                     * or OBJECT_DATE_VALUE                     */
                    PrettyPrintPropertyValue(stdout, &object_value);
                } else {
                    /* Some properties are presented just as '?' in an EPICS;
                     * screen these out here, unless ShowValues is true.  */
                    switch (rpm_property->propertyIdentifier) {
                        case PROP_DEVICE_ADDRESS_BINDING:
                            /* Make it VTS3-friendly and don't show "Null"
                             * as a value. */
                            if (value->tag == BACNET_APPLICATION_TAG_NULL) {
                                fprintf(stdout, "?");
                                break;
                            }
                            /* Else, fall through for normal processing. */
                        case PROP_DAYLIGHT_SAVINGS_STATUS:
                        case PROP_LOCAL_TIME:
                        case PROP_LOCAL_DATE:  /* Only if !ShowValues */
                        case PROP_PRESENT_VALUE:
                        case PROP_PRIORITY_ARRAY:
                        case PROP_RELIABILITY:
                        case PROP_UTC_OFFSET:
                        case PROP_DATABASE_REVISION:
                            if (!ShowValues) {
                                fprintf(stdout, "?");
                                break;
                            }
                            /* Else, fall through and print value: */
                        default:
                            bacapp_print_value(stdout, &object_value);
                            break;
                    }
                }
                if (value->next != NULL) {
                    /* there's more! */
                    fprintf(stdout, ",");
                } else {
                    if (print_brace) {
                        /* Closing brace for this multi-valued array */
                        fprintf(stdout, " }");
                    }
                    CheckIsWritableProperty(object_type,        /* object_instance, */
                        rpm_property);
                    fprintf(stdout, "\r\n");
                }
                break;
        }

        old_value = value;
        value = value->next;    /* next or NULL */
        free(old_value);
    }   /* End while loop */

}

/** Print the property identifier name to stdout,
 *  handling the proprietary property numbers.
 * @param propertyIdentifier [in] The property identifier number.
 */
void Print_Property_Identifier(
    unsigned propertyIdentifier)
{
    if (propertyIdentifier < 512) {
        fprintf(stdout, "%s", bactext_property_name(propertyIdentifier));
    } else {
        fprintf(stdout, "proprietary %u", propertyIdentifier);
    }
}

/** Build a list of properties to request with RPM.
 * @param rpm_object [out] The structure holding our linked list of properties to request.
 * @param propList [in] Simple list of properties (ptr to array), terminated with -1
 */
void BuildPropRequest(
    BACNET_READ_ACCESS_DATA * rpm_object,
    int32_t * propList)
{
    int i;
    /* To start with, StartNextObject() has prepopulated one propEntry,
     * but we will overwrite it and link more to it
     */
    BACNET_PROPERTY_REFERENCE *propEntry = rpm_object->listOfProperties;
    BACNET_PROPERTY_REFERENCE *oldEntry = rpm_object->listOfProperties;
    for (i = 0; propList[i] != -1; i++) {
        if (propEntry == NULL) {
            propEntry = calloc(1, sizeof(BACNET_PROPERTY_REFERENCE));
            assert(propEntry);
            oldEntry->next = propEntry;
        }
        propEntry->propertyIdentifier = propList[i];
        propEntry->propertyArrayIndex = BACNET_ARRAY_ALL;
        oldEntry = propEntry;
        propEntry = NULL;
    }
}

/** Send an RP request to read one property from the current Object.
 * Singly process large arrays too, like the Device Object's Object_List.
 * If GET_LIST_OF_ALL_RESPONSE failed, we will fall back to using just
 * the list of known Required properties for this type of object.
 *
 * @param device_instance [in] Our target device's instance.
 * @param pMyObject [in] The current Object's type and instance numbers.
 * @return The invokeID of the message sent, or 0 if reached the end
 *         of the property list.
 */
static uint8_t Read_Properties(
    uint32_t device_instance,
    BACNET_OBJECT_ID * pMyObject)
{
    uint8_t invoke_id = 0;
    struct special_property_list_t PropertyListStruct;
    unsigned int i = 0, j = 0;

    if ((!Has_RPM && (Property_List_Index == 0)) ||
        (Property_List_Length == 0)) {
        /* If we failed to get the Properties with RPM, just settle for what we
         * know is the fixed list of Required and Optional properties.
         * In practice, this should only happen for simple devices that don't
         * implement RPM or have really limited MAX_APDU size.
         */
        property_list_special(pMyObject->type, &PropertyListStruct);
        if (Optional_Properties) {
            Property_List_Length =
                PropertyListStruct.Required.count +
                PropertyListStruct.Optional.count;
        } else {
            Property_List_Length = PropertyListStruct.Required.count;
        }
        if (Property_List_Length > MAX_PROPS) {
            Property_List_Length = MAX_PROPS;
        }
        /* Copy this list for later one-by-one processing */
        for (i = 0; i < Property_List_Length; i++) {
            if (i < PropertyListStruct.Required.count) {
                Property_List[i] = PropertyListStruct.Required.pList[i];
            } else if (Optional_Properties) {
                Property_List[i] = PropertyListStruct.Optional.pList[j];
                j++;
            }
        }
        /* Just to be sure we terminate */
        Property_List[i] = -1;
    }
    if (Property_List[Property_List_Index] != -1) {
        int prop = Property_List[Property_List_Index];
        uint32_t array_index;
        IsLongArray = false;
        if (Using_Walked_List) {
            if (Walked_List_Length == 0) {
                array_index = 0;
            } else {
                array_index = Walked_List_Index;
            }
        } else {
            fprintf(stdout, "    ");
            Print_Property_Identifier(prop);
            fprintf(stdout, ": ");
            array_index = BACNET_ARRAY_ALL;

            switch (prop) {
                    /* These are all potentially long arrays, so they may abort */
                case PROP_OBJECT_LIST:
                case PROP_STATE_TEXT:
                case PROP_STRUCTURED_OBJECT_LIST:
                case PROP_SUBORDINATE_ANNOTATIONS:
                case PROP_SUBORDINATE_LIST:
                    IsLongArray = true;
                    break;
            }
        }
        invoke_id =
            Send_Read_Property_Request(device_instance, pMyObject->type,
            pMyObject->instance, prop, array_index);

    }

    return invoke_id;
}

/** Process the RPM list, either printing out on success or building a
 *  properties list for later use.
 *  Also need to free the data in the list.
 *  If the present state is GET_HEADING_RESPONSE, store the results
 *  in globals for later use.
 * @param rpm_data [in] The list of RPM data received.
 * @param myState [in] The current state.
 * @return The next state of the EPICS state machine, normally NEXT_OBJECT
 *         if the RPM got good data, or GET_PROPERTY_REQUEST if we have to
 *         singly process the list of Properties.
 */
EPICS_STATES ProcessRPMData(
    BACNET_READ_ACCESS_DATA * rpm_data,
    EPICS_STATES myState)
{
    BACNET_READ_ACCESS_DATA *old_rpm_data;
    BACNET_PROPERTY_REFERENCE *rpm_property;
    BACNET_PROPERTY_REFERENCE *old_rpm_property;
    BACNET_APPLICATION_DATA_VALUE *value;
    BACNET_APPLICATION_DATA_VALUE *old_value;
    bool bSuccess = true;
    EPICS_STATES nextState = myState;   /* assume no change */
    /* Some flags to keep the output "pretty" -
     * wait and put these object lists at the end */
    bool bHasObjectList = false;
    bool bHasStructuredViewList = false;
    int i = 0;

    while (rpm_data) {
        rpm_property = rpm_data->listOfProperties;
        while (rpm_property) {
            /* For the GET_LIST_OF_ALL_RESPONSE case,
             * just keep what property this was */
            if (myState == GET_LIST_OF_ALL_RESPONSE) {
                switch (rpm_property->propertyIdentifier) {
                    case PROP_OBJECT_LIST:
                        bHasObjectList = true;  /* Will append below */
                        break;
                    case PROP_STRUCTURED_OBJECT_LIST:
                        bHasStructuredViewList = true;
                        break;
                    default:
                        Property_List[Property_List_Index] =
                            rpm_property->propertyIdentifier;
                        Property_List_Index++;
                        Property_List_Length++;
                        break;
                }
                /* Free up the value(s) */
                value = rpm_property->value;
                while (value) {
                    old_value = value;
                    value = value->next;
                    free(old_value);
                }
            } else if (myState == GET_HEADING_RESPONSE) {
                InitIdValues[i++] = rpm_property->value;
                /* copy this pointer.
                 * On error, the pointer will be null
                 * We won't free these values*/
            } else {
                fprintf(stdout, "    ");
                Print_Property_Identifier(rpm_property->propertyIdentifier);
                fprintf(stdout, ": ");
                PrintReadPropertyData(rpm_data->object_type,
                    rpm_data->object_instance, rpm_property);
            }
            old_rpm_property = rpm_property;
            rpm_property = rpm_property->next;
            free(old_rpm_property);
        }
        old_rpm_data = rpm_data;
        rpm_data = rpm_data->next;
        free(old_rpm_data);
    }

    /* Now determine the next state */
    if (myState == GET_HEADING_RESPONSE)
        nextState = PRINT_HEADING;
    /* press ahead with or without the data */
    else if (bSuccess && (myState == GET_ALL_RESPONSE))
        nextState = NEXT_OBJECT;
    else if (bSuccess) {        /* and GET_LIST_OF_ALL_RESPONSE */
        /* Now append the properties we waited on. */
        if (bHasStructuredViewList) {
            Property_List[Property_List_Index] = PROP_STRUCTURED_OBJECT_LIST;
            Property_List_Index++;
            Property_List_Length++;
        }
        if (bHasObjectList) {
            Property_List[Property_List_Index] = PROP_OBJECT_LIST;
            Property_List_Index++;
            Property_List_Length++;
        }
        /* Now insert the -1 list terminator, but don't count it. */
        Property_List[Property_List_Index] = -1;
        assert(Property_List_Length < MAX_PROPS);
        Property_List_Index = 0;        /* Will start at top of the list */
        nextState = GET_PROPERTY_REQUEST;
    }
    return nextState;
}

void PrintUsage(
    )
{
    printf
        ("bacepics -- Generates Full EPICS file, including Object and Property List \r\n");
    printf("Usage: \r\n");
    printf
        ("  bacepics [-v] [-p sport] [-t target_mac [-n dnet]] device-instance \r\n");
    printf("    -v: show values instead of '?' \r\n");
    printf
        ("    -p: Use sport for \"my\" port, instead of 0xBAC0 (BACnet/IP only) \r\n");
    printf("        Allows you to communicate with a localhost target. \r\n");
    printf
        ("    -t: declare target's MAC instead of using Who-Is to bind to  \r\n");
    printf
        ("        device-instance. Format is \"C0:A8:00:18:BA:C0\" (as usual) \r\n");
    printf("        Use \"7F:00:00:01:BA:C0\" for loopback testing \r\n");
    printf("    -n: specify target's DNET if not local BACnet network  \r\n");
    printf("        or on routed Virtual Network \r\n");
    printf("\r\n");
    printf
        ("You may want to redirect the output to a .tpi file for VTS use,\r\n");
    printf("    eg, bacepics -v 2701876 > epics-2701876.tpi \r\n");
    printf("\r\n");
    exit(0);
}

int CheckCommandLineArgs(
    int argc,
    char *argv[])
{
    int i;
    bool bFoundTarget = false;
    /* FIXME: handle multi homed systems - use an argument passed to the datalink_init() */

    /* print help if not enough arguments */
    if (argc < 2) {
        fprintf(stdout, "Error: Must provide a device-instance \r\n\r\n");
        PrintUsage();   /* Will exit */
    }
    for (i = 1; i < argc; i++) {
        char *anArg = argv[i];
        if (anArg[0] == '-') {
            switch (anArg[1]) {
                case 'o':
                    Optional_Properties = true;
                    break;
                case 'v':
                    ShowValues = true;
                    break;
                case 'p':
                    if (++i < argc) {
#if defined(BACDL_BIP)
                        My_BIP_Port = (uint16_t) strtol(argv[i], NULL, 0);
                        /* Used strtol so sport can be either 0xBAC0 or 47808 */
#endif
                    }
                    break;
                case 'n':
                    /* Destination Network Number */
                    if (Target_Address.mac_len == 0)
                        fprintf(stderr,
                            "Must provide a Target MAC before DNET \r\n");
                    if (++i < argc)
                        Target_Address.net =
                            (uint16_t) strtol(argv[i], NULL, 0);
                    /* Used strtol so dest.net can be either 0x1234 or 4660 */
                    break;
                case 't':
                    if (++i < argc) {
                        /* decoded MAC addresses */
                        unsigned mac[6];
                        /* number of successful decodes */
                        int count;
                        /* loop counter */
                        unsigned j;
                        count =
                            sscanf(argv[i], "%x:%x:%x:%x:%x:%x", &mac[0],
                            &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
                        if (count == 6) {       /* success */
                            Target_Address.mac_len = count;
                            for (j = 0; j < 6; j++) {
                                Target_Address.mac[j] = (uint8_t) mac[j];
                            }
                            Target_Address.net = 0;
                            Target_Address.len = 0;     /* No src address */
                            Provided_Targ_MAC = true;
                            break;
                        } else
                            printf("ERROR: invalid Target MAC %s \r\n",
                                argv[i]);
                        /* And fall through to PrintUsage */
                    }
                    /* Either break or fall through, as above */
                    /* break; */
                default:
                    PrintUsage();
                    break;
            }
        } else {
            /* decode the Target Device Instance parameter */
            Target_Device_Object_Instance = strtol(anArg, NULL, 0);
            if (Target_Device_Object_Instance > BACNET_MAX_INSTANCE) {
                fprintf(stdout,
                    "Error: device-instance=%u - it must be less than %u\r\n",
                    Target_Device_Object_Instance, BACNET_MAX_INSTANCE + 1);
                PrintUsage();
            }
            bFoundTarget = true;
        }
    }
    if (!bFoundTarget) {
        fprintf(stdout, "Error: Must provide a device-instance \r\n\r\n");
        PrintUsage();   /* Will exit */
    }

    return 0;   /* All OK if we reach here */
}


void PrintHeading(
    )
{
    BACNET_APPLICATION_DATA_VALUE *value;
    char *relation = "for";     /* Text for Gateways */
    if (Target_Address.net != 0)
        relation = "provided by";       /* Text for child routed devices */

    printf("PICS 0\r\n");
    printf("BACnet Protocol Implementation Conformance Statement\r\n\r\n");

    printf("--\r\n--\r\n");
    printf("-- BACnet/IP Interface for BACnet-stack Devices\r\n");
    printf("-- http://sourceforge.net/projects/bacnet/ \r\n");
    printf("-- \r\n-- \r\n\r\n");
/*	InitIdValues  , , , */

    value = InitIdValues[INIT_VENDOR_NAME];
    if (value != NULL)
        printf("Vendor Name: \"%s\"\r\n",
            characterstring_value(&value->type.Character_String));
    else
        printf("Vendor Name: \"bacnet-stack\"\r\n");

    value = InitIdValues[INIT_MODEL_NAME];
    /* Best we can do with Product Name and Model Number is use the same text */
    if (value != NULL) {
        printf("Product Name: \"%s\"\r\n",
            characterstring_value(&value->type.Character_String));
        printf("Product Model Number: \"%s\"\r\n",
            characterstring_value(&value->type.Character_String));
    } else {
        printf("Product Name: \"bacnet-stack Device\"\r\n");
        printf("Product Model Number: \"Model XXX\"\r\n");
    }

    value = InitIdValues[INIT_DESCRIPTION];
    if (value != NULL)
        printf("Product Description: \"%s\"\r\n\r\n",
            characterstring_value(&value->type.Character_String));
    else
        printf("Product Description: \"bacnet-stack Demo Device\"\r\n\r\n");

    printf("BIBBs Supported:\r\n");
    printf("{\r\n");
    printf(" DS-RP-B\r\n");
    printf(" DS-RPM-B\r\n");
    printf(" DS-WP-B\r\n");
    printf(" DM-DDB-B\r\n");
    printf(" DM-DOB-B\r\n");
    printf(" DM-DCC-B\r\n");
    printf(" DM-RD-B\r\n");
#ifdef BAC_ROUTING
    /* Next line only for the gateway (ie, if not addressing a subNet) */
    if (Target_Address.net == 0)
        printf(" NM-RC-B\r\n");
#endif
    printf("}\r\n\r\n");
    /* You might add some of:
     *      -- DS-COV-A DS-COV-B
     *      -- AE-N-A AE-N-I-B AE-N-E-B
     *      -- AE-ACK-A AE-ACK-B
     *      -- DM-UTC-B
     */

    printf("BACnet Standard Application Services Supported:\r\n");
    printf("{\r\n");
    /* You might change the "Initiate Execute" values, or edit the result. */
    printf(" ReadProperty                   Initiate Execute\r\n");
    printf(" ReadPropertyMultiple           Execute\r\n");
    printf(" WriteProperty                  Execute\r\n");
    printf(" DeviceCommunicationControl     Execute\r\n");
    printf(" Who-Has                        Execute\r\n");
    printf(" I-Have                         Initiate\r\n");
    printf(" Who-Is                         Initiate Execute\r\n");
    printf(" I-Am                           Initiate\r\n");
    printf(" ReinitializeDevice             Execute\r\n");
#ifdef BAC_ROUTING
    if (Target_Address.net == 0) {
        printf(" -- Note: The following Routing Services are Supported:\r\n");
        printf(" -- Who-Is-Router-To-Network    Initiate Execute\r\n");
        printf(" -- I-Am-Router-To-Network      Initiate Execute\r\n");
        printf(" -- Initialize-Routing-Table    Execute\r\n");
        printf(" -- Initialize-Routing-Table-Ack Initiate\r\n");
    }
#endif
    printf("}\r\n\r\n");
    /* You might want to add some of:
     *      -- AcknowledgeAlarm               Initiate Execute
     *      -- ConfirmedCOVNotification       Initiate Execute
     *      -- UnconfirmedCOVNotification     Initiate
     *      -- ConfirmedEventNotification     Initiate Execute
     *      -- UnconfirmedEventNotification   Initiate Execute
     *      -- GetAlarmSummary                Initiate Execute
     *      -- GetEnrollmentSummary           Initiate Execute
     *      -- WritePropertyMultiple          Initiate Execute
     *      -- ReadRange                      Initiate Execute
     *      -- GetEventInformation            Initiate Execute
     *      -- SubscribeCOVProperty           Initiate Execute
     */

    printf("Standard Object-Types Supported:\r\n");
    printf("{\r\n");
    value = InitIdValues[INIT_OBJ_TYPES];
    /* We have to process this bit string and determine which Object Types we have,
     * and show them
     */
    if ((value != NULL) && (value->tag == BACNET_APPLICATION_TAG_BIT_STRING)) {
        int i, len = bitstring_bits_used(&value->type.Bit_String);
        for (i = 0; i < len; i++) {
            if (bitstring_bit(&value->type.Bit_String, (uint8_t) i))
                printf(" %s\r\n", bactext_object_type_name(i));
        }
    } else {    /* Else, just show the usual suspects */
        printf(" Analog Input\r\n");
        printf(" Analog Value\r\n");
        printf(" Binary Input\r\n");
        printf(" Binary Value\r\n");
        printf(" Device\r\n");
        printf(" Multi-state Input\r\n");
        printf(" Multi-state Value\r\n");
        printf(" Structured View\r\n");
        printf(" Characterstring Value\r\n");
        printf(" Datetime Value\r\n");
        printf(" Integer Value\r\n");
        printf(" Positive Integer Value\r\n");
    }
    printf("}\r\n\r\n");
    /* You might add some of:
     *      -- Analog Output
     *      -- Binary Output
     *      -- Multi-state Output
     *      -- Trend Log                      Createable Deleteable
     *      -- Load Control
     *      -- Bitstring Value
     *      -- Date Pattern Value
     *      -- Date Value
     *      -- Datetime Pattern Value
     *      -- Large Analog Value
     *      -- Octetstring Value
     *      -- Time Pattern Value
     *      -- Time Value
     */

    printf("Data Link Layer Option:\r\n");
    printf("{\r\n");
    printf(" BACnet/IP, 'DIX' Ethernet\r\n");
    printf("}\r\n\r\n");

    printf("Character Sets Supported:\r\n");
    printf("{\r\n");
    printf(" ANSI X3.4\r\n");
    printf("}\r\n\r\n");

    printf("Special Functionality:\r\n");
    printf("{\r\n");
    printf(" Maximum APDU size in octets: 1476\r\n");
    printf("}\r\n\r\n");

    printf("List of Objects in Test Device:\r\n");
    /* Print Opening brace, then kick off the Device Object */
    printf("{\r\n");
    printf("  {\r\n");  /* And opening brace for the first object */
}


/* Initialize fields for a new Object */
void StartNextObject(
    BACNET_READ_ACCESS_DATA * rpm_object,
    BACNET_OBJECT_ID * pNewObject)
{
    BACNET_PROPERTY_REFERENCE *rpm_property;
    Error_Detected = false;
    Property_List_Index = Property_List_Length = 0;
    rpm_object->object_type = pNewObject->type;
    rpm_object->object_instance = pNewObject->instance;
    rpm_property = calloc(1, sizeof(BACNET_PROPERTY_REFERENCE));
    rpm_object->listOfProperties = rpm_property;
    assert(rpm_property);
    rpm_property->propertyIdentifier = PROP_ALL;
    rpm_property->propertyArrayIndex = BACNET_ARRAY_ALL;
}

/** Main function of the bacepics program.
 *
 * @see Device_Set_Object_Instance_Number, Keylist_Create, address_init,
 *      dlenv_init, address_bind_request, Send_WhoIs,
 *      tsm_timer_milliseconds, datalink_receive, npdu_handler,
 *      Send_Read_Property_Multiple_Request,
 *
 *
 * @param argc [in] Arg count.
 * @param argv [in] Takes one to four arguments, processed in CheckCommandLineArgs().
 * @return 0 on success.
 */
int main(
    int argc,
    char *argv[])
{
    BACNET_ADDRESS src; /* address where message came from */
    uint16_t pdu_len = 0;
    unsigned timeout = 100;     /* milliseconds */
    unsigned max_apdu = 0;
    time_t elapsed_seconds = 0;
    time_t last_seconds = 0;
    time_t current_seconds = 0;
    time_t timeout_seconds = 0;
    bool found = false;
    BACNET_OBJECT_ID myObject;
    uint8_t buffer[MAX_PDU] = { 0 };
    BACNET_READ_ACCESS_DATA *rpm_object = NULL;
    KEY nextKey;

    CheckCommandLineArgs(argc, argv);   /* Won't return if there is an issue. */
    memset(&src, 0, sizeof(BACNET_ADDRESS));

    /* setup my info */
    Device_Set_Object_Instance_Number(BACNET_MAX_INSTANCE);
    Object_List = Keylist_Create();
#if defined(BACDL_BIP)
    /* For BACnet/IP, we might have set a different port for "me", so
     * (eg) we could talk to a BACnet/IP device on our same interface.
     * My_BIP_Port will be non-zero in this case.
     */
    if (My_BIP_Port > 0) {
        bip_set_port(htons(My_BIP_Port));
    }
#endif
    address_init();
    Init_Service_Handlers();
    dlenv_init();
    atexit(datalink_cleanup);

    /* configure the timeout values */
    current_seconds = time(NULL);
    timeout_seconds = (apdu_timeout() / 1000) * apdu_retries();

#if defined(BACDL_BIP)
    if (My_BIP_Port > 0) {
        bip_set_port(htons(0xBAC0));    /* Set back to std BACnet/IP port */
    }
#endif
    /* try to bind with the target device */
    found =
        address_bind_request(Target_Device_Object_Instance, &max_apdu,
        &Target_Address);
    if (!found) {
        if (Provided_Targ_MAC) {
            if (Target_Address.net > 0) {
                /* We specified a DNET; call Who-Is to find the full
                 * routed path to this Device */
                Send_WhoIs_Remote(&Target_Address,
                    Target_Device_Object_Instance,
                    Target_Device_Object_Instance);

            } else {
                /* Update by adding the MAC address */
                if (max_apdu == 0)
                    max_apdu = MAX_APDU;        /* Whatever set for this datalink. */
                address_add_binding(Target_Device_Object_Instance, max_apdu,
                    &Target_Address);
            }
        } else {
            Send_WhoIs(Target_Device_Object_Instance,
                Target_Device_Object_Instance);
        }
    }
    myObject.type = OBJECT_DEVICE;
    myObject.instance = Target_Device_Object_Instance;
    myState = INITIAL_BINDING;
    do {
        /* increment timer - will exit if timed out */
        last_seconds = current_seconds;
        current_seconds = time(NULL);
        /* Has at least one second passed ? */
        if (current_seconds != last_seconds) {
            tsm_timer_milliseconds(((current_seconds - last_seconds) * 1000));
        }

        /* OK to proceed; see what we are up to now */
        switch (myState) {
            case INITIAL_BINDING:
                /* returns 0 bytes on timeout */
                pdu_len =
                    datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

                /* process; normally is some initial error */
                if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                }
                /* will wait until the device is bound, or timeout and quit */
                found =
                    address_bind_request(Target_Device_Object_Instance,
                    &max_apdu, &Target_Address);
                if (!found) {
                    /* increment timer - exit if timed out */
                    elapsed_seconds += (current_seconds - last_seconds);
                    if (elapsed_seconds > timeout_seconds) {
                        fprintf(stderr,
                            "\rError: Unable to bind to %u"
                            " after waiting %ld seconds.\r\n",
                            Target_Device_Object_Instance,
                            (long int) elapsed_seconds);
                        break;
                    }
                    /* else, loop back and try again */
                    continue;
                } else {
                    rpm_object = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
                    assert(rpm_object);
                    myState = GET_HEADING_INFO;
                }
                break;

            case GET_HEADING_INFO:
                last_seconds = current_seconds;
                StartNextObject(rpm_object, &myObject);
                BuildPropRequest(rpm_object, &InitIdPropList[0]);
                Request_Invoke_ID =
                    Send_Read_Property_Multiple_Request(buffer, MAX_PDU,
                    Target_Device_Object_Instance, rpm_object);
                if (Request_Invoke_ID > 0) {
                    elapsed_seconds = 0;
                } else {
                    /* We failed.  Will hurt the header info we can show. */
                    fprintf(stderr, "\r-- Failed to get Heading info \r\n");
                }
                myState = GET_HEADING_RESPONSE;
                break;

            case PRINT_HEADING:
                /* Print out the header information */
                PrintHeading();
                myState = GET_ALL_REQUEST;
                /* Fall through now */

            case GET_ALL_REQUEST:
            case GET_LIST_OF_ALL_REQUEST:
                /* "list" differs in ArrayIndex only */
                /* Update times; aids single-step debugging */
                last_seconds = current_seconds;
                StartNextObject(rpm_object, &myObject);

                Request_Invoke_ID =
                    Send_Read_Property_Multiple_Request(buffer, MAX_PDU,
                    Target_Device_Object_Instance, rpm_object);
                if (Request_Invoke_ID > 0) {
                    elapsed_seconds = 0;
                    if (myState == GET_LIST_OF_ALL_REQUEST)
                        myState = GET_LIST_OF_ALL_RESPONSE;
                    else
                        myState = GET_ALL_RESPONSE;
                }
                break;

            case GET_HEADING_RESPONSE:
            case GET_ALL_RESPONSE:
            case GET_LIST_OF_ALL_RESPONSE:
                /* returns 0 bytes on timeout */
                pdu_len =
                    datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

                /* process */
                if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                }

                if ((Read_Property_Multiple_Data.new_data) &&
                    (Request_Invoke_ID ==
                        Read_Property_Multiple_Data.service_data.invoke_id)) {
                    Read_Property_Multiple_Data.new_data = false;
                    myState =
                        ProcessRPMData(Read_Property_Multiple_Data.rpm_data,
                        myState);
                    if (tsm_invoke_id_free(Request_Invoke_ID)) {
                        Request_Invoke_ID = 0;
                    } else {
                        assert(false);  /* How can this be? */
                        Request_Invoke_ID = 0;
                    }
                    elapsed_seconds = 0;
                } else if (tsm_invoke_id_free(Request_Invoke_ID)) {
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else if (Error_Detected) {
                        if (Last_Error_Code ==
                            ERROR_CODE_REJECT_UNRECOGNIZED_SERVICE) {
                            /* The normal case for Device Object */
                            /* Was it because the Device can't do RPM? */
                            Has_RPM = false;
                            myState = GET_PROPERTY_REQUEST;
                        } else if (Last_Error_Code ==
                            ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED) {
                            myState = GET_PROPERTY_REQUEST;
                            StartNextObject(rpm_object, &myObject);
                        } else if (myState == GET_ALL_RESPONSE)
                            /* Try again, just to get a list of properties. */
                            myState = GET_LIST_OF_ALL_REQUEST;
                        else {
                            /* Else drop back to RP. */
                            myState = GET_PROPERTY_REQUEST;
                            StartNextObject(rpm_object, &myObject);
                        }
                    } else if (Has_RPM)
                        myState = GET_ALL_REQUEST;      /* Let's try again */
                    else
                        myState = GET_PROPERTY_REQUEST;
                } else if (tsm_invoke_id_failed(Request_Invoke_ID)) {
                    fprintf(stderr, "\rError: TSM Timeout!\r\n");
                    tsm_free_invoke_id(Request_Invoke_ID);
                    Request_Invoke_ID = 0;
                    elapsed_seconds = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else
                        myState = GET_ALL_REQUEST;      /* Let's try again */
                } else if (Error_Detected) {
                    /* Don't think we'll ever actually reach this point. */
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    if (myState == GET_HEADING_RESPONSE)
                        myState = PRINT_HEADING;
                    /* just press ahead without the data */
                    else
                        myState = NEXT_OBJECT;  /* Give up and move on to the next. */
                    Error_Count++;
                }
                break;

                /* Process the next single property in our list,
                 * if we couldn't GET_ALL at once above. */
            case GET_PROPERTY_REQUEST:
                Error_Detected = false;
                /* Update times; aids single-step debugging */
                last_seconds = current_seconds;
                elapsed_seconds = 0;
                Request_Invoke_ID =
                    Read_Properties(Target_Device_Object_Instance, &myObject);
                if (Request_Invoke_ID == 0) {
                    /* Reached the end of the list. */
                    myState = NEXT_OBJECT;      /* Move on to the next. */
                } else
                    myState = GET_PROPERTY_RESPONSE;
                break;

            case GET_PROPERTY_RESPONSE:
                /* returns 0 bytes on timeout */
                pdu_len =
                    datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

                /* process */
                if (pdu_len) {
                    npdu_handler(&src, &Rx_Buf[0], pdu_len);
                }

                if ((Read_Property_Multiple_Data.new_data) &&
                    (Request_Invoke_ID ==
                        Read_Property_Multiple_Data.service_data.invoke_id)) {
                    Read_Property_Multiple_Data.new_data = false;
                    PrintReadPropertyData(Read_Property_Multiple_Data.
                        rpm_data->object_type,
                        Read_Property_Multiple_Data.rpm_data->object_instance,
                        Read_Property_Multiple_Data.rpm_data->
                        listOfProperties);
                    if (tsm_invoke_id_free(Request_Invoke_ID)) {
                        Request_Invoke_ID = 0;
                    } else {
                        assert(false);  /* How can this be? */
                        Request_Invoke_ID = 0;
                    }
                    elapsed_seconds = 0;
                    /* Advance the property (or Array List) index */
                    if (Using_Walked_List) {
                        Walked_List_Index++;
                        if (Walked_List_Index > Walked_List_Length) {
                            /* go on to next property */
                            Property_List_Index++;
                            Using_Walked_List = false;
                        }
                    } else {
                        Property_List_Index++;
                    }
                    myState = GET_PROPERTY_REQUEST;     /* Go fetch next Property */
                } else if (tsm_invoke_id_free(Request_Invoke_ID)) {
                    Request_Invoke_ID = 0;
                    elapsed_seconds = 0;
                    myState = GET_PROPERTY_REQUEST;
                    if (Error_Detected) {
                        if ((Last_Error_Class != ERROR_CLASS_PROPERTY) &&
                            (Last_Error_Code != ERROR_CODE_UNKNOWN_PROPERTY)) {
                            if (IsLongArray) {
                                /* Change to using a Walked List and retry this property */
                                Using_Walked_List = true;
                                Walked_List_Index = Walked_List_Length = 0;
                            } else {
                                /* OK, skip this one and try the next property. */
                                fprintf(stdout, "    -- Failed to get ");
                                Print_Property_Identifier(Property_List
                                    [Property_List_Index]);
                                fprintf(stdout, " \r\n");
                                Error_Count++;
                                Property_List_Index++;
                                if (Property_List_Index >=
                                    Property_List_Length) {
                                    /* Give up and move on to the next. */
                                    myState = NEXT_OBJECT;
                                }
                            }
                        } else {
                            fprintf(stdout, "    -- unknown property\r\n");
                            Error_Count++;
                            Property_List_Index++;
                            if (Property_List_Index >= Property_List_Length) {
                                /* Give up and move on to the next. */
                                myState = NEXT_OBJECT;
                            }
                        }
                    }
                } else if (tsm_invoke_id_failed(Request_Invoke_ID)) {
                    fprintf(stderr, "\rError: TSM Timeout!\r\n");
                    tsm_free_invoke_id(Request_Invoke_ID);
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    myState = 3;        /* Let's try again, same Property */
                } else if (Error_Detected) {
                    /* Don't think we'll ever actually reach this point. */
                    elapsed_seconds = 0;
                    Request_Invoke_ID = 0;
                    myState = NEXT_OBJECT;      /* Give up and move on to the next. */
                    Error_Count++;
                }
                break;

            case NEXT_OBJECT:
                if (myObject.type == OBJECT_DEVICE) {
                    printf("  -- Found %d Objects \r\n",
                        Keylist_Count(Object_List));
                    Object_List_Index = -1;     /* start over (will be incr to 0) */
                }
                /* Advance to the next object, as long as it's not the Device object */
                do {
                    Object_List_Index++;
                    if (Object_List_Index < Keylist_Count(Object_List)) {
                        nextKey = Keylist_Key(Object_List, Object_List_Index);
                        myObject.type = KEY_DECODE_TYPE(nextKey);
                        myObject.instance = KEY_DECODE_ID(nextKey);
                        /* Don't re-list the Device Object among its objects */
                        if (myObject.type == OBJECT_DEVICE)
                            continue;
                        /* Closing brace for the previous Object */
                        printf("  }, \r\n");
                        /* Opening brace for the new Object */
                        printf("  { \r\n");
                        /* Test code:
                           if ( myObject.type == OBJECT_STRUCTURED_VIEW )
                           printf( "    -- Structured View %d \n", myObject.instance );
                         */
                    } else {
                        /* Closing brace for the last Object */
                        printf("  } \r\n");
                        /* done with all Objects, signal end of this while loop */
                        myObject.type = MAX_BACNET_OBJECT_TYPE;
                    }
                    if (Has_RPM)
                        myState = GET_ALL_REQUEST;
                    else {
                        myState = GET_PROPERTY_REQUEST;
                        StartNextObject(rpm_object, &myObject);
                    }

                } while (myObject.type == OBJECT_DEVICE);
                /* Else, don't re-do the Device Object; move to the next object. */
                break;

            default:
                assert(false);  /* program error; fix this */
                break;
        }

        /* Check for timeouts */
        if (!found || (Request_Invoke_ID > 0)) {
            /* increment timer - exit if timed out */
            elapsed_seconds += (current_seconds - last_seconds);
            if (elapsed_seconds > timeout_seconds) {
                fprintf(stderr, "\rError: APDU Timeout! (%lds)\r\n",
                    (long int) elapsed_seconds);
                break;
            }
        }

    } while (myObject.type < MAX_BACNET_OBJECT_TYPE);

    if (Error_Count > 0)
        fprintf(stdout, "\r-- Found %d Errors \r\n", Error_Count);

    /* Closing brace for all Objects, if we got any, and closing footer  */
    if (myState != INITIAL_BINDING) {
        printf("} \r\n");
        printf
            ("End of BACnet Protocol Implementation Conformance Statement\r\n");
        printf("\r\n");
    }

    return 0;
}

/*@} */

/* End group BACEPICS */
