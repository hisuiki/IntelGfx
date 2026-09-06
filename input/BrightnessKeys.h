/* SPDX-License-Identifier: MIT */
#ifndef INTEL_GFX_BRIGHTNESS_KEYS_H
#define INTEL_GFX_BRIGHTNESS_KEYS_H

#include <InputServerFilter.h>

extern "C" _EXPORT BInputServerFilter* instantiate_input_filter();

// Turns the brightness keys of a laptop keyboard into backlight changes,
// which nothing in the system does on its own: the keys arrive as ordinary
// key presses and are then dropped for want of anyone interested.
class BrightnessKeys : public BInputServerFilter {
public:
								BrightnessKeys();

	virtual	filter_result		Filter(BMessage* message, BList* _list);
	virtual	status_t			InitCheck();

private:
			void				_Adjust(float step);
			void				_Show(float brightness);

			// Which key codes to act on. They are read from a settings file
			// because keyboards disagree about them, and a laptop whose keys
			// are not the usual ones would otherwise need a new build.
			void				_ReadSettings();

			int32				fRaiseKey;
			int32				fLowerKey;
			bool				fReportKeys;
			bigtime_t			fLastChange;
};

#endif
