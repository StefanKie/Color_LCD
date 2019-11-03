#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ugui.h"
#include "buttons.h"

/**
 * Main screen notes
 *
 * General approach:
 *
 * Make it look approximately like https://github.com/OpenSource-EBike-firmware/SW102_LCD_Bluetooth/blob/master/design/Bafang_SW102_Bluetooth_LCD-design-proposal_1.jpg
 *
 * Split UX logic from display layout with a small screen layer - to allow refactoring and sharing much of the 850C behavior/code
 * with the SW102.  Screen redraw time should go down dramatically because only change fields will need to be drawn.
 * The 850C (I think?) is calling that redraw function a lot - a full screen transfer to the display for each field update.
 *
 * initially just show power remaining in pack, SOC, time, amount of assist
 * make assist go up/down with button press. (initially copy 850C button code, but _real soon_ to allow sharing the C source for button/packet/UX handling between both
 * modern displays)
 *
 * remove call to UG_SetRefresh, it is very expensive.  Much better to just call lcd_refresh() from screenUpdate()
 *
 * define a screen as:
 *
 * an array of FieldLayouts.  each FieldLayout is a (typically constant) struct:
 * xpos, ypos, width, height, colorOpt, fieldPtr
 * colorOpt is an enum of ColorNormal, ColorInvert
 * eventually: ColorCritical, ColorBlink (for use in menu selections) -for color
 * LCDs these enums could map to the theme picked for the UX, for black and white LCDs they map to black or white
 *
 * end of screen is market by a FieldLayout with a null field member
 *
 * fieldPtr points to a particular field with ready to render data (and fields might be shared by multiple screens).
 * Each Field contains
 *   variantCode, isDirty and extra data appropriate for that variant
 *
 * isDirty is true if the field needs to be redrawn in the gui (because data has changed)
 * datavariant is a union that depends on the opcode:
 *
 * drawText: font ptr, char msg[MAXSTRLEN]
 * fillBox: nothing - just fills box based on fore/back color
 * drawBat: soc - draw a bat icon with SOC
 * drawPlot: maxval - max value seen, points - an array of 64 previous data values, to be drawn as a plot, one per column

 *
 * helper functions:
 * fieldPrintf(fieldptr, "str %d", 5) - sets the string for the specified fields, marks the field as dirty if the string changed
 * fieldSetSOC(fieldptr, 32) - sets state of charge and marks field as dirty if the soc changed
 * fieldAddPlot(fieldptr, value) - add a new data point to a plot field
 *
 * When new state is received from the controller, fieldX...() will be called to mark the various fields as dirty.  These functions
 * are cheap and can be called when each rx packet is parsed.  If any field changed in a user visible way the field will be internally
 * marked as dirty and later updateScreen() will show that new value
 *
 * screenShow(screenptr) - set the current screen
 * screenUpdate() - redraw the minimum set of dirty fields (or the whole screen if the screen has changed).
 *   if any fields are blinking the blink animation will be serviced here as well.
 *
 * NOTE: this approach could be extended to include nice support for showing vertically scrolling menus.  Initial version
 * won't do this but should make the config screen fairly easy to code up.
 *
 * add a datavariant for
 * drawMenu: curSelection, const char **menuOptions, const char **menuValues, onChange(tbd) - properly handle showing a scrolling menu
 *   that might be logically longer than our screen. onChange callback will be called when the user changes menu entries
 *
 * bool screenHandlePress(buttonEnum)
 * if a screen is showing a menu field, it might need to intercept and handle button presses. Call this function from
 * the main loop when a press occurs.  This function will return true if it has handled this press event (and therefore
 * you should not do anything else with it).
 *
 * Eventually improve these docs based on the following chat session:
 *
 * any time someone calls screenUpdate() it walks through all the fields on the screen (defined in the mainScreen array from mainscreen.c) and redraw any that have changed values. Fields define any value which can be shown on any screen. Here's the list: https://github.com/OpenSource-EBike-firmware/Color_LCD/blob/sw102-merge/firmware/common/src/mainscreen.c#L37-L64
 whereas FieldLayouts define the placement/size of fields on a particular screen. Here's the mainScreen for the 850C: https://github.com/OpenSource-EBike-firmware/Color_LCD/blob/sw102-merge/firmware/common/src/mainscreen.c#L196-L296
 x,y,width & height are pixel positions or if zero/negative they can define various automatic values: https://github.com/OpenSource-EBike-firmware/Color_LCD/blob/sw102-merge/firmware/common/include/screen.h#L256-L264

 Kevin Hester @geeksville 15:46
 There is a helper macro XbyEighths/YbyEighths to generate screen coordinates in terms of 1/8 of screen position XbyEights(4) is the middle of the screen left/right (regardless of screen size) XbyEighths(1) is 1/8 of the screen in from the left. This allows stuff like the boot/fault screens to be the same on the 850C/SW102 even though their screens are sized quite differently.
 in the case of this project the call to screenUpdate happens in the 20ms gui tick here: https://github.com/OpenSource-EBike-firmware/Color_LCD/blob/sw102-merge/firmware/common/src/mainscreen.c#L748
 it is worth noting that mainScreen isn't different from the config menu or the boot screen or the secondary info screen on SW102. In fact someday we could define a few different screens the user can page through. When a user changes screens by pressing the power button showNextScreen() just changes the ptr to the current screen.

 Kevin Hester @geeksville 15:54
 the various types of fields each have different render functions. The currently defined types of fields are this (which I think has all the options this project would ever need): https://github.com/OpenSource-EBike-firmware/Color_LCD/blob/sw102-merge/firmware/common/include/screen.h#L88-L98
 btw - I'll copy and paste this into the kinda crummy docs in screen.h so that eventually it can be clearer ;-)

 casainho @casainho 15:56
 Ok, I think I understand now, thanks.

 Kevin Hester @geeksville 15:57
 Screens can optionally define onEnter, onExit, onUpdate callbacks. onExit is used for stuff like the config screen saving eeprom settings when the user switches away from the screen. onEnter is used to set custom fonts for just that one screen. onUpdate is used for some special update code which I copied from your original lcd.c.
 *
 */

