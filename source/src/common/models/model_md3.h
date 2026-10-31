/*
** model_md3.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once
#include "model.h"

#define MD3_MAGIC			0x33504449

class FMD3Model : public FModel
{
	struct MD3Tag
	{
		// Currently I have no use for this
	};

	struct MD3TexCoord
	{
		float s,t;
	};

	struct MD3Vertex
	{
		float x,y,z;
		float nx,ny,nz;
	};

	struct MD3Triangle
	{
		int VertIndex[3];
	};

	struct MD3Surface
	{
		unsigned numVertices;
		unsigned numTriangles;
		unsigned numSkins;

		TArray<FTextureID> Skins;
		TArray<MD3Triangle> Tris;
		TArray<MD3TexCoord> Texcoords;
		TArray<MD3Vertex> Vertices;

		unsigned int vindex = UINT_MAX;	// contains numframes arrays of vertices
		unsigned int iindex = UINT_MAX;

		// DoomVR: parts that are there in some frames and folded to a point in
		// others (our guns' ejected shells). Their triangles come last in the
		// index buffer; between two frames where one of them appears or
		// vanishes they are drawn without interpolation, so a shell does not
		// shrink to nothing on its way out. doomvrSnap[a * frames + b].
		unsigned int doomvrPopTris = 0;
		TArray<uint8_t> doomvrSnap;

		void UnloadGeometry()
		{
			Tris.Reset();
			Vertices.Reset();
			Texcoords.Reset();
		}
	};

	struct MD3Frame
	{
		// The bounding box information is of no use in the Doom engine
		// That will still be done with the actor's size information.
		char Name[16];
		float origin[3];
	};

	int numTags;
	int mLumpNum;

	TArray<MD3Frame> Frames;
	TArray<MD3Surface> Surfaces;

public:
	FMD3Model() = default;

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) override;
	virtual int FindFrame(const char* name, bool nodefault) override;
	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition) override;
	void LoadGeometry();
	void DoomVR_FindPoppingParts(MD3Surface& surf);
	void BuildVertexBuffer(FModelRenderer *renderer);
	bool MeasureBounds(FVector3& lo, FVector3& hi) override;
	bool MeasurePoints(TArray<FVector3>& out) override;
	int DoomVR_FrameCount() override { return int(Frames.Size()); }
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) override;
};
