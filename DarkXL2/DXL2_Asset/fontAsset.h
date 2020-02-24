#pragma once
//////////////////////////////////////////////////////////////////////
// DarkXL 2 Level/Map
// Handles the following:
// 1. Parsing the level data (.LEV)
// 2. Runtime storage and manipulation of level data
//    (vertices, lines, sectors)
//////////////////////////////////////////////////////////////////////
#include <DXL2_System/types.h>

struct Font
{
	u8 startChar;
	u8 endChar;
	u8 charCount;
	u8 height;

	u8 maxWidth;
	u8 pad[3];

	u8 width[256];
	u32 imageOffset[256];

	u8* imageData;
};

namespace DXL2_Font
{
	Font* get(const char* name);
	void freeAll();
}
