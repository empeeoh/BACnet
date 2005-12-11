/*####COPYRIGHTBEGIN####
 -------------------------------------------
 Copyright (C) 2005 Steve Karg

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to:
 The Free Software Foundation, Inc.
 59 Temple Place - Suite 330
 Boston, MA  02111-1307, USA.

 As a special exception, if other files instantiate templates or
 use macros or inline functions from this file, or you compile
 this file and link it with other works to produce a work based
 on this file, this file does not by itself cause the resulting
 work to be covered by the GNU General Public License. However
 the source code for this file must still be made available in
 accordance with section (3) of the GNU General Public License.

 This exception does not invalidate any other reasons why a work
 based on this file might be covered by the GNU General Public
 License.
 -------------------------------------------
####COPYRIGHTEND####*/
#include <stdint.h>
#include "bacenum.h"
#include "bacdcode.h"
#include "bacdef.h"
#include "bacapp.h"

int bacapp_encode_application_data(
  uint8_t *apdu,
  BACNET_APPLICATION_DATA_VALUE *value)
{
  int apdu_len = 0; // total length of the apdu, return value

  if (apdu)
  {
    if (value->tag == BACNET_APPLICATION_TAG_NULL)
      apdu[apdu_len++] = value->tag;
    else if (value->tag == BACNET_APPLICATION_TAG_BOOLEAN)
      apdu_len += encode_tagged_boolean(&apdu[apdu_len],
        value->type.Boolean);
    else if (value->tag == BACNET_APPLICATION_TAG_UNSIGNED_INT)
      apdu_len += encode_tagged_unsigned(&apdu[apdu_len],
        value->type.Unsigned_Int);
    else if (value->tag == BACNET_APPLICATION_TAG_SIGNED_INT)
      apdu_len += encode_tagged_signed(&apdu[apdu_len],
        value->type.Signed_Int);
    else if (value->tag == BACNET_APPLICATION_TAG_REAL)
      apdu_len += encode_tagged_real(&apdu[apdu_len],
        value->type.Real);
/* FIXME: encode the strings using the string datatype
    else if (value->tag == BACNET_APPLICATION_TAG_CHARACTER_STRING)
      apdu_len += encode_tagged_character_string(
        &apdu[apdu_len],
        &value->type.Character_String[0]);
*/
    else if (value->tag == BACNET_APPLICATION_TAG_ENUMERATED)
      apdu_len += encode_tagged_enumerated(&apdu[apdu_len],
        value->type.Enumerated);
    else if (value->tag == BACNET_APPLICATION_TAG_DATE)
      apdu_len += encode_tagged_date(&apdu[apdu_len],
        value->type.Date.year,
        value->type.Date.month,
        value->type.Date.day,
        value->type.Date.wday);
    else if (value->tag == BACNET_APPLICATION_TAG_TIME)
      apdu_len += encode_tagged_time(&apdu[apdu_len],
        value->type.Time.hour,
        value->type.Time.min,
        value->type.Time.sec,
        value->type.Time.hundredths);
    else if (value->tag == BACNET_APPLICATION_TAG_OBJECT_ID)
      apdu_len += encode_tagged_object_id(&apdu[apdu_len],
        value->type.Object_Id.type,value->type.Object_Id.instance);
  }

  return apdu_len;
}

