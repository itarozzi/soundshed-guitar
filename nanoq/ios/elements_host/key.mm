/*=============================================================================
   Elements' host layer for UIKit: a hardware keyboard's keys.

   UIKey reports a key as its USB HID usage (page 7, UIKeyboardHIDUsage), the
   same on every keyboard layout, as the Cocoa host's virtual key codes are.

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#include <elements/base_view.hpp>
#import <UIKit/UIKit.h>

namespace cycfi::elements
{
   key_code translate_key(long usage)
   {
      // Letters and digits run in order.
      if (usage >= 0x04 && usage <= 0x1D)
         return key_code(int(key_code::a) + int(usage - 0x04));
      if (usage >= 0x1E && usage <= 0x26)
         return key_code(int(key_code::_1) + int(usage - 0x1E));
      if (usage >= 0x3A && usage <= 0x45)
         return key_code(int(key_code::f1) + int(usage - 0x3A));
      if (usage >= 0x68 && usage <= 0x73)
         return key_code(int(key_code::f13) + int(usage - 0x68));
      if (usage >= 0x59 && usage <= 0x61)
         return key_code(int(key_code::kp_1) + int(usage - 0x59));

      switch (usage)
      {
         case 0x27:  return key_code::_0;
         case 0x28:  return key_code::enter;
         case 0x29:  return key_code::escape;
         case 0x2A:  return key_code::backspace;
         case 0x2B:  return key_code::tab;
         case 0x2C:  return key_code::space;
         case 0x2D:  return key_code::minus;
         case 0x2E:  return key_code::equal;
         case 0x2F:  return key_code::left_bracket;
         case 0x30:  return key_code::right_bracket;
         case 0x31:  return key_code::backslash;
         case 0x32:  return key_code::world_1;
         case 0x33:  return key_code::semicolon;
         case 0x34:  return key_code::apostrophe;
         case 0x35:  return key_code::grave_accent;
         case 0x36:  return key_code::comma;
         case 0x37:  return key_code::period;
         case 0x38:  return key_code::slash;
         case 0x39:  return key_code::caps_lock;
         case 0x46:  return key_code::print_screen;
         case 0x47:  return key_code::scroll_lock;
         case 0x48:  return key_code::pause;
         case 0x49:  return key_code::insert;
         case 0x4A:  return key_code::home;
         case 0x4B:  return key_code::page_up;
         case 0x4C:  return key_code::_delete;
         case 0x4D:  return key_code::end;
         case 0x4E:  return key_code::page_down;
         case 0x4F:  return key_code::right;
         case 0x50:  return key_code::left;
         case 0x51:  return key_code::down;
         case 0x52:  return key_code::up;
         case 0x53:  return key_code::num_lock;
         case 0x54:  return key_code::kp_divide;
         case 0x55:  return key_code::kp_multiply;
         case 0x56:  return key_code::kp_subtract;
         case 0x57:  return key_code::kp_add;
         case 0x58:  return key_code::kp_enter;
         case 0x62:  return key_code::kp_0;
         case 0x63:  return key_code::kp_decimal;
         case 0x64:  return key_code::world_2;
         case 0x65:  return key_code::menu;
         case 0x67:  return key_code::kp_equal;
         case 0xE0:  return key_code::left_control;
         case 0xE1:  return key_code::left_shift;
         case 0xE2:  return key_code::left_alt;
         case 0xE3:  return key_code::left_super;
         case 0xE4:  return key_code::right_control;
         case 0xE5:  return key_code::right_shift;
         case 0xE6:  return key_code::right_alt;
         case 0xE7:  return key_code::right_super;
         default:    return key_code::unknown;
      }
   }

   // Command is the action key, as on macOS.
   int translate_flags(UIKeyModifierFlags flags)
   {
      int mods = 0;

      if (flags & UIKeyModifierShift)
         mods |= mod_shift;
      if (flags & UIKeyModifierControl)
         mods |= mod_control;
      if (flags & UIKeyModifierAlternate)
         mods |= mod_alt;
      if (flags & UIKeyModifierCommand)
         mods |= mod_super | mod_action;

      return mods;
   }
}
