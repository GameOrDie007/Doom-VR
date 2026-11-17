/*
** intermission.zs
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2006-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

extend class ScreenJobRunner
{
	protected native static void ReadyPlayer();
	protected native static void ResetReadyTimer();

	native static int GetReadyTimer();
	native static bool IsPlayerReady(int pNum);

	bool ConsumedInput(InputEvent evt)
	{
		if (netgame && evt.type == InputEvent.Type_KeyDown
			&& (evt.KeyScan == InputEvent.Key_Space || evt.KeyScan == InputEvent.Key_Mouse1
				|| evt.KeyScan == InputEvent.Key_Pad_A || evt.KeyScan == InputEvent.Key_Joy1))
		{
			ReadyPlayer();
			return true;
		}

		return false;
	}

	void DrawReadiedPlayers(double smoothratio)
	{
		if (!netgame || GetSkipType() == ST_UNSKIPPABLE)
			return;

		if (net_cutscenereadytype == 0)
		{
			int totalClients, readyClients;
			for (int i; i < MAXPLAYERS; ++i)
			{
				if (!playerInGame[i] || players[i].Bot)
					continue;

				++totalClients;
				readyClients += IsPlayerReady(i);
			}

			if (totalClients > 1)
			{
				TextureID readyico = TexMan.CheckForTexture("READYICO", TexMan.Type_MiscPatch);
				Vector2 readysize = TexMan.GetScaledSize(readyico);

				if (IsPlayerReady(consoleplayer))
					Screen.DrawTexture(readyico, true, 0, 0, DTA_CleanNoMove, true, DTA_TopLeft, true);

				// DoomVR: who is ready and the countdown, large and in the middle
				// of the bottom where a headset player looks (the corner was out
				// of sight in the headset, and the screen seemed stuck).
				int startTimer = GetReadyTimer();
				string readyLine = String.Format("Ready %d of %d", readyClients, totalClients);
				int col = Font.CR_GOLD;
				if (startTimer > 0 && readyClients < totalClients)
				{
					readyLine = readyLine .. String.Format(" - next map in %d", int(ceil(double(startTimer) / GameTicRate)));
					if (startTimer <= GameTicRate * 5)
						col = Font.CR_RED;
				}
				// Twice the clean size, but never wider than the screen: with the
				// countdown on it the line ran off both sides of the headset's screen.
				double big = CleanXFac * 2;
				double lineW = ConFont.StringWidth(readyLine);
				if (lineW * big > Screen.GetWidth() * 0.92)
					big = max(1.0, Screen.GetWidth() * 0.92 / lineW);
				int lx = int((Screen.GetWidth() - lineW * big) / 2);
				int ly = int(Screen.GetHeight() - ConFont.GetHeight() * (CleanYFac + big) - CleanYFac * 4);
				Screen.DrawText(ConFont, col, lx, ly, readyLine, DTA_ScaleX, big, DTA_ScaleY, big);
			}
		}

		if (net_cutscenereadytype != 2 || consoleplayer == Net_Arbitrator)
		{
			if (IsPlayerReady(consoleplayer))
			{
				string waitTxt = "You are ready - waiting for the others";
				int wx = (Screen.GetWidth() - ConFont.StringWidth(waitTxt) * CleanXFac) / 2;
				int wy = Screen.GetHeight() - ConFont.GetHeight() * CleanYFac - CleanYFac;
				Screen.DrawText(ConFont, Font.CR_GREEN, wx, wy, waitTxt, DTA_CleanNoMove, true);
				return;
			}
			CVar vrm = CVar.FindCVar("vr_mode");
			if (vrm && vrm.GetInt() != 0)
			{
				string vrTxt = "Press A or the trigger to ready up";
				int vx = (Screen.GetWidth() - ConFont.StringWidth(vrTxt) * CleanXFac) / 2;
				int vy = Screen.GetHeight() - ConFont.GetHeight() * CleanYFac - CleanYFac;
				Screen.DrawText(ConFont, Font.CR_GREEN, vx, vy, vrTxt, DTA_CleanNoMove, true);
				return;
			}
			string contType;
			switch (GetLastInputType())
			{
				case INP_KEYBOARD_MOUSE:
					contType = "$NET_CONTINUE_MKB";
					break;
				case INP_CONTROLLER:
					contType = "$NET_CONTINUE_CONTROLLER";
					break;
				case INP_JOYSTICK:
					contType = "$NET_CONTINUE_JOYSTICK";
					break;
			}

			string contTxt = StringTable.Localize(contType);
			int xOfs = (Screen.GetWidth() - ConFont.StringWidth(contTxt) * CleanXFac) / 2;
			int yOfs = Screen.GetHeight() - ConFont.GetHeight() * CleanYFac - CleanYFac;
			Screen.DrawText(ConFont, Font.CR_GREEN, xOfs, yOfs, contTxt, DTA_CleanNoMove, true);
		}
	}
}

class IntermissionController native ui
{
	// This is mostly a black box to the native intermission code.
	// May be scriptified later, but right now we do not need it.

	native void Start();
	native bool Responder(InputEvent ev);
	native bool Ticker();
	native void Drawer();
	native bool NextPage();
}

// Wrapper to play the native intermissions within a screen job.
class IntermissionScreenJob : ScreenJob
{
	IntermissionController controller;

	ScreenJob Init(IntermissionController ctrl, bool allowwipe)
	{
		Super.Init();
		if (allowwipe && wipetype != 0) flags = wipetype << ScreenJob.transition_shift;
		controller = ctrl;
		return self;
	}

	override void Start() { controller.Start(); }
	override bool OnEvent(InputEvent evt) { return controller.Responder(evt); }
	override void OnTick() { if (!controller.Ticker()) jobstate = finished; }
	override void Draw(double smoothratio) { controller.Drawer(); }

	override void OnDestroy()
	{
		if (controller)
			controller.Destroy();
		Super.OnDestroy();
	}
}


class DoomCutscenes ui
{
	//---------------------------------------------------------------------------
	//
	//
	//
	//---------------------------------------------------------------------------

	static void BuildMapTransition(ScreenJobRunner runner, IntermissionController inter, StatusScreen status)
	{
		if (status)
		{
			runner.Append(status);
		}
		if (inter)
		{
			runner.Append(new("IntermissionScreenJob").Init(inter, status != null));
		}
	}
}