int bacapp_decode_application_data(
  uint8_t *apdu,
  uint8_t apdu_len,
  BACNET_APPLICATION_DATA_VALUE *value)
{
  int len = 0;
  int tag_len = 0;
  uint8_t tag_number = 0;
  uint32_t len_value_type = 0;
  int hour, min, sec, hundredths;
  int year, month, day, wday;
  int object_type = 0;
  uint32_t instance = 0;

  if (apdu)
  {
    tag_len = decode_tag_number_and_value(&apdu[0],
      &tag_number, &len_value_type);
    if (tag_len)
    {
      len += tag_len;
      if (tag_number == BACNET_APPLICATION_TAG_NULL)
      {
        value->tag = tag_number;
      }
      else if (tag_number == BACNET_APPLICATION_TAG_BOOLEAN)
      {
        value->tag = tag_number;
        value->type.Boolean = decode_boolean(len_value_type);
      }
      else if (tag_number == BACNET_APPLICATION_TAG_UNSIGNED_INT)
      {
        value->tag = tag_number;
        len += decode_unsigned(&apdu[len],
          len_value_type,
          &value->type.Unsigned_Int);
      }
      else if (tag_number == BACNET_APPLICATION_TAG_SIGNED_INT)
      { 
        value->tag = tag_number;
        len += decode_signed(&apdu[len],
          len_value_type,
          &value->type.Signed_Int);
      }
      else if (tag_number == BACNET_APPLICATION_TAG_REAL)
      {
        value->tag = tag_number;
        len += decode_real(&apdu[len],&(value->type.Real));
      }
/* FIXME: decode the strings using the string datatype
      else if (tag_number == BACNET_APPLICATION_TAG_CHARACTER_STRING)
      {
        value->tag = tag_number;
        len += decode_character_string(&apdu[len],
          &value->type.Character_String[0],
          sizeof(value->type.Character_String));
      }
*/
      else if (tag_number == BACNET_APPLICATION_TAG_ENUMERATED)
      {
        value->tag = tag_number;
        len += decode_enumerated(&apdu[len],
          len_value_type,
          &value->type.Enumerated);
      }
      else if (tag_number == BACNET_APPLICATION_TAG_DATE)
      {
        value->tag = tag_number;
        len += decode_date(&apdu[len], &year, &month, &day, &wday);
        value->type.Date.year = year;
        value->type.Date.month = month;
        value->type.Date.day = day;
        value->type.Date.wday = wday;
      }
      else if (tag_number == BACNET_APPLICATION_TAG_TIME)
      {
        value->tag = tag_number;
        len += decode_bacnet_time(&apdu[len], &hour, &min, &sec, &hundredths);
        value->type.Time.hour = hour;
        value->type.Time.min = min;
        value->type.Time.sec = sec;
        value->type.Time.hundredths = hundredths;
      }
      else if (tag_number == BACNET_APPLICATION_TAG_OBJECT_ID)
      {
        value->tag = tag_number;
        len += decode_object_id(&apdu[len],
          &object_type, 
          &instance);
        value->type.Object_Id.type = object_type;
        value->type.Object_Id.instance = instance;
      }
    }
  }

  return len;
}

#ifdef TEST
#include <assert.h>
#include <string.h>
#include "ctest.h"

/* generic - can be used by other unit tests */
void testCompareApplicationData(Test * pTest,
  BACNET_APPLICATION_DATA_VALUE *value,
  BACNET_APPLICATION_DATA_VALUE *test_value)
{
  /* does the tag match? */
  ct_test(pTest, test_value->tag == value->tag);
  /* does the value match? */
  switch (test_value->tag)
  {
    case BACNET_APPLICATION_TAG_NULL:
      break;
    case BACNET_APPLICATION_TAG_BOOLEAN:
      ct_test(pTest,
        test_value->type.Boolean == value->type.Boolean);
      break;
    case BACNET_APPLICATION_TAG_UNSIGNED_INT:
      ct_test(pTest,
        test_value->type.Unsigned_Int == value->type.Unsigned_Int);
      break;
    case BACNET_APPLICATION_TAG_SIGNED_INT:
      ct_test(pTest,
        test_value->type.Signed_Int == value->type.Signed_Int);
      break;
    case BACNET_APPLICATION_TAG_REAL:
      ct_test(pTest,
        test_value->type.Real == value->type.Real);
      break;
    case BACNET_APPLICATION_TAG_ENUMERATED:
      ct_test(pTest,
        test_value->type.Enumerated == value->type.Enumerated);
      break;
    case BACNET_APPLICATION_TAG_DATE:
      ct_test(pTest,
        test_value->type.Date.year == value->type.Date.year);
      ct_test(pTest,
        test_value->type.Date.month == value->type.Date.month);
      ct_test(pTest,
        test_value->type.Date.day == value->type.Date.day);
      ct_test(pTest,
        test_value->type.Date.wday == value->type.Date.wday);
      break;
    case BACNET_APPLICATION_TAG_TIME:
      ct_test(pTest,
        test_value->type.Time.hour == value->type.Time.hour);
      ct_test(pTest,
        test_value->type.Time.min == value->type.Time.min);
      ct_test(pTest,
        test_value->type.Time.sec == value->type.Time.sec);
      ct_test(pTest,
        test_value->type.Time.hundredths == value->type.Time.hundredths);
      break;
    case BACNET_APPLICATION_TAG_OBJECT_ID:
      ct_test(pTest,
        test_value->type.Object_Id.type == value->type.Object_Id.type);
      ct_test(pTest, test_value->type.Object_Id.instance ==
        value->type.Object_Id.instance);
      break;
    default:
      break;
  }
}

