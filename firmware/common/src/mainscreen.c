/*
 * Bafang LCD 850C firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include <math.h>
#include <string.h>
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

uint8_t ui8_m_wheel_speed_decimal;

static uint8_t ui8_walk_assist_state = 0;

void lcd_main_screen(void);
void warnings(void);
void walk_assist_state(void);
void power(void);
void time(void);
void wheel_speed(void);
void battery_soc(void);
void trip_time(void);
void wheel_speed(void);
static void showNextScreen();
static bool renderWarning(FieldLayout *layout);

/// set to true if this boot was caused because we had a watchdog failure, used to show user the problem in the fault line
bool wd_failure_detected;

//
// Fields - these might be shared my multiple screens
//p
Field socField = FIELD_DRAWTEXT();
Field timeField = FIELD_DRAWTEXT();
Field assistLevelField = FIELD_READONLY_UINT("assist", &l3_vars.ui8_assist_level, "");
Field noTxTassistLevelField = FIELD_READONLY_UINT("", &l3_vars.ui8_assist_level, "p");
#ifdef SW102
Field wheelSpeedIntegerField = FIELD_READONLY_UINT("speed", &l3_vars.ui16_wheel_speed_x10, "kmh", .div_digits = 1, .hide_fraction = true);
#else
Field wheelSpeedIntegerField = FIELD_READONLY_UINT("speed", &l3_vars.ui16_wheel_speed_x10, "", .div_digits = 1, .hide_fraction = true);
#endif
Field wheelSpeedDecimalField = FIELD_READONLY_UINT("", &ui8_m_wheel_speed_decimal, "");
Field maxPowerField = FIELD_READONLY_UINT(_S("motor power", "motor pwr"), &l3_vars.ui16_battery_power_filtered, "W");
Field humanPowerField = FIELD_READONLY_UINT("human power", &l3_vars.ui16_pedal_power_filtered, "W");
Field warnField = FIELD_CUSTOM(renderWarning);

Field tripTimeField = FIELD_READONLY_STRING("trp time", "unset");
Field tripDistanceField = FIELD_READONLY_UINT("trip distance", &l3_vars.ui32_trip_x10, "km", .div_digits = 1);
Field tripDistanceIntegerField = FIELD_READONLY_UINT("trp distance", &l3_vars.ui32_trip_x10, "km", .div_digits = 1, .hide_fraction = true);
Field odoField = FIELD_READONLY_UINT("odometer", &l3_vars.ui32_odometer_x10, "km", .div_digits = 1, .hide_fraction = false);

Field motorTempField = FIELD_READONLY_UINT("motor temperature", &l3_vars.ui8_motor_temperature, "C");
Field batteryVoltageField = FIELD_READONLY_UINT("battery voltage", &l3_vars.ui16_battery_voltage_filtered_x10, "", .div_digits = 1);

Field pwmDutyField = FIELD_READONLY_UINT("pwm duty-cycle", &l3_vars.ui8_duty_cycle, "");
Field motorErpsField = FIELD_READONLY_UINT("motor speed", &l3_vars.ui16_motor_speed_erps, "");
Field motorFOCField = FIELD_READONLY_UINT("motor foc", &l3_vars.ui8_foc_angle, "");
Field cadenceField = FIELD_READONLY_UINT("cadence", &l3_vars.ui8_pedal_cadence, "rpm");

Field AvgSpeed = FIELD_READONLY_UINT("Avg speed", &l3_vars.ui16_avg_speed_x10, "kmh/h", .div_digits = 1);
Field RangeField = FIELD_READONLY_UINT("Reichw", &l3_vars.ui16_erwartete_reichweite_gesamt_x10, "km", .div_digits = 1, .hide_fraction = false);
Field maxSpeedField = FIELD_READONLY_UINT("MaxSpeed", &l3_vars.ui16_max_speed_x10_kmh, "kmh", .div_digits = 1, .hide_fraction = false);
Field UsedField = FIELD_READONLY_UINT("Used", &l3_vars.ui32_wh_x10, "whr");
Field gesamt_kmField = FIELD_READONLY_UINT("GesKm", &l3_vars.ui32_ee_gesamt_km, "gKm");

Field WhKmField = FIELD_READONLY_UINT("Wh/Km", &l3_vars.ui16_durchschn_verbrauch_Wh_x10_p_km__gesamt, "Wk", .div_digits = 1, .hide_fraction = false);
/**
 * NOTE: The indexes into this array are stored in EEPROM, to prevent user confusion add new options only at the end.
 * If you remove old values, either warn users or bump up eeprom version to force eeprom contents to be discarded.
 */