#define MAX_FIELD_LEN 32

typedef enum {
	ColorNormal = 0, // white on black
	ColorInvert,     // black on white
	ColorHeading,    // white on dk blue
	ColorError,      // red on black (and possibly blinking)
	ColorWarning     // yellow on black
} ColorOp;

/**
 * specifies what type of data is in this field
 *
 * Note: might change someday to instead just be a pointer to a constant vtable like thing
 */
typedef enum {
	FieldDrawText = 0, FieldDrawTextPtr, FieldFill, // Fill with a solid color
	FieldMesh, // Fill with a mesh color
	FieldScrollable, // Contains a menu name and points to a submenu to optionally expand its place.  If at the root of a screen, submenu will be automatically expanded to fill remaining screen space
	FieldEditable, // An editable property with a human visible label and metadata for min/max/type of data and ptr to raw variable to render
	FieldCustom, // A field with a custom render function (provided by the user)
	FieldGraph, // A bar graph
	FieldCustomizable, // A field where the user can choose what is shown, effectively a pointer to another field
	FieldEnd // Marker record for the last entry in a scrollable submenu - never shown to user
} FieldVariant;

/**
 * What sorts of types are supported for FieldEditable.  Currently just uint8, uint16, bool and choosing a string from a list and storing its
 * index
 */
typedef enum {
	EditUInt = 0, // This is the default type if not specified
	EditEnum, // Choose a string from a list
	ReadOnlyStr // Show a simple string
} EditableType;


#define GRAPH_MAX_POINTS	(256) // Note: we waste one record, to make our ring buffer code easier
#define GRAPH_INTERVAL_MS 	3500 // graph updates are expensive - do rarely
#define GRAPH_COLOR_ACCENT  C_WHITE // Drawn as a top line on the graph
#define GRAPH_COLOR_NORMAL  C_BLUE
#define GRAPH_COLOR_WARN    C_YELLOW
#define GRAPH_COLOR_ERROR   C_RED
#define GRAPH_COLOR_BACKGROUND C_BLACK
#define GRAPH_COLOR_AXIS	C_SLATE_GRAY
#define GRAPH_LABEL_FONT	SMALL_TEXT_FONT
#define GRAPH_MAXVAL_FONT 	SMALL_TEXT_FONT

// Assumed period of screenUpdate invoke
#define UPDATE_INTERVAL_MS 20

// How often to toggle blink animations
#define BLINK_INTERVAL_MS  300

