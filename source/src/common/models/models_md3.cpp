/*
** models_md3.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2006-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "filesystem.h"
#include "cmdlib.h"
#include "model_md3.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "m_swap.h"
#include <map>
#include <vector>
#include <algorithm>

#define MAX_QPATH 64

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//===========================================================================
//
// decode the lat/lng normal to a 3 float normal
//
//===========================================================================

static void UnpackVector(unsigned short packed, float & nx, float & ny, float & nz)
{
	double lat = ( packed >> 8 ) & 0xff;
	double lng = ( packed & 0xff );
	lat *= M_PI/128;
	lng *= M_PI/128;

	nx = cos(lat) * sin(lng);
	ny = sin(lat) * sin(lng);
	nz = cos(lng);
}

//===========================================================================
//
// MD3 File structure
//
//===========================================================================

#pragma pack(4)
struct md3_header_t
{
	uint32_t Magic;
	uint32_t Version;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Tags;
	uint32_t Num_Surfaces;
	uint32_t Num_Skins;
	uint32_t Ofs_Frames;
	uint32_t Ofs_Tags;
	uint32_t Ofs_Surfaces;
	uint32_t Ofs_Eof;
};

struct md3_surface_t
{
	uint32_t Magic;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Shaders;
	uint32_t Num_Verts;
	uint32_t Num_Triangles;
	uint32_t Ofs_Triangles;
	uint32_t Ofs_Shaders;
	uint32_t Ofs_Texcoord;
	uint32_t Ofs_XYZNormal;
	uint32_t Ofs_End;
};

struct md3_triangle_t
{
	uint32_t vt_index[3];
};

struct md3_shader_t
{
	char Name[MAX_QPATH];
	uint32_t index;
};

struct md3_texcoord_t
{
	float s, t;
};

struct md3_vertex_t
{
	short x, y, z, n;
};

struct md3_frame_t
{
	float min_Bounds[3];
	float max_Bounds[3];
	float localorigin[3];
	float radius;
	char Name[16];
};
#pragma pack()


//===========================================================================
//
//
//
//===========================================================================

bool FMD3Model::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	md3_header_t * hdr = (md3_header_t *)buffer;

	auto numFrames = LittleLong(hdr->Num_Frames);
	auto numSurfaces = LittleLong(hdr->Num_Surfaces);
	hasSurfaces = numSurfaces > 1;

	numTags = LittleLong(hdr->Num_Tags);

	md3_frame_t * frm = (md3_frame_t*)(buffer + LittleLong(hdr->Ofs_Frames));

	Frames.Resize(numFrames);
	for (unsigned i = 0; i < numFrames; i++)
	{
		strncpy(Frames[i].Name, frm[i].Name, 15);
		for (int j = 0; j < 3; j++) Frames[i].origin[j] = frm[i].localorigin[j];
	}

	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	Surfaces.Resize(numSurfaces);

	for (unsigned i = 0; i < numSurfaces; i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		s->numSkins = LittleLong(ss->Num_Shaders);
		s->numTriangles = LittleLong(ss->Num_Triangles);
		s->numVertices = LittleLong(ss->Num_Verts);

		// copy shaders (skins)
		md3_shader_t * shader = (md3_shader_t*)(((char*)ss) + LittleLong(ss->Ofs_Shaders));
		s->Skins.Resize(s->numSkins);

		for (unsigned ii = 0; ii < s->numSkins; ii++)
		{
			// [BB] According to the MD3 spec, Name is supposed to include the full path.
			// ... and since some tools seem to output backslashes, these need to be replaced with forward slashes to work.
			FixPathSeperator(shader[ii].Name);
			s->Skins[ii] = LoadSkin("", shader[ii].Name);
			// [BB] Fall back and check if Name is relative.
			if (!s->Skins[ii].isValid())
				s->Skins[ii] = LoadSkin(path, shader[ii].Name);
		}
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::LoadGeometry()
{
	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	auto buffer = lumpdata.string();
	md3_header_t * hdr = (md3_header_t *)buffer;
	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		// copy triangle indices
		md3_triangle_t * tris = (md3_triangle_t*)(((char*)ss) + LittleLong(ss->Ofs_Triangles));
		s->Tris.Resize(s->numTriangles);

		for (unsigned ii = 0; ii < s->numTriangles; ii++) for (int j = 0; j < 3; j++)
		{
			s->Tris[ii].VertIndex[j] = LittleLong(tris[ii].vt_index[j]);
		}

		// Load texture coordinates
		md3_texcoord_t * tc = (md3_texcoord_t*)(((char*)ss) + LittleLong(ss->Ofs_Texcoord));
		s->Texcoords.Resize(s->numVertices);

		for (unsigned ii = 0; ii < s->numVertices; ii++)
		{
			s->Texcoords[ii].s = tc[ii].s;
			s->Texcoords[ii].t = tc[ii].t;
		}

		// Load vertices and texture coordinates
		md3_vertex_t * vt = (md3_vertex_t*)(((char*)ss) + LittleLong(ss->Ofs_XYZNormal));
		s->Vertices.Resize(s->numVertices * Frames.Size());

		for (unsigned ii = 0; ii < s->numVertices * Frames.Size(); ii++)
		{
			s->Vertices[ii].x = LittleShort(vt[ii].x) / 64.f;
			s->Vertices[ii].y = LittleShort(vt[ii].y) / 64.f;
			s->Vertices[ii].z = LittleShort(vt[ii].z) / 64.f;
			UnpackVector(LittleShort(vt[ii].n), s->Vertices[ii].nx, s->Vertices[ii].ny, s->Vertices[ii].nz);
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

// DoomVR: the first frame's extent, in the axes the vertex buffer uses (x, z, y).
bool FMD3Model::MeasureBounds(FVector3& lo, FVector3& hi)
{
	LoadGeometry();
	bool any = false;
	lo = FVector3(1e9f, 1e9f, 1e9f);
	hi = FVector3(-1e9f, -1e9f, -1e9f);
	for (auto& s : Surfaces)
	{
		for (unsigned j = 0; j < s.numVertices && j < s.Vertices.Size(); j++)
		{
			const FVector3 p(s.Vertices[j].x, s.Vertices[j].z, s.Vertices[j].y);
			lo = FVector3(min(lo.X, p.X), min(lo.Y, p.Y), min(lo.Z, p.Z));
			hi = FVector3(max(hi.X, p.X), max(hi.Y, p.Y), max(hi.Z, p.Z));
			any = true;
		}
		s.UnloadGeometry();
	}
	return any;
}

// DoomVR: the first frame's points, in the same axes.
bool FMD3Model::MeasurePoints(TArray<FVector3>& out)
{
	LoadGeometry();
	out.Clear();
	for (auto& s : Surfaces)
	{
		for (unsigned j = 0; j < s.numVertices && j < s.Vertices.Size(); j++)
			out.Push(FVector3(s.Vertices[j].x, s.Vertices[j].z, s.Vertices[j].y));
		s.UnloadGeometry();
	}
	return out.Size() > 0;
}

// DoomVR: the parts of a surface that appear and vanish between frames (see
// MD3Surface::doomvrPopTris). Only our own gun models (doomvr-b/): another
// model's part growing from a point (a muzzle flash) may mean to.
void FMD3Model::DoomVR_FindPoppingParts(MD3Surface& surf)
{
	const unsigned nf = Frames.Size(), nv = surf.numVertices;
	if (nf < 2 || mFileName.IndexOf("doomvr-b/") < 0 || surf.Vertices.Size() < nf * nv) return;
	// The parts: vertices joined by triangles.
	std::vector<unsigned> parent(nv);
	for (unsigned i = 0; i < nv; i++) parent[i] = i;
	auto root = [&](unsigned a) { while (parent[a] != a) a = parent[a] = parent[parent[a]]; return a; };
	for (auto& t : surf.Tris)
	{
		if (unsigned(t.VertIndex[0]) >= nv || unsigned(t.VertIndex[1]) >= nv || unsigned(t.VertIndex[2]) >= nv) return;
		parent[root(t.VertIndex[0])] = root(t.VertIndex[1]);
		parent[root(t.VertIndex[1])] = root(t.VertIndex[2]);
	}
	// Each part's size in each frame: folded (0) or not (1).
	std::map<unsigned, std::vector<float>> lo, hi;
	for (unsigned v = 0; v < nv; v++)
	{
		const unsigned r = root(v);
		auto& l = lo[r]; auto& h = hi[r];
		if (l.empty()) { l.assign(nf * 3, 1e30f); h.assign(nf * 3, -1e30f); }
		for (unsigned f = 0; f < nf; f++)
		{
			const MD3Vertex& p = surf.Vertices[f * nv + v];
			const float c[3] = { p.x, p.y, p.z };
			for (int k = 0; k < 3; k++) { l[f * 3 + k] = std::min(l[f * 3 + k], c[k]); h[f * 3 + k] = std::max(h[f * 3 + k], c[k]); }
		}
	}
	std::map<unsigned, std::vector<uint8_t>> shown;
	for (auto& [r, l] : lo)
	{
		auto& h = hi[r];
		std::vector<uint8_t> s(nf);
		bool some = false, none = false;
		for (unsigned f = 0; f < nf; f++)
		{
			float ext = 0;
			for (int k = 0; k < 3; k++) ext = std::max(ext, h[f * 3 + k] - l[f * 3 + k]);
			s[f] = ext > 1e-3f;
			(s[f] ? some : none) = true;
		}
		if (some && none) shown[r] = s;
	}
	if (shown.empty()) return;
	surf.doomvrSnap.Resize(nf * nf);
	for (unsigned a = 0; a < nf; a++)
		for (unsigned b = 0; b < nf; b++)
		{
			uint8_t snap = 0;
			for (auto& [r, s] : shown) if (s[a] != s[b]) { snap = 1; break; }
			surf.doomvrSnap[a * nf + b] = snap;
		}
	// Their triangles last.
	TArray<MD3Triangle> keep, pop;
	for (auto& t : surf.Tris) (shown.count(root(t.VertIndex[0])) ? pop : keep).Push(t);
	surf.doomvrPopTris = pop.Size();
	surf.Tris = std::move(keep);
	for (auto& t : pop) surf.Tris.Push(t);
}

void FMD3Model::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		LoadGeometry();

		unsigned int vbufsize = 0;
		unsigned int ibufsize = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			vbufsize += Frames.Size() * surf->numVertices;
			ibufsize += 3 * surf->numTriangles;
		}

		auto vbuf = renderer->CreateVertexBuffer(true, Frames.Size() == 1);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize);
		unsigned int *indxptr = vbuf->LockIndexBuffer(ibufsize);

		assert(vertptr != nullptr && indxptr != nullptr);

		unsigned int vindex = 0, iindex = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];

			surf->vindex = vindex;
			surf->iindex = iindex;
			DoomVR_FindPoppingParts(*surf);
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];

				FModelVertex *bvert = &vertptr[vindex++];

				int tc = j % surf->numVertices;
				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);
				bvert->SetNormal(vert->nx, vert->nz, vert->ny);
			}

			for (unsigned k = 0; k < surf->numTriangles; k++)
			{
				for (int l = 0; l < 3; l++)
				{
					indxptr[iindex++] = surf->Tris[k].VertIndex[l];
				}
			}
			surf->UnloadGeometry();
		}
		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
	}
}


//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FMD3Model::AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids)
{
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		if (surfaceskinids && surfaceskinids[i].isValid())
		{
			hitlist[surfaceskinids[i].GetIndex()] |= FTextureManager::HIT_Flat;
		}

		MD3Surface * surf = &Surfaces[i];
		for (unsigned j = 0; j < surf->numSkins; j++)
		{
			if (surf->Skins[j].isValid())
			{
				hitlist[surf->Skins[j].GetIndex()] |= FTextureManager::HIT_Flat;
			}
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

int FMD3Model::FindFrame(const char* name, bool nodefault)
{
	for (unsigned i = 0; i < Frames.Size(); i++)
	{
		if (!stricmp(name, Frames[i].Name)) return i;
	}
	return FErr_NotFound;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frameno, int frameno2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition)
{
	if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;

	renderer->SetInterpolation(inter);
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * surf = &Surfaces[i];

		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
		// Note: Each surface may have a different skin.
		FGameTexture *surfaceSkin = skin;
		if (!surfaceSkin)
		{
			if (surfaceskinids && surfaceskinids[i].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surfaceskinids[i], true);
			}
			else if (surf->numSkins > 0 && surf->Skins[0].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surf->Skins[0], true);
			}

			if (!surfaceSkin)
			{
				continue;
			}
		}

		renderer->SetMaterial(surfaceSkin, false, translation);
		renderer->SetupFrame(this, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices, -1);
		// DoomVR: a part that appears or vanishes between these two frames is
		// drawn as the current frame has it, not shrunk or grown on the way.
		const unsigned nf = Frames.Size();
		if (surf->doomvrPopTris > 0 && inter != 0 && surf->doomvrSnap.Size() == nf * nf && surf->doomvrSnap[frameno * nf + frameno2])
		{
			const unsigned rest = surf->numTriangles - surf->doomvrPopTris;
			if (rest > 0) renderer->DrawElements(rest * 3, surf->iindex * sizeof(unsigned int));
			renderer->SetInterpolation(0.f);
			renderer->DrawElements(surf->doomvrPopTris * 3, (surf->iindex + rest * 3) * sizeof(unsigned int));
			renderer->SetInterpolation(inter);
			continue;
		}
		renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
	}
	renderer->SetInterpolation(0.f);
}