Field *customizables[] = {
		&maxPowerField, // 0
		&humanPowerField, // 1
		&tripTimeField, // 2
		&odoField, // 3
		&motorTempField, // 4
		&batteryVoltageField, // 5
		&pwmDutyField, // 6
		&motorErpsField, // 7
		&motorFOCField, // 8
		&cadenceField, // 9
		&tripDistanceField, // 10
		&AvgSpeed,
		&maxSpeedField,
		&RangeField,
		&UsedField,
		&WhKmField,
		&gesamt_kmField,
		NULL
};


// Note: field_selectors[0] is used on the 850C for the graphs selector
Field custom1 = FIELD_CUSTOMIZABLE_PTR(&l3_vars.field_selectors[1], customizables);
Field custom2 = FIELD_CUSTOMIZABLE_PTR(&l3_vars.field_selectors[2], customizables);
Field custom3 = FIELD_CUSTOMIZABLE_PTR(&l3_vars.field_selectors[3], customizables);
Field custom4 = FIELD_CUSTOMIZABLE_PTR(&l3_vars.field_selectors[4], customizables);


Field bootHeading = FIELD_DRAWTEXTPTR(_S("OpenSource EBike", "OS-EBike"));
Field bootURL = FIELD_DRAWTEXTPTR(_S("github.com/\nOpenSource-EBike-Firmware", "see github.com"));
Field bootFirmwareVersion = FIELD_DRAWTEXTPTR("850C firmware version:");
Field bootVersion = FIELD_DRAWTEXTPTR(VERSION_STRING);
Field bootStatus = FIELD_DRAWTEXT(.msg = "Booting...");

#define MIN_VOLTAGE_10X 140 // If our measured bat voltage (using ADC in the display) is lower than this, we assume we are running on a developers desk

static void bootScreenOnPreUpdate() {

	uint16_t bvolt = battery_voltage_10x_get();

	is_sim_motor = (bvolt < MIN_VOLTAGE_10X);

  if(is_sim_motor)
    fieldPrintf(&bootStatus, "SIMULATING TSDZ2!");
  else if(has_seen_motor)
    fieldPrintf(&bootStatus, "Found TSDZ2");
  else
    fieldPrintf(&bootStatus, "Waiting TSDZ2 - (%u.%uV)", bvolt / 10, bvolt % 10);

  // Stop showing only after we release on/off button and we are commutication with motor
  if(buttons_get_onoff_state() == 0 && (has_seen_motor || is_sim_motor))
    showNextScreen();
}

Screen bootScreen = {
  .onPreUpdate = bootScreenOnPreUpdate,

  .fields = {
    {
      .x = 0, .y = YbyEighths(1), .height = -1,
      .field = &bootHeading,
      .font = &REGULAR_TEXT_FONT,
    },

    {
      .x = 0, .y = -20, .height = -1,
      .field = &bootURL,
      .font = &SMALL_TEXT_FONT,
    },
#ifndef SW102
    {
      .x = 0, .y = YbyEighths(4), .height = -1,
      .field = &bootFirmwareVersion,
      .font = &SMALL_TEXT_FONT,
    },
#endif
    {
      .x = 0, .y = -8, .height = -1,
      .field = &bootVersion,
      .font = &SMALL_TEXT_FONT,
    },
    {
      .x = 0, .y = YbyEighths(6), .height = -1,
      .field = &bootStatus,
      .font = &SMALL_TEXT_FONT,
    },
    {
      .field = NULL
    }
  }
};

