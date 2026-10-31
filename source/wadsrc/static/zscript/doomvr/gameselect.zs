/*
** gameselect.zs - DoomVR's Switch Game page: the list, grouped by game, with
** the highlighted game's cover beside it, opening on the game being played.
**
** Written for Doom VR from the Switch Game design of our earlier ports. The rows are built in C++
** (doomvr_switchgame.cpp) from the launchers setup writes; this adds the
** picture and the starting row.
**
** Behind it all, the backdrop (graphics/vrbackdrop in boxart.pk3, else the
** splash, graphics/vrsplash; the player's own if they made one), darkened
** and softened, with a dark card
** behind the list: the page reads the same over any game's title screen.
**
** Covers are boxart/<game name>.png or .jpg in boxart.pk3, which setup builds
** (the player's own art where they have made it, else one generated from the
** game's own title screen) and the engine loads for every game. No pack, or
** no cover for a game, leaves the plain list - never an error.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

class DoomVRGameSelectMenu : OptionMenu
{
	const COVER_HEIGHT = 0.56;       // of the screen's height: clear of the list's card
	const COVER_MARGIN = 0.02;       // from the right edge, of the screen's width
	const COVER_ASPECT = 2.0 / 3.0;  // 600 x 900

	const CARD_WIDTH = 0.50;         // the list's card, of the screen's width
	const BLUR = 0.0045;             // the backdrop's softening, of the screen's height

	int coverItem;
	TextureID coverTex;
	TextureID backdrop;

	// The PC's mouse picks no row until it moves: a cursor left resting over
	// the window - where the shortcut was double-clicked - selected the row
	// under it as the picker opened, so it opened mid-list. The first position
	// seen is only noted; a move past MOUSE_STILL from it, a click, or the
	// headset's own pointer (which moves) works as before.
	const MOUSE_STILL = 12;
	bool mouseMoved;
	int mouseX0, mouseY0;
	bool mouseSeen;

	override bool MouseEvent(int type, int x, int y)
	{
		if (type == MOUSE_Move && !mouseMoved)
		{
			if (!mouseSeen) { mouseSeen = true; mouseX0 = x; mouseY0 = y; return true; }
			if (abs(x - mouseX0) < MOUSE_STILL && abs(y - mouseY0) < MOUSE_STILL) return true;
			mouseMoved = true;
		}
		if (type == MOUSE_Click) mouseMoved = true;
		return Super.MouseEvent(type, x, y);
	}

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		coverItem = -2;
		mouseSeen = false;
		mouseMoved = false;
		// The backdrop picture when there is one (no title text on it), else the splash.
		backdrop = Picture("graphics/vrbackdrop");
		if (!backdrop.IsValid()) backdrop = Picture("graphics/vrsplash");

		// Always open at the top of the list, not in the middle:
		// on the first game. Both are the first game row - a command row
		// that switches - so the page starts scrolled to the top.
		mDesc.mScrollPos = 0;
		for (int i = 0; i < mDesc.mItems.Size(); i++)
		{
			let it = OptionMenuItemCommand(mDesc.mItems[i]);
			if (it && it.Selectable())
			{
				mDesc.mSelectedItem = i;
				return;
			}
		}
	}

	// Looked up when the selection changes, not every frame: the first lookup
	// of a path builds the texture, and this page is drawn every frame.
	virtual TextureID Cover(int item)
	{
		if (item == coverItem) return coverTex;
		coverItem = item;
		coverTex.SetInvalid();
		if (item < 0 || item >= mDesc.mItems.Size()) return coverTex;
		let it = OptionMenuItem(mDesc.mItems[item]);
		if (it == null || !it.Selectable() || it.mLabel.Length() == 0) return coverTex;
		String label = it.mLabel;
		if (label.IndexOf("Continue: ") == 0) label = label.Mid(10);	// the game it continues
		coverTex = CoverFor(label);
		return coverTex;
	}

	String coverName;
	TextureID coverNamed;

	// A game's cover by its name. A build from Build Your Own Game ("Doom II
	// + Brutal Doom") has its game's.
	TextureID CoverFor(String label)
	{
		if (label == coverName) return coverNamed;
		coverName = label;
		coverNamed.SetInvalid();
		for (int pass = 0; pass < 2 && !coverNamed.IsValid() && label.Length() > 0; pass++)
		{
			String base = "boxart/" .. label;
			coverNamed = TexMan.CheckForTexture(base .. ".png", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
			if (!coverNamed.IsValid())
				coverNamed = TexMan.CheckForTexture(base .. ".jpg", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
			int plus = label.IndexOf(" + ");
			if (plus < 0) break;
			label = label.Left(plus);
		}
		return coverNamed;
	}

	TextureID Picture(String name)
	{
		TextureID t = TexMan.CheckForTexture(name .. ".png", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
		if (!t.IsValid()) t = TexMan.CheckForTexture(name .. ".jpg", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
		return t;
	}

	// The backdrop filling the screen (cropped, never stretched), softened by
	// faint copies a few pixels to each side, then darkened; no splash, the
	// darkening alone. Then the card the list sits on, edged in turquoise.
	void DrawBackdrop()
	{
		double sw = Screen.GetWidth(), sh = Screen.GetHeight();
		if (backdrop.IsValid())
		{
			int tw, th;
			[tw, th] = TexMan.GetSize(backdrop);
			if (tw > 0 && th > 0)
			{
				double k = max(sw / tw, sh / th);
				double w = tw * k, h = th * k;
				double x = (sw - w) * 0.5, y = (sh - h) * 0.5;
				Screen.DrawTexture(backdrop, false, x, y, DTA_DestWidthF, w, DTA_DestHeightF, h);
				double d = sh * BLUR;
				for (int i = -1; i <= 1; i++)
					for (int j = -1; j <= 1; j++)
						if (i != 0 || j != 0)
							Screen.DrawTexture(backdrop, false, x + i * d, y + j * d, DTA_DestWidthF, w, DTA_DestHeightF, h, DTA_Alpha, 0.22);
			}
		}
		Screen.Dim(0x000000, backdrop.IsValid() ? 0.55 : 0.85, 0, 0, int(sw), int(sh));

		int cw = int(sw * CARD_WIDTH), cx = int((sw - cw) * 0.5);
		int cy = int(sh * 0.008), ch = int(sh * 0.984);
		Screen.Dim(0x0B0614, 0.8, cx, cy, cw, ch);
		int edge = max(2, int(sh * 0.003));
		Screen.Clear(cx, cy, cx + cw, cy + edge, 0xFF03C1CB);
		Screen.Clear(cx, cy + ch - edge, cx + cw, cy + ch, 0xFF03C1CB);
	}

	override void Drawer()
	{
		DrawBackdrop();
		Super.Drawer();
		TextureID tex = Cover(mDesc.mSelectedItem);
		if (!tex.IsValid()) return;

		double sw = Screen.GetWidth(), sh = Screen.GetHeight();
		double h = sh * COVER_HEIGHT;
		double w = h * COVER_ASPECT;
		double x = sw - w - sw * COVER_MARGIN;
		double y = (sh - h) * 0.5;
		// A dark plate a little larger than the cover gives it an edge.
		double b = sh * 0.008;
		Screen.Dim(0x000000, 0.65, int(x - b), int(y - b), int(w + b * 2), int(h + b * 2));
		Screen.DrawTexture(tex, false, x, y, DTA_DestWidthF, w, DTA_DestHeightF, h);
	}
}
