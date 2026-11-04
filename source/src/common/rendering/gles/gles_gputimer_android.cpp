/*
** gles_gputimer_android.cpp
**
** DoomVR: how long the GPU spends on each frame, on a standalone headset
** (GL_EXT_disjoint_timer_query - every Adreno has it). One query per frame,
** read back a few frames later so the game never waits on the GPU; the
** 15-second performance line in the log prints the average and the worst.
** Only the headset build compiles this; the OpenGL ES 3 functions come
** straight from libGLESv3, not the engine's loader.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 Game Or Die
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <string.h>

namespace
{
	constexpr int kRing = 4;
	GLuint gQueries[kRing] = {};
	bool gPending[kRing] = {};
	int gNext = 0;			// the query the next frame uses
	bool gBegun = false;
	int gHave = -1;			// -1 unknown, 0 no extension, 1 ready
	double gSumMs = 0, gMaxMs = 0;
	int gFrames = 0;

	bool Ready()
	{
		if (gHave < 0)
		{
			const char* ext = (const char*)glGetString(GL_EXTENSIONS);
			gHave = (ext && strstr(ext, "GL_EXT_disjoint_timer_query")) ? 1 : 0;
			if (gHave) glGenQueries(kRing, gQueries);
		}
		return gHave == 1;
	}

	// Collect every finished query: the ring is small, so a frame's result
	// is normally in by the time its slot comes round again.
	void Collect(bool wait)
	{
		for (int i = 0; i < kRing; ++i)
		{
			if (!gPending[i]) continue;
			GLuint available = wait ? 1 : 0;
			if (!wait) glGetQueryObjectuiv(gQueries[i], GL_QUERY_RESULT_AVAILABLE, &available);
			if (!available) continue;
			GLuint ns = 0;
			glGetQueryObjectuiv(gQueries[i], GL_QUERY_RESULT, &ns);
			gPending[i] = false;
			GLint disjoint = 0;
			glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
			if (disjoint) continue;	// the clock jumped (power state); this one is not a measurement
			const double ms = ns / 1.0e6;
			gSumMs += ms;
			if (ms > gMaxMs) gMaxMs = ms;
			++gFrames;
		}
	}
}

void DoomVR_GpuTimerBegin()
{
	if (!Ready() || gBegun) return;
	Collect(false);
	if (gPending[gNext]) Collect(true);	// the ring wrapped onto a result still out: wait for it, once
	glBeginQuery(GL_TIME_ELAPSED_EXT, gQueries[gNext]);
	gBegun = true;
}

void DoomVR_GpuTimerEnd()
{
	if (!gBegun) return;
	glEndQuery(GL_TIME_ELAPSED_EXT);
	gPending[gNext] = true;
	gNext = (gNext + 1) % kRing;
	gBegun = false;
}

// The frames measured since the last call, then the counters start again.
bool DoomVR_GpuTimerStats(double& avgMs, double& maxMs, int& frames)
{
	if (gHave != 1 || gFrames == 0) { avgMs = maxMs = 0; frames = 0; return false; }
	avgMs = gSumMs / gFrames;
	maxMs = gMaxMs;
	frames = gFrames;
	gSumMs = gMaxMs = 0;
	gFrames = 0;
	return true;
}