// Allow common operations (like walk assist and headlights) button presses to work on any page
bool anyscreen_onpress(buttons_events_t events) {
  if ((events & DOWN_LONG_CLICK) && l3_vars.ui8_walk_assist_feature_enabled) {
    ui8_walk_assist_state = 1;
    return true;
  }

  // long up to turn on headlights
  if (events & UP_LONG_CLICK) {
    l3_vars.ui8_lights = !l3_vars.ui8_lights;
    set_lcd_backlight();

    return true;
  }
//  return false;

	// Stef long M after first long M to clear range Data
	if (events & M_LONG_CLICK && l3_vars.ui32_trip_x10 == 0) {
		l3_vars.ui32_ee_gesamt_km = 0;
		l3_vars.ui32_ee_gesamt_km_mit_motor = 0;
		l3_vars.ui32_wh_gesamt_x10_offset = 0;
		return true;
	}

	// first long M to clear last trip data
	if (events & M_LONG_CLICK) {
		l3_vars.ui32_trip_x10 = 0;
		l3_vars.ui32_trip_timeSec = 0;
		l3_vars.ui16_avg_speed_x10 = 0;
		l3_vars.ui16_max_speed_x10_kmh = 0;
		return true;
	}




		if (events & ONOFF_CLICK_LONG_CLICK) {		// goto configScreen
			 screenShow(screens[2]);
			 return true;
		 }

		 if (events & M_CLICK_LONG_CLICK && l3_vars.ui16_wheel_speed_x10 > 300) {	// Turbo
					l3_vars.ui8_offroad_mode = 1;
		 		 return true;
		 	 }



	return false;
}

bool mainscreen_onpress(buttons_events_t events) {
	if(anyscreen_onpress(events))
	  return true;

	if (events & UP_CLICK ) {
		l3_vars.ui8_assist_level++;

		if (l3_vars.ui8_assist_level > l3_vars.ui8_number_of_assist_levels) {
			l3_vars.ui8_assist_level = l3_vars.ui8_number_of_assist_levels;
		}

		return true;
	}

	if (events & DOWN_CLICK) {
		if (l3_vars.ui8_assist_level > 0)
			l3_vars.ui8_assist_level--;

		return true;
	}

	return false;
}


void set_conversions() {
  screenConvertMiles = l3_vars.ui8_units_type != 0; // Set initial value on unit conversions (FIXME, move this someplace better)
  screenConvertFarenheit = screenConvertMiles; // FIXME, should be based on a different eeprom config value
}

void lcd_main_screen(void) {
	time();
	walk_assist_state();
//  offroad_mode();
	power();
	battery_soc();
	battery_display();
	warnings();
	trip_time();
	wheel_speed();
}

void wheel_speed(void)
{
  // limit otherwise at startup this value goes crazy
	if(l3_vars.ui16_wheel_speed_x10 > 999) {
    l3_vars.ui16_wheel_speed_x10 = 999;
  }

	if(l3_vars.ui16_wheel_speed_x10 > l3_vars.ui16_max_speed_x10_kmh) {
    l3_vars.ui16_max_speed_x10_kmh = l3_vars.ui16_wheel_speed_x10;
  }

  // Note: no need to check for 'wheel speed previous' because this math is so cheap
  ui8_m_wheel_speed_decimal = (uint8_t) (l3_vars.ui16_wheel_speed_x10 % 10);
}

void power(void) {
#if 0

  if(!m_lcd_vars.ui8_lcd_menu_max_power)
  {
    _ui16_battery_power_filtered = l3_vars.ui16_battery_power_filtered;

    if((_ui16_battery_power_filtered != ui16_battery_power_filtered_previous) ||
        m_lcd_vars.ui32_main_screen_draw_static_info ||
        ui8_target_max_battery_power_state == 0)
    {
      ui16_battery_power_filtered_previous = _ui16_battery_power_filtered;
      ui8_target_max_battery_power_state = 1;

      if (_ui16_battery_power_filtered > 9999) { _ui16_battery_power_filtered = 9999; }

      power_number.ui32_number = _ui16_battery_power_filtered;
      power_number.ui8_refresh_all_digits = m_lcd_vars.ui32_main_screen_draw_static_info;
      lcd_print_number(&power_number);
      power_number.ui8_refresh_all_digits = 0;
    }
    else
    {

    }
  }
  else
  {
    // because this click envent can happens and will block the detection of button_onoff_long_click_event
    buttons_clear_onoff_click_event();

    // leave this menu with a button_onoff_long_click
    if(buttons_get_onoff_long_click_event())
    {
      buttons_clear_all_events();
      m_lcd_vars.ui8_lcd_menu_max_power = 0;
      ui8_target_max_battery_power_state = 0;
      power_number.ui8_refresh_all_digits = 1;

      // save the updated variables on EEPROM
      eeprom_write_variables();

      buttons_clear_all_events();
      return;
    }

    if(buttons_get_up_click_event())
    {
      buttons_clear_all_events();

      if(l3_vars.ui8_target_max_battery_power < 10)
      {
        l3_vars.ui8_target_max_battery_power++;
      }
      else
      {
        l3_vars.ui8_target_max_battery_power += 2;
      }

      // limit to 100 * 25 = 2500 Watts
      if(l3_vars.ui8_target_max_battery_power > 100) { l3_vars.ui8_target_max_battery_power = 100; }
    }

    if(buttons_get_down_click_event ())
    {
      buttons_clear_all_events();

      if(l3_vars.ui8_target_max_battery_power == 0)
      {

      }
      else if(l3_vars.ui8_target_max_battery_power <= 10)
      {
        l3_vars.ui8_target_max_battery_power--;
      }
      else
      {
        l3_vars.ui8_target_max_battery_power -= 2;
      }
    }

    if(ui8_lcd_menu_flash_state)
    {
      if(ui8_target_max_battery_power_state == 1)
      {
        ui8_target_max_battery_power_state = 0;

        // clear area
        power_number.ui8_clean_area_all_digits = 1;
        lcd_print_number(&power_number);
        power_number.ui8_clean_area_all_digits = 0;
      }
    }
    else
    {
      if(ui8_target_max_battery_power_state == 0)
      {
        ui8_target_max_battery_power_state = 1;

        ui16_target_max_power = l3_vars.ui8_target_max_battery_power * 25;

        power_number.ui8_refresh_all_digits = 1;
        power_number.ui32_number = ui16_target_max_power;
        lcd_print_number(&power_number);

        l3_vars.ui8_target_max_battery_power = ui16_target_max_power / 25;
      }
    }
  }
#endif
}

