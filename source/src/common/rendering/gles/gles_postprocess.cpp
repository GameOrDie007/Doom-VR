/*
** gles_postprocess.cpp
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "gles_system.h"
#include "m_png.h"
#include "gles_buffers.h"
#include "gles_framebuffer.h"
#include "gles_renderbuffers.h"
#include "gles_renderer.h"
#include "gles_postprocessstate.h"
#include "gles_shaderprogram.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "flatvertices.h"
#include "r_videoscale.h"
#include "v_video.h"

#include "hw_vrmodes.h"
#include "v_draw.h"
#include "doomvr_headset.h"
#include "printf.h"
#include <string.h>
#include <chrono>
#ifdef __ANDROID__
void DoomVR_PerfAdd(int slot, double ms);	// doomvr_headset.cpp
#endif

extern bool vid_hdr_active;


namespace OpenGLESRenderer
{

int gl_dither_bpc = -1;

void FGLRenderer::RenderScreenQuad()
{
	auto buffer = static_cast<GLVertexBuffer *>(screen->mVertexData->GetBufferObjects().first);
	buffer->Bind(nullptr);
	glDrawArrays(GL_TRIANGLE_STRIP, FFlatVertexBuffer::PRESENT_INDEX, 3);
}

void FGLRenderer::PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
#ifndef NO_RENDER_BUFFER
	mBuffers->BindCurrentFB();
#endif
	if (afterBloomDrawEndScene2D) afterBloomDrawEndScene2D();
}



//-----------------------------------------------------------------------------
//
// Copies the rendered screen to its final destination
//
//-----------------------------------------------------------------------------

// DoomVR: whether this GL can switch the sRGB encode off on an sRGB
// framebuffer (GL_EXT_sRGB_write_control; Adreno has it). The engine's
// picture is already in display space, and the headset's images are sRGB:
// encoded again on the way in, everything would come out pale.
static bool SrgbWriteControl()
{
	static int has = -1;
	if (has < 0)
	{
		const char* ext = (const char*)glGetString(GL_EXTENSIONS);
		has = ext && strstr(ext, "GL_EXT_sRGB_write_control") ? 1 : 0;
		if (!has) Printf(PrintFlag(PRINT_HIGH | PRINT_NONOTIFY), "DoomVR: this GL has no GL_EXT_sRGB_write_control - the headset's picture may come out pale\n");
	}
	return has == 1;
}
static constexpr GLenum kFramebufferSrgbExt = 0x8DB9;	// GL_FRAMEBUFFER_SRGB_EXT

void FGLRenderer::PresentHeadsetEye(int eye)
{
	if (!DoomVR_ShouldRender()) return;
	int w, h;
	DoomVR_EyeSize(w, h);
	IntRect eyeBox;
	eyeBox.left = eyeBox.top = 0;
	eyeBox.width = w;
	eyeBox.height = h;

	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);
	const bool srgb = SrgbWriteControl();
	if (srgb) glDisable(kFramebufferSrgbExt);
	DoomVR_BeginEye(eye);	// binds that eye's image as the draw target
	mBuffers->BindCurrentTexture(0);
	DrawPresentTexture(eyeBox, true);
	DoomVR_EndEye(eye);
	if (srgb) glEnable(kFramebufferSrgbExt);
}

void FGLRenderer::Flush()
{
	if (DoomVR_Active())
	{
		// DoomVR: a frame drawn without the engine's BeginFrame (a screen
		// wipe) is still paced by the runtime and still submitted.
		DoomVR_BeginFrame();

		// The second eye: the scene buffer holds it now (the first went at
		// NextEye). Then the 2D - menus, the HUD, the console - on its own
		// flat panel, drawn once and flat, not into each eye.
		PresentHeadsetEye(1);
		int panelW, panelH;
		if (DoomVR_PhotoMode() || DoomVR_GameManualShowing())
		{
			// Photo mode: the world alone. The Game Tome open: the book is in front.
		}
		else
		{
#ifdef __ANDROID__
			const auto panelT0 = std::chrono::steady_clock::now();
#endif
			if (DoomVR_BeginPanel(twod->GetWidth(), twod->GetHeight(), panelW, panelH))
			{
				const bool srgb = SrgbWriteControl();
				if (srgb) glDisable(kFramebufferSrgbExt);
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				::Draw2D(twod, gl_RenderState, 0, 0, panelW, panelH);
				if (srgb) glEnable(kFramebufferSrgbExt);
				DoomVR_EndPanel();
			}
#ifdef __ANDROID__
			::DoomVR_PerfAdd(4, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - panelT0).count());
#endif
		}
		twod->Clear();

		// The window: a standalone headset has no monitor and its window is
		// never swapped while the session runs (gles_framebuffer.cpp), so the
		// eye's picture is not drawn to it either - a full-window draw a frame
		// for nothing. The desk emulator (no runtime, flat) still shows it.
#ifndef __ANDROID__
		CopyToBackbuffer(nullptr, true);
#else
		twod->Clear();
#endif

		DoomVR_EndFrame();
		return;
	}
	CopyToBackbuffer(nullptr, true);
}

//-----------------------------------------------------------------------------
//
// Gamma correct while copying to frame buffer
//
//-----------------------------------------------------------------------------

void FGLRenderer::CopyToBackbuffer(const IntRect *bounds, bool applyGamma)
{
#ifdef NO_RENDER_BUFFER
	mBuffers->BindOutputFB();
#endif
	screen->Draw2D();	// draw all pending 2D stuff before copying the buffer
	twod->Clear();

	FGLPostProcessState savedState;
	savedState.SaveTextureBindings(2);

	mBuffers->BindOutputFB();

	IntRect box;
	if (bounds)
	{
		box = *bounds;
	}
	else
	{
		ClearBorders();
		box = screen->mOutputLetterbox;
	}

	mBuffers->BindCurrentTexture(0);
#ifndef NO_RENDER_BUFFER
	DrawPresentTexture(box, applyGamma);
#endif
}

void FGLRenderer::DrawPresentTexture(const IntRect &box, bool applyGamma)
{
	glViewport(box.left, box.top, box.width, box.height);

	mBuffers->BindDitherTexture(1);

	glActiveTexture(GL_TEXTURE0);
	if (ViewportLinearScale())
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	else
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	mPresentShader->Bind();
	if (!applyGamma || framebuffer->IsHWGammaActive())
	{
		mPresentShader->Uniforms->InvGamma = 1.0f;
		mPresentShader->Uniforms->Contrast = 1.0f;
		mPresentShader->Uniforms->Saturation = 1.0f;
		mPresentShader->Uniforms->BlackPoint = 0.0f;
		mPresentShader->Uniforms->WhitePoint = 1.0f;
	}
	else
	{
		mPresentShader->Uniforms->InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		mPresentShader->Uniforms->Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		mPresentShader->Uniforms->Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		mPresentShader->Uniforms->BlackPoint = clamp<float>(vid_i_blackpoint, 0.f, 1.f);
		mPresentShader->Uniforms->WhitePoint = clamp<float>(vid_i_whitepoint, 0.f, 5.f);
		mPresentShader->Uniforms->GrayFormula = static_cast<int>(gl_satformula);
	}
	if (vid_hdr_active && framebuffer->IsFullscreen())
	{
		// Full screen exclusive mode treats a rgba16f frame buffer as linear.
		// It probably will eventually in desktop mode too, but the DWM doesn't seem to support that.
		mPresentShader->Uniforms->HdrMode = 1;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 1023.0f : (float)((1 << gl_dither_bpc) - 1);
	}
	else
	{
		mPresentShader->Uniforms->HdrMode = 0;
		mPresentShader->Uniforms->ColorScale = (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	}

	mPresentShader->Uniforms->Scale = { screen->mScreenViewport.width / (float)mBuffers->GetWidth(), screen->mScreenViewport.height / (float)mBuffers->GetHeight() };
	mPresentShader->Uniforms->Offset = { 0.0f, 0.0f };
	mPresentShader->Uniforms.SetData();

	for (size_t n = 0; n < mPresentShader->Uniforms.mFields.size(); n++)
	{
		UniformFieldDesc desc = mPresentShader->Uniforms.mFields[n];
		int loc = mPresentShader->Uniforms.UniformLocation[n];
		switch (desc.Type)
		{
		case UniformType::Int:
			glUniform1i(loc, *((GLint*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		case UniformType::Float:
			glUniform1f(loc, *((GLfloat*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		case UniformType::Vec2:
			glUniform2fv(loc,1 , ((GLfloat*)(((char*)(&mPresentShader->Uniforms)) + desc.Offset)));
			break;
		default:
			break;
		}
	}

	RenderScreenQuad();
}

//-----------------------------------------------------------------------------
//
// Fills the black bars around the screen letterbox
//
//-----------------------------------------------------------------------------

void FGLRenderer::ClearBorders()
{
	const auto &box = screen->mOutputLetterbox;

	int clientWidth = framebuffer->GetClientWidth();
	int clientHeight = framebuffer->GetClientHeight();
	if (clientWidth == 0 || clientHeight == 0)
		return;

	glViewport(0, 0, clientWidth, clientHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_SCISSOR_TEST);
	if (box.top > 0)
	{
		glScissor(0, 0, clientWidth, box.top);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientHeight - box.top - box.height > 0)
	{
		glScissor(0, box.top + box.height, clientWidth, clientHeight - box.top - box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (box.left > 0)
	{
		glScissor(0, box.top, box.left, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	if (clientWidth - box.left - box.width > 0)
	{
		glScissor(box.left + box.width, box.top, clientWidth - box.left - box.width, box.height);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
}

}
