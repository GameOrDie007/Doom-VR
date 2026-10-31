/*
** splash.zs - DoomVR's splash: one picture, shown once at startup ahead of
** whatever the game itself opens with.
**
** Written for Doom VR from UZDoom's ScreenJob interface.
**
** The picture is graphics/vrsplash.png or .jpg in boxart.pk3 - the player's
** own, or the one setup composes from their games' title screens. Drawn to
** fit whole in the screen, keeping its own shape (a 16:9 plate is not
** squeezed to 4:3 as the games' own 320x200 screens are). Not there means no
** splash, not a black screen. Any button skips it.
**
** SPDX-License-Identifier: GPL-3.0-or-later
*/

class DoomVRSplashScreen : SkippableScreenJob
{
	TextureID picture;
	int showMs;
	bool drawn;

	ScreenJob Init(TextureID tex, int ms)
	{
		Super.Init(fadein | fadeout);
		picture = tex;
		showMs = ms;
		return self;
	}

	override void OnTick()
	{
		// Counted from the first frame actually drawn, so a slow start does
		// not eat the time the picture is on screen.
		if (drawn && ticks * 1000 / GameTicRate > showMs) jobstate = finished;
	}

	override void Draw(double smooth)
	{
		Screen.DrawTexture(picture, true, 0, 0, DTA_FullscreenEx, FSMode_ScaleToFit, DTA_LegacyRenderStyle, STYLE_Normal);
		drawn = true;
	}
}

// A ui class the intro in gameinfo names; C++ finds Create by name.
class DoomVRSplash ui
{
	static void Create(ScreenJobRunner runner)
	{
		TextureID tex = TexMan.CheckForTexture("graphics/vrsplash.png", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
		if (!tex.IsValid()) tex = TexMan.CheckForTexture("graphics/vrsplash.jpg", TexMan.Type_Any, TexMan.TryAny | TexMan.ForceLookup);
		if (!tex.IsValid()) return;
		runner.Append(new("DoomVRSplashScreen").Init(tex, 2600));
	}
}
