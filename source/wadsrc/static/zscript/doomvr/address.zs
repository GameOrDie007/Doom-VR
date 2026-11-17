/*
** address.zs - DoomVR's address box on the Join a Game page.
**
** Written for Doom VR. A text box like any other, except that in the headset
** it always opens with the on-screen character grid: a player in a headset
** (a Quest or a Frame above all) has no keyboard to type an address with,
** and picks its characters with the controller instead. On the monitor it is
** the plain text box.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

class OptionMenuItemDoomVRAddress : OptionMenuItemTextField
{
	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		if (mkey == Menu.MKEY_Enter)
		{
			CVar mode = CVar.FindCVar("vr_mode");
			if (mode != null && mode.GetInt() != 0) fromcontroller = true;	// the grid, in the headset
		}
		return Super.MenuEvent(mkey, fromcontroller);
	}
}
