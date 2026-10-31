/*
** gl_stereo3d.cpp
**
** Stereoscopic 3D API
**
**---------------------------------------------------------------------------
**
** Copyright 2015 Christopher Bruns
** Copyright 2016-2021 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "gl_system.h"
#include "gl_renderer.h"
#include "gl_renderbuffers.h"
#include "hw_vrmodes.h"
#include "doomvr_headset.h"
#include "c_dispatch.h"
#include "gl_framebuffer.h"
#include "gl_postprocessstate.h"
#include "gl_framebuffer.h"
#include "gl_shaderprogram.h"
#include "gl_buffers.h"
#include "menu.h"
#include "printf.h"


EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Float, vid_i_whitepoint)
EXTERN_CVAR(Float, vid_i_blackpoint)
EXTERN_CVAR(Float, vid_saturation)
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Int, gl_satformula)
EXTERN_CVAR(Int, gl_dither_bpc)

#ifdef _WIN32
EXTERN_CVAR(Bool, vr_enable_quadbuffered)
#endif

void UpdateVRModes(bool considerQuadBuffered)
{
	FOptionValues** pVRModes = OptionValues.CheckKey("VRMode");
	if (pVRModes == nullptr) return;

	TArray<FOptionValues::Pair>& vals = (*pVRModes)->mValues;
	TArray<FOptionValues::Pair> filteredValues;
	int cnt = vals.Size();
	for (int i = 0; i < cnt; ++i) {
		auto const& mode = vals[i];
		if (mode.Value == 7) {  // Quad-buffered stereo
#ifdef _WIN32
			if (!vr_enable_quadbuffered) continue;
#else
			continue;  // Remove quad-buffered option on Mac and Linux
#endif
			if (!considerQuadBuffered) continue;  // Probably no compatible screen mode was found
		}
		filteredValues.Push(mode);
	}
	vals = filteredValues;
}

namespace OpenGLRenderer
{

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentAnaglyph(bool r, bool g, bool b)
{
	mBuffers->BindOutputFB();
	ClearBorders();

	glColorMask(r, g, b, 1);
	mBuffers->BindEyeTexture(0, 0);
 	DrawPresentTexture(screen->mOutputLetterbox, true);

	glColorMask(!r, !g, !b, 1);
	mBuffers->BindEyeTexture(1, 0);
	DrawPresentTexture(screen->mOutputLetterbox, true);

	glColorMask(1, 1, 1, 1);
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentSideBySide(int vrmode)
{
	if (vrmode == VR_SIDEBYSIDEFULL || vrmode == VR_SIDEBYSIDESQUISHED)
	{
		mBuffers->BindOutputFB();
		ClearBorders();

		// Compute screen regions to use for left and right eye views
		int leftWidth = screen->mOutputLetterbox.width / 2;
		int rightWidth = screen->mOutputLetterbox.width - leftWidth;
		IntRect leftHalfScreen = screen->mOutputLetterbox;
		leftHalfScreen.width = leftWidth;
		IntRect rightHalfScreen = screen->mOutputLetterbox;
		rightHalfScreen.width = rightWidth;
		rightHalfScreen.left += leftWidth;

		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(leftHalfScreen, true);

		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(rightHalfScreen, true);
	}
	else if (vrmode == VR_SIDEBYSIDELETTERBOX)
	{
		mBuffers->BindOutputFB();
		screen->mOutputLetterbox.top = screen->mOutputLetterbox.height;

		ClearBorders();
		screen->mOutputLetterbox.top = 0;  //reset so screenshots can be taken

		// Compute screen regions to use for left and right eye views
		int leftWidth = screen->mOutputLetterbox.width / 2;
		int rightWidth = screen->mOutputLetterbox.width - leftWidth;
		//cut letterbox height in half
		int height = screen->mOutputLetterbox.height / 2;
		int top = height * .5;
		IntRect leftHalfScreen = screen->mOutputLetterbox;
		leftHalfScreen.width = leftWidth;
		leftHalfScreen.height = height;
		leftHalfScreen.top = top;
		IntRect rightHalfScreen = screen->mOutputLetterbox;
		rightHalfScreen.width = rightWidth;
		rightHalfScreen.left += leftWidth;
		//give it those cinematic black bars on top and bottom
		rightHalfScreen.height = height;
		rightHalfScreen.top = top;

		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(leftHalfScreen, true);

		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(rightHalfScreen, true);
	}
}


//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentTopBottom()
{
	mBuffers->BindOutputFB();
	ClearBorders();

	// Compute screen regions to use for left and right eye views
	int topHeight = screen->mOutputLetterbox.height / 2;
	int bottomHeight = screen->mOutputLetterbox.height - topHeight;
	IntRect topHalfScreen = screen->mOutputLetterbox;
	topHalfScreen.height = topHeight;
	topHalfScreen.top = topHeight;
	IntRect bottomHalfScreen = screen->mOutputLetterbox;
	bottomHalfScreen.height = bottomHeight;
	bottomHalfScreen.top = 0;

	mBuffers->BindEyeTexture(0, 0);
	DrawPresentTexture(topHalfScreen, true);

	mBuffers->BindEyeTexture(1, 0);
	DrawPresentTexture(bottomHalfScreen, true);
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::prepareInterleavedPresent(FPresentShaderBase& shader)
{
	mBuffers->BindOutputFB();
	ClearBorders();


	// Bind each eye texture, for composition in the shader
	mBuffers->BindEyeTexture(0, 0);
	mBuffers->BindEyeTexture(1, 1);

	glActiveTexture(GL_TEXTURE0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glActiveTexture(GL_TEXTURE1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	const IntRect& box = screen->mOutputLetterbox;
	glViewport(box.left, box.top, box.width, box.height);

	shader.Bind();

	if (framebuffer->IsHWGammaActive())
	{
		shader.Uniforms->InvGamma = 1.0f;
		shader.Uniforms->Contrast = 1.0f;
		shader.Uniforms->Saturation = 1.0f;
		shader.Uniforms->BlackPoint = 0.0f;
		shader.Uniforms->WhitePoint = 1.0f;
	}
	else
	{
		shader.Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		shader.Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		shader.Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.0f);
		shader.Uniforms->BlackPoint = clamp<float>(vid_i_blackpoint, 0.f, 1.f);
		shader.Uniforms->WhitePoint = clamp<float>(vid_i_whitepoint, 0.f, 5.f);
		shader.Uniforms->GrayFormula = static_cast<int>(gl_satformula);
	}

	shader.Uniforms->HdrMode = 0;
	shader.Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	shader.Uniforms->Scale = {
		screen->mScreenViewport.width / (float)mBuffers->GetWidth(),
		screen->mScreenViewport.height / (float)mBuffers->GetHeight()
	};
	shader.Uniforms->Offset = { 0.0f, 0.0f };
	shader.Uniforms.SetData();

	static_cast<GLDataBuffer*>(shader.Uniforms.GetBuffer())->BindBase();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentColumnInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dColumnShader);

	// Compute absolute offset from top of screen to top of current display window
	// because we need screen-relative, not window-relative, scan line parity

	// Todo:
	//auto clientoffset = screen->GetClientOffset();
	//auto windowHOffset = clientoffset.X % 2;
	int windowHOffset = 0;

	mPresent3dColumnShader->Uniforms->WindowPositionParity = windowHOffset;
	mPresent3dColumnShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dColumnShader->Uniforms.GetBuffer())->BindBase();

	RenderScreenQuad();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentRowInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dRowShader);

	// Todo:
	//auto clientoffset = screen->GetClientOffset();
	//auto windowVOffset = clientoffset.Y % 2;
	int windowVOffset = 0;

	mPresent3dRowShader->Uniforms->WindowPositionParity =
		(windowVOffset
			+ screen->mOutputLetterbox.height + 1 // +1 because of origin at bottom
			) % 2;

	mPresent3dRowShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dRowShader->Uniforms.GetBuffer())->BindBase();
	RenderScreenQuad();
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentCheckerInterleaved()
{
	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	prepareInterleavedPresent(*mPresent3dCheckerShader);

	// Compute absolute offset from top of screen to top of current display window
	// because we need screen-relative, not window-relative, scan line parity

	//auto clientoffset = screen->GetClientOffset();
	//auto windowHOffset = clientoffset.X % 2;
	//auto windowVOffset = clientoffset.Y % 2;
	int windowHOffset = 0;
	int windowVOffset = 0;

	mPresent3dCheckerShader->Uniforms->WindowPositionParity =
		(windowVOffset
			+ windowHOffset
			+ screen->mOutputLetterbox.height + 1 // +1 because of origin at bottom
			) % 2; // because we want the top pixel offset, but gl_FragCoord.y is the bottom pixel offset

	mPresent3dCheckerShader->Uniforms.SetData();
	static_cast<GLDataBuffer*>(mPresent3dCheckerShader->Uniforms.GetBuffer())->BindBase();
	RenderScreenQuad();
}

//==========================================================================
//
// Sometimes the stereo render context is not ready immediately at start up
//
//==========================================================================

bool FGLRenderer::QuadStereoCheckInitialRenderContextState()
{
	// Keep trying until we see at least one good OpenGL context to render to
	bool bQuadStereoSupported = false;
	bool bDecentContextWasFound = false;
	int contextCheckCount = 0;
	if ((!bDecentContextWasFound) && (contextCheckCount < 200))
	{
		contextCheckCount += 1;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);  // This question is about the main screen display context
		GLboolean supportsStereo, supportsBuffered;
		glGetBooleanv(GL_DOUBLEBUFFER, &supportsBuffered);
		if (supportsBuffered) // Finally, a useful OpenGL context
		{
			// This block will be executed exactly ONCE during a game run
			bDecentContextWasFound = true; // now we can stop checking every frame...
										   // Now check whether this context supports hardware stereo
			glGetBooleanv(GL_STEREO, &supportsStereo);
			bQuadStereoSupported = supportsStereo && supportsBuffered;
			if (! bQuadStereoSupported)
				UpdateVRModes(false);
		}
	}
	return bQuadStereoSupported;
}

//==========================================================================
//
//
//
//==========================================================================

void FGLRenderer::PresentQuadStereo()
{
	if (QuadStereoCheckInitialRenderContextState())
	{
		mBuffers->BindOutputFB();

		glDrawBuffer(GL_BACK_LEFT);
		ClearBorders();
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);

		glDrawBuffer(GL_BACK_RIGHT);
		ClearBorders();
		mBuffers->BindEyeTexture(1, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);

		glDrawBuffer(GL_BACK);
	}
	else
	{
		mBuffers->BindOutputFB();
		ClearBorders();
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);
	}
}


void FGLRenderer::PresentStereo()
{
	auto vrmode = VRMode::GetVRMode(true);
	const int eyeCount = vrmode->mEyeCount;
	// Don't invalidate the bound framebuffer (..., false)
	if (eyeCount > 1)
		mBuffers->BlitToEyeTexture(mBuffers->CurrentEye(), false);

	switch (vr_mode)
	{
	default:
		return;

	case VR_GREENMAGENTA:
		PresentAnaglyph(false, true, false);
		break;

	case VR_REDCYAN:
		PresentAnaglyph(true, false, false);
		break;

	case VR_AMBERBLUE:
		PresentAnaglyph(true, true, false);
		break;

	case VR_SIDEBYSIDEFULL:
	case VR_SIDEBYSIDESQUISHED:
	case VR_SIDEBYSIDELETTERBOX:
		PresentSideBySide(vr_mode);
		break;

	case VR_TOPBOTTOM:
		PresentTopBottom();
		break;

	case VR_ROWINTERLEAVED:
		PresentRowInterleaved();
		break;

	case VR_COLUMNINTERLEAVED:
		PresentColumnInterleaved();
		break;

	case VR_CHECKERINTERLEAVED:
		PresentCheckerInterleaved();
		break;

	case VR_QUADSTEREO:
		PresentQuadStereo();
		break;

	case VR_HEADSET:
		PresentHeadset();
		break;
	}
}


// DoomVR: the left eye as last captured for a screenshot; GetScreenshotBuffer
// hands it to UZDoom's screenshot instead of the desktop mirror, which is the
// eye squeezed into the window's shape.
FHeadsetShot HeadsetShot;

void FHeadsetShot::Capture(int w, int h)
{
	pixels.Resize(w * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, &pixels[0]);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	width = w;
	height = h;
	ready = true;
}

// DoomVR: the spectator view's draw. The
// left eye, finished exactly as the headset gets it, goes into a texture of
// our own; then DoomVR_SpectatorCrop's cut-out of it goes onto the window as
// one quad. Our own GL objects, and every piece of GL state touched here is
// put back: UZDoom's renderer keeps its own record of that state.
namespace
{
	struct FSpectatorGL
	{
		GLuint fbo = 0, tex = 0, prog = 0, vao = 0, vbo = 0;
		int w = 0, h = 0;
		bool failed = false;
	} gSpecGL;

	const char* const kSpectatorVS =
		"#version 330\n"
		"layout(location = 0) in vec2 aPos;\n"
		"layout(location = 1) in vec2 aUV;\n"
		"out vec2 vUV;\n"
		"void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
	const char* const kSpectatorFS =
		"#version 330\n"
		"in vec2 vUV;\n"
		"uniform sampler2D uTex;\n"
		"out vec4 oColor;\n"
		"void main() { oColor = vec4(texture(uTex, vUV).rgb, 1.0); }\n";

	// The 2D panel (menus, the HUD, the console) as this frame drew it, kept
	// for the observer view: the headset shows the panel as a layer of its
	// own, which the eye picture never has (else no pause menu on the
	// monitor). Premultiplied, as the compositor takes it.
	struct FPanelCopyGL
	{
		GLuint fbo = 0, tex = 0, prog = 0;
		int w = 0, h = 0;
		bool fresh = false, failed = false;
	} gPanelGL;
	const char* const kPanelFS =
		"#version 330\n"
		"in vec2 vUV;\n"
		"uniform sampler2D uTex;\n"
		"out vec4 oColor;\n"
		"void main() { oColor = texture(uTex, vUV); }\n";
	constexpr GLenum kTextureSrgbDecode = 0x8A48, kSkipDecode = 0x8A4A;	// EXT_texture_sRGB_decode

	GLuint CompileSpectatorShader(GLenum type, const char* src)
	{
		GLuint s = glCreateShader(type);
		glShaderSource(s, 1, &src, nullptr);
		glCompileShader(s);
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[512] = {};
			glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
			Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the spectator view's shader did not compile - the plain eye instead: %s\n", log);
			glDeleteShader(s);
			return 0;
		}
		return s;
	}

	// Made on first use, and the eye texture again when the eye size changes.
	// A failure is said once and the window goes back to the plain eye.
	bool EnsureSpectator(int w, int h)
	{
		if (gSpecGL.failed) return false;
		if (gSpecGL.prog == 0)
		{
			GLuint vs = CompileSpectatorShader(GL_VERTEX_SHADER, kSpectatorVS);
			GLuint fs = CompileSpectatorShader(GL_FRAGMENT_SHADER, kSpectatorFS);
			if (!vs || !fs) { gSpecGL.failed = true; return false; }
			GLuint p = glCreateProgram();
			glAttachShader(p, vs);
			glAttachShader(p, fs);
			glLinkProgram(p);
			glDeleteShader(vs);
			glDeleteShader(fs);
			GLint ok = 0;
			glGetProgramiv(p, GL_LINK_STATUS, &ok);
			if (!ok) { Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the spectator view's shader did not link - the plain eye instead\n"); glDeleteProgram(p); gSpecGL.failed = true; return false; }
			gSpecGL.prog = p;
			glGenVertexArrays(1, &gSpecGL.vao);
			glGenBuffers(1, &gSpecGL.vbo);
		}
		if (gSpecGL.w != w || gSpecGL.h != h || gSpecGL.fbo == 0)
		{
			if (gSpecGL.fbo) glDeleteFramebuffers(1, &gSpecGL.fbo);
			if (gSpecGL.tex) glDeleteTextures(1, &gSpecGL.tex);
			GLint oldTex = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
			glGenTextures(1, &gSpecGL.tex);
			glBindTexture(GL_TEXTURE_2D, gSpecGL.tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, oldTex);
			GLint oldFb = 0;
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldFb);
			glGenFramebuffers(1, &gSpecGL.fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, gSpecGL.fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gSpecGL.tex, 0);
			const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			glBindFramebuffer(GL_FRAMEBUFFER, oldFb);
			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the spectator view's framebuffer is incomplete (0x%x) - the plain eye instead\n", status);
				gSpecGL.failed = true;
				return false;
			}
			gSpecGL.w = w;
			gSpecGL.h = h;
		}
		return true;
	}

	// A picture onto whatever framebuffer is bound, filling (x,y)-(x+qw,y+qh).
	// corners: fractions of the picture, y DOWN; the texture's rows run up, as
	// GL's do, so v = 1 - y. over: laid on what is there, premultiplied;
	// otherwise the window is cleared first.
	void DrawQuad(GLuint program, GLuint texture, int x, int y, int qw, int qh, const float corners[8], bool over)
	{
		GLint prog, vao, buf, active, tex0, smp0, vp[4], bsrc, bdst, bsrca, bdsta, beq, beqa;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buf);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
		glGetIntegerv(GL_SAMPLER_BINDING, &smp0);
		glGetIntegerv(GL_VIEWPORT, vp);
		const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
			scissor = glIsEnabled(GL_SCISSOR_TEST), cull = glIsEnabled(GL_CULL_FACE), stencil = glIsEnabled(GL_STENCIL_TEST);

		glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc); glGetIntegerv(GL_BLEND_DST_RGB, &bdst);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrca); glGetIntegerv(GL_BLEND_DST_ALPHA, &bdsta);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beqa);

		glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
		if (over)
		{
			glEnable(GL_BLEND);
			glBlendEquation(GL_FUNC_ADD);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		}
		else
		{
			glViewport(x, y, qw, qh);
			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		glViewport(x, y, qw, qh);

		// Window corners TL, TR, BL, BR as a strip; each with its eye point.
		const int order[4] = { 0, 1, 3, 2 };
		const float pos[4][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
		float v[16];
		for (int i = 0; i < 4; ++i)
		{
			v[4 * i + 0] = pos[i][0];
			v[4 * i + 1] = pos[i][1];
			v[4 * i + 2] = corners[2 * order[i]];
			v[4 * i + 3] = 1.0f - corners[2 * order[i] + 1];
		}
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "uTex"), 0);
		glBindVertexArray(gSpecGL.vao);
		glBindBuffer(GL_ARRAY_BUFFER, gSpecGL.vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
		glBindTexture(GL_TEXTURE_2D, texture);
		glBindSampler(0, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		glBindSampler(0, smp0);
		glBindTexture(GL_TEXTURE_2D, tex0);
		glActiveTexture(active);
		glBindBuffer(GL_ARRAY_BUFFER, buf);
		glBindVertexArray(vao);
		glUseProgram(prog);
		glViewport(vp[0], vp[1], vp[2], vp[3]);
		glBlendFuncSeparate(bsrc, bdst, bsrca, bdsta);
		glBlendEquationSeparate(beq, beqa);
		if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
		if (depth) glEnable(GL_DEPTH_TEST);
		if (scissor) glEnable(GL_SCISSOR_TEST);
		if (cull) glEnable(GL_CULL_FACE);
		if (stencil) glEnable(GL_STENCIL_TEST);
	}

	void DrawSpectatorQuad(int cw, int ch, const float corners[8])
	{
		DrawQuad(gSpecGL.prog, gSpecGL.tex, 0, 0, cw, ch, corners, false);
	}

	// The panel's copy: made on first use and again when its size changes.
	bool EnsurePanelCopy(int w, int h)
	{
		if (gPanelGL.failed) return false;
		if (gPanelGL.prog == 0)
		{
			GLuint vs = CompileSpectatorShader(GL_VERTEX_SHADER, kSpectatorVS);
			GLuint fs = CompileSpectatorShader(GL_FRAGMENT_SHADER, kPanelFS);
			if (!vs || !fs) { gPanelGL.failed = true; return false; }
			GLuint p = glCreateProgram();
			glAttachShader(p, vs);
			glAttachShader(p, fs);
			glLinkProgram(p);
			glDeleteShader(vs);
			glDeleteShader(fs);
			GLint ok = 0;
			glGetProgramiv(p, GL_LINK_STATUS, &ok);
			if (!ok) { glDeleteProgram(p); gPanelGL.failed = true; return false; }
			gPanelGL.prog = p;
		}
		if (gPanelGL.w != w || gPanelGL.h != h || gPanelGL.fbo == 0)
		{
			if (gPanelGL.fbo) glDeleteFramebuffers(1, &gPanelGL.fbo);
			if (gPanelGL.tex) glDeleteTextures(1, &gPanelGL.tex);
			GLint oldTex = 0, oldFb = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldFb);
			// The panel's own format, so the copy is a byte copy; read back
			// undecoded, as the 2D wrote finished values into it.
			glGenTextures(1, &gPanelGL.tex);
			glBindTexture(GL_TEXTURE_2D, gPanelGL.tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, kTextureSrgbDecode, kSkipDecode);
			glBindTexture(GL_TEXTURE_2D, oldTex);
			glGenFramebuffers(1, &gPanelGL.fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gPanelGL.fbo);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPanelGL.tex, 0);
			const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFb);
			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: the observer view's copy of the menu panel is incomplete (0x%x) - no menus on the monitor\n", status);
				gPanelGL.failed = true;
				return false;
			}
			gPanelGL.w = w;
			gPanelGL.h = h;
		}
		return true;
	}

	// The panel's copy over the window, at its own shape, centred - as the
	// flat game lays its 2D over the picture.
	void DrawPanelOver(int cw, int ch)
	{
		if (!gPanelGL.fresh) return;
		gPanelGL.fresh = false;
		if (gPanelGL.w <= 0 || gPanelGL.h <= 0 || cw <= 0 || ch <= 0) return;
		int qw = cw, qh = int(double(cw) * gPanelGL.h / gPanelGL.w + 0.5);
		if (qh > ch) { qh = ch; qw = int(double(ch) * gPanelGL.w / gPanelGL.h + 0.5); }
		static const float kWhole[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
		DrawQuad(gPanelGL.prog, gPanelGL.tex, (cw - qw) / 2, (ch - qh) / 2, qw, qh, kWhole, true);
	}

	// vr_dump_spectator: what the window was given, the eye it was cut from,
	// and the corners, for tools\spectator-check.py.
	void WritePPM(const FString& path, int w, int h, const uint8_t* bottomUpRGB)
	{
		FILE* f = fopen(path.GetChars(), "wb");
		if (!f) { Printf("vr_dump_spectator: cannot write %s\n", path.GetChars()); return; }
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; --y) fwrite(bottomUpRGB + size_t(y) * w * 3, 1, size_t(w) * 3, f);
		fclose(f);
	}
	void DumpSpectator(const char* dir, int cw, int ch, const float corners[8])
	{
		GLint readFb = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		TArray<uint8_t> px;
		GLint drawFb = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
		px.Resize(cw * ch * 3);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFb);
		glReadPixels(0, 0, cw, ch, GL_RGB, GL_UNSIGNED_BYTE, &px[0]);
		WritePPM(FStringf("%s/spectator_window.ppm", dir), cw, ch, &px[0]);
		px.Resize(gSpecGL.w * gSpecGL.h * 3);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gSpecGL.fbo);
		glReadPixels(0, 0, gSpecGL.w, gSpecGL.h, GL_RGB, GL_UNSIGNED_BYTE, &px[0]);
		WritePPM(FStringf("%s/spectator_eye.ppm", dir), gSpecGL.w, gSpecGL.h, &px[0]);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		if (FILE* f = fopen(FStringf("%s/spectator_corners.txt", dir).GetChars(), "w"))
		{
			for (int i = 0; i < 8; ++i) fprintf(f, "%.7f%s", corners[i], i == 7 ? "\n" : " ");
			fclose(f);
		}
		Printf("vr_dump_spectator: window %dx%d, eye %dx%d, into %s\n", cw, ch, gSpecGL.w, gSpecGL.h, dir);
	}
}

// DoomVR: the 2D panel this frame drew, copied while its image is still ours
// (bound for drawing), for PresentHeadset to lay over the desktop window.
void FGLRenderer::KeepPanelForObserver(int w, int h)
{
	if (w <= 0 || h <= 0 || !EnsurePanelCopy(w, h)) return;
	GLint readFb = 0, drawFb = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
	const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gPanelGL.fbo);
	glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFb);
	if (scissor) glEnable(GL_SCISSOR_TEST);
	gPanelGL.fresh = true;
}

// DoomVR: each eye texture into that eye's swapchain image, then the left eye
// to the desktop window - the spectator view's steady cut-out of it, or the
// whole eye - then the frame to the runtime.
void FGLRenderer::PresentHeadset()
{
	// A frame drawn without the engine's BeginFrame - a screen wipe - is still
	// paced by the runtime and still submitted.
	DoomVR_BeginFrame();

	int w, h;
	DoomVR_EyeSize(w, h);
	IntRect eyeBox;
	eyeBox.left = eyeBox.top = 0;
	eyeBox.width = w;
	eyeBox.height = h;

	// The present shader writes finished, gamma-corrected values. The
	// swapchain image is sRGB, and with the encode on they would be corrected
	// a second time and come out pale.
	const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_FRAMEBUFFER_SRGB);

	const bool shot = DoomVR_TakeScreenshotRequest();
	if (DoomVR_ShouldRender())
	{
		for (int eye = 0; eye < 2; ++eye)
		{
			DoomVR_BeginEye(eye);
			mBuffers->BindEyeTexture(eye, 0);
			DrawPresentTexture(eyeBox, true);
			// The picture exactly as the headset gets it - gamma and all -
			// read back only on the frame a screenshot was asked for: a
			// readback stalls the GPU.
			if (shot && eye == 0) HeadsetShot.Capture(w, h);
			DoomVR_EndEye(eye);
		}
	}
	if (shot) AddCommandString("screenshot");

	// The desktop window. The spectator view's cut-out is taken with this
	// frame's View - the pose these eyes were drawn and are submitted with.
	float corners[8];
	const int cw = screen->GetClientWidth(), ch = screen->GetClientHeight();
	if (DoomVR_SpectatorCrop(cw, ch, corners) && EnsureSpectator(w, h))
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gSpecGL.fbo);
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(eyeBox, true);
		mBuffers->BindOutputFB();
		DrawSpectatorQuad(cw, ch, corners);
		DrawPanelOver(cw, ch);
		if (const char* dir = DoomVR_TakeSpectatorDumpRequest()) DumpSpectator(dir, cw, ch, corners);
	}
	else
	{
		mBuffers->BindOutputFB();
		ClearBorders();
		mBuffers->BindEyeTexture(0, 0);
		DrawPresentTexture(screen->mOutputLetterbox, true);
		if (EnsureSpectator(w, h)) DrawPanelOver(cw, ch);
	}
	gPanelGL.fresh = false;

	if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);

	DoomVR_EndFrame();
}

}