void mainscreen_show(void) {
	screenShow(&mainScreen);
}

void screen_clock(void) {
	static uint8_t ui8_counter_100ms = 0;

	// every 100ms
	if (ui8_counter_100ms++ >= 4) {
		ui8_counter_100ms = 0;

		// receive data from layer 2 to layer 3
		// send data from layer 3 to layer 2
		ui32_g_layer_2_can_execute = 0;
		copy_layer_2_layer_3_vars();
		ui32_g_layer_2_can_execute = 1;
	}

	lcd_main_screen();
	screenUpdate();
}

void trip_time(void) {
	struct_rtc_time_t *p_time = rtc_get_time_since_startup();
	static int oldmin = -1; // used to prevent unneeded updates
	static char timestr[8]; // 12:13

	if(p_time->ui8_minutes != oldmin) {
		oldmin = p_time->ui8_minutes;
		sprintf(timestr, "%d:%02d", p_time->ui8_hours, p_time->ui8_minutes);
		updateReadOnlyStr(&tripTimeField, timestr);
	}
}

static ColorOp warnColor = ColorNormal;
static char warningStr[MAX_FIELD_LEN];

// We use a custom callback so we can reuse the standard drawtext code, but with a dynamically changing color
static bool renderWarning(FieldLayout *layout) {
	layout->color = warnColor;
	return renderDrawTextCommon(layout, warningStr);
}

static void setWarning(ColorOp color, const char *str) {
	warnColor = color;
	warnField.blink = (color == ColorError);
	warnField.dirty = (strcmp(str, warningStr) != 0);
	if(warnField.dirty)
		strncpy(warningStr, str, sizeof(warningStr));
}


#define NO_ERROR                                0
#define ERROR_MOTOR_BLOCKED                     1
#define ERROR_TORQUE_APPLIED_DURING_POWER_ON    2
#define ERROR_BRAKE_APPLIED_DURING_POWER_ON     3
#define ERROR_THROTTLE_APPLIED_DURING_POWER_ON  4
#define ERROR_NO_SPEED_SENSOR_DETECTED          5
#define ERROR_LOW_CONTROLLER_VOLTAGE            6 // controller works with no less than 15 V so give error code if voltage is too low

static const char *motorErrors[] = { "None", "Motor Blocked", "Torque Fault", "Brake Fault", "Throttle Fault", "Speed Fault", "Low Volt" };