// Each _active_ graph needs a graphcache to store past points and invariants.  Currently we use use one,
// but as soon as we have multiple active graphs we should assign dynamically.
typedef struct {
	int32_t points[GRAPH_MAX_POINTS];
	int32_t max_val, min_val; // the max/min value we've seen (ever)
	uint32_t start_valid; // the oldest point in our ring buffer
	uint32_t end_valid; // the newest point in our ring buffer
} GraphCache;

struct FieldLayout;
// Forward declaration

/**
 * Ready to render data (normally populated by comms code) which might be used on multiple different screens
 */
typedef struct Field {
	FieldVariant variant :4;
	bool dirty :1; // true if this data has changed and needs to be rerendered
	bool blink :1; // if true, we should invoke the render function for this field every 500ms (or whatever the blink interval is) to possibly toggle animations on/off
	bool is_selected :1; // if true this field is currently selected by the user (either in a scrollable or actively editing it)
	// bool is_rendered : 1; // true if we're showing this field on the current screen (if false, some fieldPrintf work can be avoided

	union {
		//FIXME: possibly move these fields out into separate structures, because currently the
		//biggest member causes all members to become larger.

		struct {
			char msg[MAX_FIELD_LEN];
		} drawText;

		struct {
			const char *msg; // A string stored in a ptr
		} drawTextPtr;

		struct {
			struct Field *source; // the data field we are graphing
			GraphCache *cache;
			int32_t warn_threshold, error_threshold; // if != -1 and a value exceeds this it will be drawn in the warn/error colors
			int32_t min_threshold; // if value is less than this, it is ignored for purposes of calculating min/average - useful for ignoring speed/cadence when stopped
		} graph;

		struct {
			bool (*render)(struct FieldLayout*); // a custom render function, returns true if we did a render
		} custom;

		struct {
			struct Field *entries; // the menu entries for this submenu.
			const char *label; // the title shown in the GUI for this menu
			uint8_t first; // The first entry we are showing on the screen (ie for scrolling through a series of entries)
			uint8_t selected; // the currently highlighted entry
		} scrollable;

		struct {
			struct Field **choices; // An array of ptrs (editable) fields (terminated with NULL) that the user can choose from
			uint8_t 	*selector; // the index into the array of the users current choice (library clients should store this to eeprom)
		} customizable;

		struct {
			const char *label; // the label shown in the GUI for this item
			void *target; // the data we are showing/manipulating
			const EditableType typ : 2;
			const uint8_t size :3; // sizeof for the specified target - we support 1 or 2 or 4
			bool read_only :1; // if true user can't really edit this

			// the following parameters are particular to the editable type
			union {
				struct {
					const char *units;
					const uint8_t div_digits :4; // how many digits to divide by for fractions (i.e. 0 for integers, 1 for /10x, 2 for /100x, 3 /1000x
					const bool hide_fraction :1; // if set, don't ever show the fractional part
					const uint32_t max_value, min_value; // min/max
					const uint32_t inc_step; // if zero, then 1 is assumed
				} number;

				struct {
					// we assume *target is a uint8_t
					const char **options; // An array of strings, with a NULL entry at the end to mark end of choices
				} editEnum;
			};
		} editable;
	};
} Field;

//
// Helper macros to declare fields more easily
//

#define FIELD_SCROLLABLE(lbl, arry) { .variant = FieldScrollable, .scrollable = { .label = lbl, .entries = arry } }