void testBACnetApplicationDataValue(Test * pTest,
  BACNET_APPLICATION_DATA_VALUE *value)
{
  uint8_t apdu[480] = {0};
  int len = 0;
  int apdu_len = 0;
  BACNET_APPLICATION_DATA_VALUE test_value;

  apdu_len = bacapp_encode_application_data(&apdu[0],value);
  len = bacapp_decode_application_data(
    &apdu[0],
    apdu_len,
    &test_value);
  testCompareApplicationData(pTest, value, &test_value);

  return;
}

void testBACnetApplicationData(Test * pTest)
{
  BACNET_APPLICATION_DATA_VALUE value = {0};

  value.tag = BACNET_APPLICATION_TAG_NULL;
  testBACnetApplicationDataValue(pTest, &value);

  value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
  value.type.Boolean = true;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Boolean = false;
  testBACnetApplicationDataValue(pTest, &value);

  value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
  value.type.Unsigned_Int = 0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Unsigned_Int = 0xFFFF;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Unsigned_Int = 0xFFFFFFFF;
  testBACnetApplicationDataValue(pTest, &value);

  value.tag = BACNET_APPLICATION_TAG_SIGNED_INT;
  value.type.Signed_Int = 0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Signed_Int = -1;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Signed_Int = 32768;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Signed_Int = -32768;
  testBACnetApplicationDataValue(pTest, &value);
  
  value.tag = BACNET_APPLICATION_TAG_REAL;
  value.type.Real = 0.0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Real = -1.0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Real = 1.0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Real = 3.14159;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Real = -3.14159;
  testBACnetApplicationDataValue(pTest, &value);
    
  value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
  value.type.Enumerated = 0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Enumerated = 0xFFFF;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Enumerated = 0xFFFFFFFF;
  testBACnetApplicationDataValue(pTest, &value);
  
  value.tag = BACNET_APPLICATION_TAG_DATE;
  value.type.Date.year = 5;
  value.type.Date.month = 5;
  value.type.Date.day = 22;
  value.type.Date.wday = 1;
  testBACnetApplicationDataValue(pTest, &value);

  value.tag = BACNET_APPLICATION_TAG_TIME;
  value.type.Time.hour = 23;
  value.type.Time.min = 59;
  value.type.Time.sec = 59;
  value.type.Time.hundredths = 12;
  testBACnetApplicationDataValue(pTest, &value);

  value.tag = BACNET_APPLICATION_TAG_OBJECT_ID;
  value.type.Object_Id.type = OBJECT_ANALOG_INPUT;
  value.type.Object_Id.instance = 0;
  testBACnetApplicationDataValue(pTest, &value);
  value.type.Object_Id.type = OBJECT_LIFE_SAFETY_ZONE;
  value.type.Object_Id.instance = BACNET_MAX_INSTANCE;
  testBACnetApplicationDataValue(pTest, &value);

  return;
}

#ifdef TEST_BACNET_APPLICATION_DATA
int main(void)
{
    Test *pTest;
    bool rc;

    pTest = ct_create("BACnet Application Data", NULL);
    /* individual tests */
    rc = ct_addTestFunction(pTest, testBACnetApplicationData);
    assert(rc);

    ct_setStream(pTest, stdout);
    ct_run(pTest);
    (void) ct_report(pTest);
    ct_destroy(pTest);

    return 0;
}
#endif /* TEST_BACNET_APPLICATION_DATA */
#endif /* TEST */
