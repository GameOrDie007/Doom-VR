/*
** gles_hwtexture.cpp
**
** GL texture abstraction
**
**---------------------------------------------------------------------------
**
** Copyright 2019 Christoph Oelckers
** Copyright 2019-2025 GZDoom Maintainers and Contributors
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

#include "gles_system.h"
#include "printf.h"
#include <string.h>

#include "c_cvars.h"
#include "hw_material.h"

#include "hw_cvars.h"
#include "gles_renderer.h"
#include "gles_samplers.h"
#include "gles_renderstate.h"
#include "gles_hwtexture.h"

namespace OpenGLESRenderer
{


TexFilter_s TexFilter[] = {
	{GL_NEAREST,					GL_NEAREST,		false},
	{GL_NEAREST_MIPMAP_NEAREST,		GL_NEAREST,		true},
	{GL_LINEAR,						GL_LINEAR,		false},
	{GL_LINEAR_MIPMAP_NEAREST,		GL_LINEAR,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_LINEAR,		true},
	{GL_NEAREST_MIPMAP_LINEAR,		GL_NEAREST,		true},
	{GL_LINEAR_MIPMAP_LINEAR,		GL_NEAREST,		true},
};

//===========================================================================
//
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];

//===========================================================================
//
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================

unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	int rh,rw;
	int texformat = GL_RGBA;// TexFormat[gl_texture_format];
	bool deletebuffer=false;

	// When running in SW mode buffer will be null, so set it to the texBuffer already created
	// There could be other use cases I do not know about which means this is a bad idea..
	if (buffer == nullptr)
		buffer = texBuffer;

	bool firstCall = glTexID == 0;
	if (firstCall)
	{
		glGenTextures(1, &glTexID);
	}

	int textureBinding = UINT_MAX;
	if (texunit == -1)	glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
	if (texunit > 0) glActiveTexture(GL_TEXTURE0+texunit);
	if (texunit >= 0) lastbound[texunit] = glTexID;
	glBindTexture(GL_TEXTURE_2D, glTexID);


	rw = GetTexDimension(w);
	rh = GetTexDimension(h);

	if (!buffer)
	{
		// The texture must at least be initialized if no data is present.
		mipmapped = false;
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		//texheight=-h;
	}
	else
	{
		if (rw < w || rh < h)
		{
			// The texture is larger than what the hardware can handle so scale it down.
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				Resize(w, h, rw, rh, buffer, scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}


	}
	// store the physical size.

	int sourcetype;

	// DoomVR: on OpenGL ES the engine's BGRA pixels used to go up as a BGRA
	// texture (GL_EXT_texture_format_BGRA8888). OpenGL ES 3 refuses to build
	// mipmaps for that format (glGenerateMipmap: GL_INVALID_OPERATION), so
	// every mipmapped texture - every wall and floor - was incomplete and
	// sampled black; only the unmipmapped sprites showed. The bytes now go up
	// as RGBA and the texture's swizzle swaps red and blue back, which ES 3.0
	// has, costs nothing, and mipmaps fine.
	bool swizzleBGRA = false;

	if (gles.glesMode == GLES_MODE_GLES)
	{
		if (glTextureBytes == 1)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_ALPHA;
			texformat = GL_ALPHA;
		}
		else
		{
			sourcetype = GL_RGBA;
			texformat = GL_RGBA;
			swizzleBGRA = true;
		}
	}
	else
	{
		if (glTextureBytes == 1) //Use Red channel instread becuase Alpha does not work in OpenGL, swizzle later
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			sourcetype = GL_RED;
			texformat = GL_RED;
		}
		else
		{
			sourcetype = GL_BGRA;
			texformat = GL_RGBA;
		}
	}

	while (glGetError() != GL_NO_ERROR) {}	// DoomVR: a clean slate, so the errors below are this upload's
	glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, sourcetype, GL_UNSIGNED_BYTE, buffer);
	{
		// DoomVR: a texture that failed to upload samples black with no word from GL; say so, once per kind.
		const GLenum err = glGetError();
		static int said = 0;
		if (err != GL_NO_ERROR && said < 8) { said++; Printf("GLES: glTexImage2D(%dx%d, format 0x%x/0x%x, %d bytes) failed: GL error 0x%x (%s)\n", rw, rh, texformat, sourcetype, glTextureBytes, err, name ? name : ""); }
	}
	if (swizzleBGRA)
	{
		glTexParameteri(GL_TEXTURE_2D, 0x8E42 /*GL_TEXTURE_SWIZZLE_R*/, GL_BLUE);
		glTexParameteri(GL_TEXTURE_2D, 0x8E44 /*GL_TEXTURE_SWIZZLE_B*/, GL_RED);
	}

	if (gles.glesMode != GLES_MODE_GLES)
	{
		// The shader is using the alpha channel instead of red, this work on GLES but not on GL
		// So the texture uses GL_RED and this swizzels the red channel into the alpha channel
		if (glTextureBytes == 1)
		{
			GLint swizzleMask[] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
			glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);
		}
	}

	if (deletebuffer && buffer) free(buffer);

	if (mipmap && TexFilter[gl_texture_filter].mipmapping)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
		{
			const GLenum err = glGetError();
			static int said = 0;
			if (err != GL_NO_ERROR && said < 8) { said++; Printf("GLES: glGenerateMipmap(%dx%d, format 0x%x, %d bytes) failed: GL error 0x%x (%s)\n", rw, rh, texformat, glTextureBytes, err, name ? name : ""); }
		}
		mipmapped = true;
	}

	if (texunit > 0) glActiveTexture(GL_TEXTURE0);
	else if (texunit == -1) glBindTexture(GL_TEXTURE_2D, textureBinding);
	return glTexID;
}


void FHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (texelsize < 1 || texelsize > 4) texelsize = 4;
	glTextureBytes = texelsize;
	bufferpitch = w;

	if (texBuffer)
		delete[] texBuffer;

	texBuffer = new uint8_t[(w * h) * texelsize];
	return;
}

uint8_t* FHardwareTexture::MapBuffer()
{
	return texBuffer;
}

//===========================================================================
//
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture()
{
	if (glTexID != 0) glDeleteTextures(1, &glTexID);

	if (texBuffer)
		delete[] texBuffer;
}


//===========================================================================
//
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, bool needmipmap)
{
	if (glTexID != 0)
	{
		if (lastbound[texunit] == glTexID) return glTexID;
		lastbound[texunit] = glTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
		glBindTexture(GL_TEXTURE_2D, glTexID);
		// Check if we need mipmaps on a texture that was creted without them.
		if (needmipmap && !mipmapped && TexFilter[gl_texture_filter].mipmapping)
		{
			glGenerateMipmap(GL_TEXTURE_2D);
			mipmapped = true;
		}
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return glTexID;
	}
	return 0;
}

void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}

//===========================================================================
//
//	Creates a depth buffer for this texture
//
//===========================================================================

int FHardwareTexture::GetDepthBuffer(int width, int height)
{
	if (glDepthID == 0)
	{
		glGenRenderbuffers(1, &glDepthID);
		glBindRenderbuffer(GL_RENDERBUFFER, glDepthID);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
			GetTexDimension(width), GetTexDimension(height));
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	return glDepthID;
}


//===========================================================================
//
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer(int width, int height)
{
	width = GetTexDimension(width);
	height = GetTexDimension(height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer(width, height));
}


//===========================================================================
//
//	Binds a texture to the renderer
//
//===========================================================================

bool FHardwareTexture::BindOrCreate(FTexture *tex, int texunit, int clampmode, int translation, int flags)
{
	bool needmipmap = (clampmode <= CLAMP_XY) && !forcenofilter;

	// Bind it to the system.
	if (!Bind(texunit, needmipmap))
	{
		if (flags & CTF_Indexed)
		{
			glTextureBytes = 1;
			forcenofilter = true;
			needmipmap = false;
		}
		int w = 0, h = 0;

		// Create this texture

		FTextureBuffer texbuffer;

		if (!tex->isHardwareCanvas())
		{
			texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
			w = texbuffer.mWidth;
			h = texbuffer.mHeight;
		}
		else
		{
			w = tex->GetWidth();
			h = tex->GetHeight();
		}
		if (!CreateTexture(texbuffer.mBuffer, w, h, texunit, needmipmap, "FHardwareTexture.BindOrCreate"))
		{
			// could not create texture
			return false;
		}
	}
	if (forcenofilter && clampmode <= CLAMP_XY) clampmode += CLAMP_NOFILTER - CLAMP_NONE;
	GLRenderer->mSamplerManager->Bind(texunit, clampmode, 255);
	return true;
}

}