void warnings(void) {
	// High priorty faults in red

	if(l3_vars.ui8_error_states) {
		const char *str = (l3_vars.ui8_error_states > ERROR_MAX) ? "Unknown Motor" : motorErrors[l3_vars.ui8_error_states];
		setWarning(ColorError, str);
		return;
	}

	if(l3_vars.ui8_motor_temperature >= l3_vars.ui8_motor_temperature_max_value_to_limit) {
		setWarning(ColorError, "Temp Shutdown");
		return;
	}

	// If we had a watchdog failure, show it forever - so user will report a bug
	if(wd_failure_detected) {
    setWarning(ColorError, "Report Bug!");
    return;
	}

	// warn faults in yellow
	if(l3_vars.ui8_motor_temperature >= l3_vars.ui8_motor_temperature_min_value_to_limit) {
		setWarning(ColorWarning, "Temp Warning");
		return;
	}

	// All of the following possible 'faults' are low priority

	if(l3_vars.ui8_braking) {
		setWarning(ColorNormal, "BRAKE");
		return;
	}

	if(l3_vars.ui8_walk_assist) {
		setWarning(ColorNormal, "WALK");
		return;
	}

	if(l3_vars.ui8_lights) {
		setWarning(ColorNormal, "LIGHT");
		return;
	}

	setWarning(ColorNormal, "");
	if (l3_vars.ui32_trip_timeSec == 0)
			setWarning(ColorNormal, "CLEAR TRP");

	if (l3_vars.ui32_ee_gesamt_km == 0)
			setWarning(ColorNormal, "CLEAR RNG");

}



void battery_soc(void) {
	if (l3_vars.ui8_battery_soc_enable)
		fieldPrintf(&socField, "%3d%%", ui16_m_battery_soc_watts_hour);
	else
		fieldPrintf(&socField, "%u.%1uV",
				l3_vars.ui16_battery_voltage_soc_x10 / 10,
				l3_vars.ui16_battery_voltage_soc_x10 % 10);
}


void time(void) {
	struct_rtc_time_t *p_rtc_time = rtc_get_time();

	// force to be [0 - 12]
	if (l3_vars.ui8_units_type) { // FIXME, should be based on a different eeprom config value, just because someone is using mph doesn't mean they want 12 hr time
		if (p_rtc_time->ui8_hours > 12) {
			p_rtc_time->ui8_hours -= 12;
		}
	}

	fieldPrintf(&timeField, "%d:%02d", p_rtc_time->ui8_hours,
			p_rtc_time->ui8_minutes);
}

void walk_assist_state(void) {
	// kevinh - note on the sw102 we show WALK in the box normally used for BRAKE display - the display code is handled there now
	if (l3_vars.ui8_walk_assist_feature_enabled) {
		// if down button is still pressed
		if (ui8_walk_assist_state && buttons_get_down_state()) {
			l3_vars.ui8_walk_assist = 1;
		} else if (buttons_get_down_state() == 0) {
			ui8_walk_assist_state = 0;
			l3_vars.ui8_walk_assist = 0;
		}
	} else {
		ui8_walk_assist_state = 0;
		l3_vars.ui8_walk_assist = 0;
	}
}

// Screens in a loop, shown when the user short presses the power button
extern Screen *screens[];

static void showNextScreen() {
	static int nextScreen;

	Screen *next = screens[nextScreen++];

//	if (!next) {
	if (nextScreen > 2) {	//Stef 2 bei 2 Screens 3. ist config
		nextScreen = 0;
		next = screens[nextScreen++];
	}

	screenShow(next);
}

static bool appwide_onpress(buttons_events_t events)
{
  // power off only after we release first time the onoff button
  if (events & ONOFF_LONG_CLICK)
  {
    lcd_power_off(1);
    return true;
  }

  if(events & SCREENCLICK_NEXT_SCREEN) {
    showNextScreen();
    return true;
  }
	return false;
}

/// Called every 20ms to check for button events and dispatch to our handlers
static void handle_buttons() {

  static uint8_t firstTime = 1;

  // keep tracking of first time release of onoff button
  if(firstTime && buttons_get_onoff_state() == 0) {
    firstTime = 0;
    buttons_clear_onoff_click_event();
    buttons_clear_onoff_long_click_event();
    buttons_clear_onoff_click_long_click_event();
  }

  if (buttons_events && firstTime == 0)
  {
    bool handled = false;

		if (!handled)
			handled |= screenOnPress(buttons_events);

		// Note: this must be after the screen/menu handlers have had their shot
		if (!handled)
			handled |= appwide_onpress(buttons_events);

		if (handled)
			buttons_clear_all_events();
	}

	buttons_clock(); // Note: this is done _after_ button events is checked to provide a 20ms debounce
}

/// Call every 20ms from the main thread.
void main_idle() {
	handle_buttons();
	screen_clock(); // This is _after_ handle_buttons so if a button was pressed this tick, we immediately update the GUI
	automatic_power_off_management(); // Note: this was moved from layer_2() because it does eeprom operations which should not be used from ISR
}