#define FIELD_EDITABLE_UINT(lbl, targ, unt, minv, maxv, ...) { .variant = FieldEditable, \
  .editable = { .typ = EditUInt, .label = lbl, .target = targ, .size = sizeof(*targ),  \
      .number = { .units = unt, .max_value = maxv, .min_value = minv, ##__VA_ARGS__ } } }

#define FIELD_READONLY_UINT(lbl, targ, unt, ...) { .variant = FieldEditable, \
  .editable = { .read_only = true, .typ = EditUInt, .label = lbl, .target = targ, .size = sizeof(*targ),  \
      .number = { .units = unt, ##__VA_ARGS__ } } }

#define FIELD_READONLY_STRING(lbl, targ) { .variant = FieldEditable, \
  .editable = { .read_only = true, .typ = ReadOnlyStr, .label = lbl, .target = targ, .size = sizeof(*targ)  } }


// C99 allows anonymous constant arrays - take advantage of that here to make declaring the various options easy
#define FIELD_EDITABLE_ENUM(lbl, targ, ...) { .variant = FieldEditable, \
  .editable = { .typ = EditEnum, .label = lbl, .target = targ, .size = sizeof(EditableType), \
      .editEnum = { .options = (const char *[]){ __VA_ARGS__, NULL } } } }

#define FIELD_DRAWTEXT(...) { .variant = FieldDrawText, .drawText = { __VA_ARGS__  } }
#define FIELD_DRAWTEXTPTR(str, ...) { .variant = FieldDrawTextPtr, .drawTextPtr = { .msg = str, ##__VA_ARGS__  } }
#define FIELD_CUSTOM(cb) { .variant = FieldCustom, .custom = { .render = &cb  } }
#define FIELD_GRAPH(s, ...) { .variant = FieldGraph, .blink = true, .graph = { .source = s, ##__VA_ARGS__  } }
#define FIELD_CUSTOMIZABLE_PTR(s, c) { .variant = FieldCustomizable, .customizable = { .selector = s, .choices = c  } }
#define FIELD_CUSTOMIZABLE(s, ...) { .variant = FieldCustomizable, .customizable = { .selector = s, .choices = (Field *[]){ __VA_ARGS__, NULL }}}

#define FIELD_END { .variant = FieldEnd }

typedef int16_t Coord; // Change to int16_t for screens wider/longer than 128, screens shorter than 128 can use uint8_t

typedef enum {
	BorderNone = 0,
	BorderBottom = (1 << 0),
	BorderTop = (1 << 1),
	BorderLeft = (1 << 2),
	BorderRight = (1 << 3),
	BorderFat = (1 << 4), // two pixels tall
	BorderBox = BorderLeft | BorderRight | BorderTop | BorderBottom, // left,right,bottom,top
// define others as needed
} BorderOp;

/// layouts can tell the field they are showing special rendering options
typedef enum {
	ModNone = 0,
	// ModNoLabel = 1, // For editable fields: don't show label (normally), instead show just the data and the units - No longer used, instead just label_align_x == AlignHidden
	// ModLabelTop = 2 // For editable fields: show the label above the value, normally it is shown to the left -  No longer used instead just set label_align_y == AlignTop
} LayoutModifier;

typedef enum {
	AlignDefault = 0,
	AlignCenter = AlignDefault,
	AlignLeft,
	AlignRight,
	AlignHidden // Do not show this element
} AlignmentX;

typedef enum {
	AlignCenterY = AlignCenter, // default
	AlignTop,
	AlignBottom
} AlignmentY;

/**
 * Defines the layout of a field on a particular screen
 */
typedef struct FieldLayout {
	Coord x, y; // a y <0 means, start just below the previous lowest point on the screen, -1 is immediately below, -2 has one blank line, -3 etc...

	// for text fields if negative width is in # of characters. or 0 to determine length based on remaining screen width
	// für Textfelder, wenn die negative Breite in Anzahl Zeichen angegeben ist. oder 0, um die Länge basierend auf der verbleibenden Bildschirmbreite zu bestimmen
	// For all other cases, width is in pixels
	Coord width;

	// for text fields use height = -1 to determine height based on font size.  for all fields 0 means 'rest of screen'
	// Verwenden Sie für Textfelder height = -1, um die Höhe basierend auf der Schriftgröße zu bestimmen. für alle Felder bedeutet 0 "Rest des Bildschirms"

	// for other cases height is in pixels
	Coord height;

	BorderOp border; // an optional border to draw within this field

	ColorOp color :4;
	LayoutModifier modifier :4; // layouts can tell the field they are showing special rendering options

	AlignmentX align_x : 2; // Align the primary contents of this element within our bounds using this rule (defaults to centered)
	AlignmentY align_y : 2; // Align the primary contents of this element within our bounds using this rule (defaults to centered)
	AlignmentX label_align_x : 2; // Align the label using this rule
	AlignmentY label_align_y : 2; // Align the label using this rule
	AlignmentX unit_align_x : 2; // Align units using this rule
	AlignmentY unit_align_y : 2; // Align units using this rule
	uint8_t inset_x, inset_y; 		// inset primary content in from sides by this amount

	Field *field; // The field to render in this location

	const UG_FONT *font; // If this field requires a font, use this.  Or if NULL auto select the biggest font that can hold the string

	uint32_t old_editable; // a cache value only used for editable fields, used to compare against previous values and redraw if needed.

} FieldLayout;

/** Called when a press has occured, return true if this function has handled the event (and therefore it should be cleared)
 or false to let others handle it.

 The order of handlers is:
 * any active editable controls
 * any scrollable controls on this screen
 * the current screen handler
 * the top level application handler
 The top three handlers in the list above are handled by screen
 */
typedef bool (*ButtonEventHandler)(buttons_events_t events);

typedef struct {
	void (*onEnter)(); // If !NULL will be called whenever this screen is about to be shown (good to change globals etc)
	void (*onExit)(); // If !NULL will be called when this screen is no longer visible
	void (*onPreUpdate)(); // If !NULL, Called just before each update operation
	void (*onPostUpdate)(); // If !NULL, Called just after each update operation
	void (*onDirtyClean)(); // If !NULL, Called after screen is cleared because it is dirty, good to draw any mask
	void (*onCustomized)(); // If !NULL, called when the user has just customized fields with FieldCustomize (used to save to EEPROM)
	ButtonEventHandler onPress; // or NULL for no handler
	FieldLayout fields[];
} Screen;

// Standard vertical spacing for fonts
// #define FONT12_Y 14 // we want a little bit of extra space

void panicScreenShow(Screen *screen);
void screenShow(Screen *screen);
void screenUpdate();

/// Return the current visible screen
Screen* getCurrentScreen();

/// Returns true if the current screen handled the press
bool screenOnPress(buttons_events_t events);

// Set to true if we should automatically convert kph -> mph and km -> mi
extern bool screenConvertMiles;

// Set to true if we should automatically convert C -> F
extern bool screenConvertFarenheit;

void fieldPrintf(Field *field, const char *fmt, ...);

// Update this readonly editable with a string value, str must point to a static buffer
void updateReadOnlyStr(Field *field, char *str);

/** These are render callback functions, you should normally never need to call them, but they can be useful if you
 * are using your own custom render callback.
 */
bool renderDrawTextCommon(FieldLayout *layout, const char *msg);

extern const UG_FONT *editable_label_font;
extern const UG_FONT *editable_value_font;
extern const UG_FONT *editable_units_font;

// The default is for editables to be two rows tall, with the data value on the second row
// define this as 1 if you want them to be one row tall (because you have a wide enough screen)
// #define EDITABLE_NUM_ROWS 2

// Used to define  positions in terms of # of 1/8ths of screen width/height (i.e. 4 is middle, 3 is slightly to left etc)
#define XbyEighths(n) ((SCREEN_WIDTH * (n)) / 8)
#define YbyEighths(n) ((SCREEN_HEIGHT * (n)) / 8)
#define Xby64(n) ((SCREEN_WIDTH * (n)) / 64)
#define Yby64(n) ((SCREEN_HEIGHT * (n)) / 64)

// Define _S macro which returns long strings on devices with big screens, or short strings on tiny devices
// eventually we can replace _S with one of the internationalization libraries, and start generating builds
// with strings in German, Portuguese etc...
#ifndef SW102
#define _S(longstr, shortstr) longstr
#else
#define _S(longstr, shortstr) shortstr
#endif

#ifdef SW102
#define SCREENFN_FORCE_LABELS buttons_get_m_state()

#define SCREENCLICK_START_EDIT M_CLICK
#define SCREENCLICK_STOP_EDIT M_CLICK
#define SCREENCLICK_EXIT_SCROLLABLE ONOFF_CLICK
#define SCREENCLICK_NEXT_SCREEN ONOFF_CLICK

#define SCREENCLICK_START_CUSTOMIZING M_CLICK
#define SCREENCLICK_STOP_CUSTOMIZING M_CLICK
#else
#define SCREENFN_FORCE_LABELS false

#define SCREENCLICK_START_EDIT ONOFF_CLICK
#define SCREENCLICK_STOP_EDIT ONOFF_CLICK
#define SCREENCLICK_EXIT_SCROLLABLE ONOFF_CLICK
#define SCREENCLICK_NEXT_SCREEN UPDOWN_CLICK

#define SCREENCLICK_START_CUSTOMIZING ONOFF_CLICK
#define SCREENCLICK_STOP_CUSTOMIZING ONOFF_CLICK
#endif
