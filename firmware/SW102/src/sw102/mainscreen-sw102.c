/*
 * Bafang LCD 850C firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include <math.h>
#include "stdio.h"
#include "main.h"
#include "utils.h"
#include "screen.h"
#include "rtc.h"
#include "fonts.h"
#include "uart.h"
#include "mainscreen.h"
#include "eeprom.h"
#include "buttons.h"
#include "lcd.h"
#include "adc.h"
#include "ugui.h"
#include "configscreen.h"

Field batteryField = FIELD_DRAWTEXT();

static void mainScreenOnEnter() {
	// Set the font preference for this screen
	editable_label_font = &SMALL_TEXT_FONT;
	editable_value_font = &SMALL_TEXT_FONT;
	editable_units_font = &SMALL_TEXT_FONT;
}

/**
 * Appears at the bottom of all screens, includes status msgs or critical fault alerts
 * FIXME - get rid of this nasty define - instead add the concept of Subscreens, so that the battery bar
 * at the top and the status bar at the bottom can be shared across all screens
 */
#define BATTERY_BAR \
    { \
        .x = 0, .y = 0, \
        .width = -1, .height = -1, \
        .field = &batteryField, \
        .font = &MY_FONT_BATTERY, \
    }, \
    { \
        .x = 32, .y = 0, \
        .width = -5, .height = -1, \
				.field = &socField, \
        .font = &REGULAR_TEXT_FONT \
    }



		// .border = BorderBottom | BorderTop  | BorderLeft  | BorderRight,
		// y = -1 für die automatische Zuweisung nächste Zeile
		// height = 0 -> ab y rest vom Screen
		// height = -1 -> Höhe Fonts
		// width = 0 -> ab x rest vom Screen
		// width < 0 -> Nr Fonts
 	 #define WH_BAR \
{ \
 	         .x = 1, .y = 85, \
 	         .width = -9, .height = -1, \
 	         .field = &RangeField, \
					 .font = &REGULAR_TEXT_FONT, \
					 .label_align_x = AlignHidden, \
 	     }

/*  { \
      .x = 1, .y = 75, \
      .width = -8, .height = -1, \
      .field = &WhKmField, \
      .font = &REGULAR_TEXT_FONT, \
      .label_align_x = AlignHidden, \
  }, \
*/

		#define STATUS_BAR \
		{ \
		    .x = 1, .y = 102, \
		    .width = 0, .height = -1, \
		    .field = &warnField, \
		    .font = &REGULAR_TEXT_FONT, \
		}

		#define FOOT_BAR \
		{ \
				.x = 1, .y = 115, \
				.width = -4, .height = -1, \
				.field = &tripTimeField, \
				.font = &REGULAR_TEXT_FONT, \
				.label_align_x = AlignHidden, \
		},\
		{ \
	      .x = 31, .y = 115, \
	      .width = -7, .height = -1, \
				.field = &tripDistanceIntegerField, \
	      .font = &REGULAR_TEXT_FONT, \
				.label_align_x = AlignHidden, \
	  }
		/*
	  { \
	      .x = 1, .y = 114, \
	      .width = -7, .height = -1, \
	      .field = &tripDistanceField, \
	      .font = &REGULAR_TEXT_FONT, \
				.label_align_x = AlignHidden, \
	  }
		*/


//
// Screens
//
Screen mainScreen = {
  .onPress = mainscreen_onpress,
	.onEnter = mainScreenOnEnter,

    .fields = {
    BATTERY_BAR,
		{
        .x = 0, .y = 48,//35
        .width = -1, .height = -1,
        .field = &assistLevelField,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_x = AlignHidden,
        .border = BorderBottom//BorderNone
    },
    {
        .x = 12, .y = 24,
        .width = -2, .height = -1,
        .field = &wheelSpeedIntegerField,
        .font = &BIG_NUMBERS_TEXT_FONT,
        .label_align_x = AlignHidden,
        .border = BorderBottom
    },
/*    {
        .x = 0, .y = -2,
        .width = 0, .height = 19,
        .field = &maxPowerField,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_x = AlignHidden,
        .border = BorderBottom
    },
		*/
		WH_BAR,
    STATUS_BAR,
		FOOT_BAR,
    {
        .field = NULL
    } }
};

Screen infoScreen = {
    // .onPress = mainscreen_onpress,
	.onEnter = mainScreenOnEnter,
  .onCustomized = eeprom_write_variables,
	 .onPress = mainscreen_onpress,
  //Stef   .onPress = anyscreen_onpress,

    .fields = {
    BATTERY_BAR,
    {
        .x = 0, .y = -3,
        .width = 0, .height = -1,
        .field = &custom1,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_y = AlignTop,
        .border = BorderBottom | BorderTop
    },
    {
        .x = 0, .y = -3,
        .width = 0, .height = -1,
        .field = &custom2,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_y = AlignTop,
        .border = BorderBottom
    },
    {
        .x = 0, .y = -3,
        .width = 0, .height = -1,
        .field = &custom3,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_y = AlignTop,
        .border = BorderBottom
    },
#if 0
    {
        .x = 0, .y = -3,
        .width = 0, .height = -1,
        .field = &custom4,
        .font = &MEDIUM_NUMBERS_TEXT_FONT,
        .label_align_x = AlignHidden,
        .border = BorderBottom
    },
#endif
    FOOT_BAR,
    STATUS_BAR,
    {
        .field = NULL
    } }
};

// Show our battery graphic
void battery_display() {
  // on this board we use a special battery font
  uint8_t ui32_battery_bar_number = l3_vars.volt_based_soc / (90 / 5); // scale SOC so anything greater than 90% is 5 bars, and zero is zero.
  fieldPrintf(&batteryField, "%d", ui32_battery_bar_number);
}

// Screens in a loop, shown when the user short presses the power button
Screen *screens[] = { &mainScreen,
	&infoScreen, &configScreen,
//	&infoScreen,
		NULL };
