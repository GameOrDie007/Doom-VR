/*
** build.zs - DoomVR's Build Your Own Game page: a game you own plus maps, a
** gameplay mod, looks, sounds and music, with what does not fit grayed.
**
** Written for Doom VR. The rows, the
** rules and the launcher it writes are C++ (doomvr_build.cpp), from the
** catalogue setup writes; this draws them: the marker and color of each row
** and the chosen game's cover. Why a row is gray is the row's tooltip.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

// One row: a piece, a "none" for its kind, or Play. doomvr_build.cpp sets
// the label, the state and the tooltip (the reason) after every pick.
class OptionMenuItemDoomVRPick : OptionMenuItemCommand
{
	int mState;     // 0 free, 1 chosen, 2 picking it takes something off, 3 cannot

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		int color = selected ? OptionMenuSettings.mFontColorSelection
		          : mState == 1 ? OptionMenuSettings.mFontColorValue
		          : OptionMenuSettings.mFontColorMore;
		int x = drawLabel(indent, y, color, mState >= 2 && !selected);
		return mCentered ? x - 16 * CleanXfac_1 : indent;
	}

	override bool Activate()
	{
		if (mState == 3)
		{
			Menu.MenuSound("menu/invalid");
			return true;
		}
		return Super.Activate();
	}
}

class DoomVRBuildMenu : DoomVRGameSelectMenu
{
	// Open on the chosen game.
	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		for (int i = 0; i < mDesc.mItems.Size(); i++)
		{
			let it = OptionMenuItemDoomVRPick(mDesc.mItems[i]);
			if (it && it.mState == 1)
			{
				mDesc.mSelectedItem = i;
				mDesc.mScrollPos = max(0, i - 4);
				break;
			}
		}
	}

	// The chosen game's cover, not the highlighted row's.
	override TextureID Cover(int item)
	{
		let cv = CVar.FindCVar('vr_build_cover');
		return CoverFor(cv ? cv.GetString() : "");
	}

	// A pick changes the highlighted row's text without moving the cursor,
	// which is when the menu would otherwise pick it up.
	override void Drawer()
	{
		String tip = GetSelectedTooltip();
		if (tip != mCurrentTooltip) UpdateTooltip(tip);
		Super.Drawer();
	}
}
