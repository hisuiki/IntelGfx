/* SPDX-License-Identifier: MIT */
#include "BrightnessKeys.h"

#include <new>
#include <stdio.h>
#include <syslog.h>

#include <Bitmap.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Notification.h>
#include <Path.h>
#include <Screen.h>
#include <String.h>
#include <driver_settings.h>

// How much one press moves the backlight, and how dim it may get: a key that
// can reach zero is a key that turns the screen off with no way back.
static const float kStep = 0.05f;
static const float kMinimum = 0.05f;

// Presses repeat quickly when a key is held; the notification is only worth
// redrawing at a rate someone can read.
static const bigtime_t kNotifyInterval = 40000;

// A keyboard that reports these keys sends the display brightness usages of
// the HID consumer page, in the same encoding the volume keys arrive with:
// the page in the upper half, the usage in the lower. Keyboards that use
// something else are handled through the settings file.
static const int32 kDefaultRaiseKey = 0xc006f;	// display brightness increment
static const int32 kDefaultLowerKey = 0xc0070;	// display brightness decrement

static const char* kSettingsFile = "intel_gfx_brightness_keys";


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) BrightnessKeys();
}


BrightnessKeys::BrightnessKeys()
	:
	fRaiseKey(kDefaultRaiseKey),
	fLowerKey(kDefaultLowerKey),
	fReportKeys(false),
	fLastChange(0)
{
	_ReadSettings();
}


void
BrightnessKeys::_ReadSettings()
{
	void* handle = load_driver_settings(kSettingsFile);
	if (handle == NULL)
		return;

	const char* raise = get_driver_parameter(handle, "raise_key", NULL, NULL);
	if (raise != NULL)
		fRaiseKey = strtol(raise, NULL, 0);
	const char* lower = get_driver_parameter(handle, "lower_key", NULL, NULL);
	if (lower != NULL)
		fLowerKey = strtol(lower, NULL, 0);

	// With this set, every key press is written to the syslog with its code,
	// which is how the two keys above are found on a keyboard that does not
	// use the usual ones.
	fReportKeys = get_driver_boolean_parameter(handle, "report_keys", false,
		true);

	unload_driver_settings(handle);
}


status_t
BrightnessKeys::InitCheck()
{
	BScreen screen(B_MAIN_SCREEN_ID);
	float brightness;
	if (!screen.IsValid() || screen.GetBrightness(&brightness) != B_OK) {
		// No backlight to control: stay out of the way entirely rather than
		// swallowing keys that would do nothing.
		syslog(LOG_INFO, "intel_gfx brightness keys: no backlight, not "
			"taking part\n");
		return B_UNSUPPORTED;
	}

	syslog(LOG_INFO, "intel_gfx brightness keys: ready, raise 0x%" B_PRIx32
		", lower 0x%" B_PRIx32 ", reporting %s, backlight at %d%%\n",
		fRaiseKey, fLowerKey, fReportKeys ? "on" : "off",
		(int)(brightness * 100.0f + 0.5f));
	return B_OK;
}


void
BrightnessKeys::_Show(float brightness)
{
	bigtime_t now = system_time();
	if (now - fLastChange < kNotifyInterval)
		return;
	fLastChange = now;

	BNotification notification(B_PROGRESS_NOTIFICATION);
	// The same identifier every time, so that holding a key replaces the
	// notification rather than stacking up a column of them.
	notification.SetMessageID("intel_gfx_brightness");
	notification.SetGroup("Screen");
	notification.SetTitle("Brightness");
	BString content;
	content.SetToFormat("%d%%", (int)(brightness * 100.0f + 0.5f));
	notification.SetContent(content);
	notification.SetProgress(brightness);
	notification.Send(1500000);
}


void
BrightnessKeys::_Adjust(float step)
{
	BScreen screen(B_MAIN_SCREEN_ID);
	float brightness;
	if (screen.GetBrightness(&brightness) != B_OK)
		return;

	brightness += step;
	if (brightness > 1.0f)
		brightness = 1.0f;
	if (brightness < kMinimum)
		brightness = kMinimum;

	if (screen.SetBrightness(brightness) != B_OK)
		return;

	_Show(brightness);
}


filter_result
BrightnessKeys::Filter(BMessage* message, BList* _list)
{
	if (message->what != B_KEY_DOWN && message->what != B_UNMAPPED_KEY_DOWN)
		return B_DISPATCH_MESSAGE;

	int32 key;
	if (message->FindInt32("key", &key) != B_OK)
		return B_DISPATCH_MESSAGE;

	if (fReportKeys) {
		int32 modifiers = 0;
		message->FindInt32("modifiers", &modifiers);
		syslog(LOG_INFO, "intel_gfx brightness keys: key 0x%" B_PRIx32
			", modifiers 0x%" B_PRIx32 "\n", key, modifiers);
	}

	if (key == fRaiseKey)
		_Adjust(kStep);
	else if (key == fLowerKey)
		_Adjust(-kStep);
	else
		return B_DISPATCH_MESSAGE;

	// The key did something, so nothing else should see it as a character.
	return B_SKIP_MESSAGE;
}
