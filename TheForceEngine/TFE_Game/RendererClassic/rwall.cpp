#include "rwall.h"
#include "rflat.h"
#include "rlighting.h"
#include "rsector.h"
#include "fixedPoint.h"
#include "rmath.h"
#include "rcommon.h"
#include <TFE_Renderer/renderer.h>
#include <TFE_System/system.h>
#include <TFE_System/math.h>
#include <TFE_FileSystem/paths.h>
#include <TFE_Asset/levelAsset.h>
#include <TFE_Asset/levelObjectsAsset.h>
#include <TFE_Asset/colormapAsset.h>
#include <TFE_Game/level.h>
#include <TFE_Asset/spriteAsset.h>
#include <TFE_Asset/textureAsset.h>
#include <TFE_Asset/paletteAsset.h>
#include <TFE_Asset/colormapAsset.h>
#include <TFE_Asset/modelAsset.h>
#include <TFE_LogicSystem/logicSystem.h>
#include <TFE_FrontEndUI/console.h>
#include <algorithm>
#include <assert.h>

using namespace RendererClassic;
using namespace FixedPoint;
using namespace RMath;
using namespace RClassicFlat;
using namespace RClassicLighting;

namespace RClassicWall
{
	enum SegSide
	{
		FRONT = 0xffff,
		BACK = 0,
	};

	static s32 s_segmentCross;
	static s32 s_texHeightMask;
	static s32 s_yPixelCount;
	static s32 s_vCoordStep;
	static s32 s_vCoordFixed;
	static const u8* s_columnLight;
	static u8* s_texImage;
	static u8* s_columnOut;

	s32 segmentCrossesLine(s32 ax0, s32 ay0, s32 ax1, s32 ay1, s32 bx0, s32 by0, s32 bx1, s32 by1);
	s32 solveForZ_Numerator(RWallSegment* wallSegment);
	s32 solveForZ(RWallSegment* wallSegment, s32 x, s32 numerator, s32* outViewDx=nullptr);
	void drawColumn_Fullbright(s32 y0, s32 y1);
	void drawColumn_Lit(s32 y0, s32 y1);

	// Process the wall and produce an RWallSegment for rendering if the wall is potentially visible.
	void wall_process(RWall* wall)
	{
		const vec2* p0 = wall->v0;
		const vec2* p1 = wall->v1;

		// viewspace wall coordinates.
		s32 x0 = p0->x;
		s32 x1 = p1->x;
		s32 z0 = p0->z;
		s32 z1 = p1->z;

		// x values of frustum lines that pass through (x0,z0) and (x1,z1)
		s32 left0 = -z0;
		s32 left1 = -z1;
		s32 right0 = z0;
		s32 right1 = z1;

		// Cull the wall if it is completely beyind the camera.
		if (z0 < 0 && z1 < 0)
		{
			wall->visible = 0;
			return;
		}
		// Cull the wall if it is completely outside the view
		if ((x0 < left0 && x1 < left1) || (x0 > right0 && x1 > right1))
		{
			wall->visible = 0;
			return;
		}

		s32 dx = x1 - x0;
		s32 dz = z1 - z0;
		// Cull the wall if it is back facing.
		// y0*dx - x0*dy
		const s32 side = mul16(z0, dx) - mul16(x0, dz);
		if (side < 0)
		{
			wall->visible = 0;
			return;
		}

		s32 curU = 0;
		s32 clipLeft = 0;
		s32 clipRight = 0;
		s32 clipX1_Near = 0;
		s32 clipX0_Near = 0;

		s32 texelLen = wall->texelLength;
		s32 texelLenRem = texelLen;

		//////////////////////////////////////////////
		// Clip the Wall Segment by the left and right
		// frustum lines.
		//////////////////////////////////////////////

		// The wall segment extends past the left clip line.
		if (x0 < left0)
		{
			// Intersect the segment (x0, z0),(x1, z1) with the frustum line that passes through (-z0, z0) and (-z1, z1)
			s32 xz = mul16(x0, z1) - mul16(z0, x1);
			s32 dyx = -dz - dx;
			if (dyx != 0)
			{
				xz = div16(xz, dyx);
			}

			// Compute the parametric intersection of the segment and the left frustum line
			// where s is in the range of [0.0, 1.0]
			s32 s = 0;
			if (dz != 0 && RMath::abs(dz) > RMath::abs(dx))
			{
				s = div16(xz - z0, dz);
			}
			else if (dx != 0)
			{
				s = div16(-xz - x0, dx);
			}

			// Update the x0,y0 coordinate of the segment.
			x0 = -xz;
			z0 =  xz;

			if (s != 0)
			{
				// length of the clipped portion of the remaining texel length.
				s32 clipLen = mul16(texelLenRem, s);
				// adjust the U texel offset.
				curU += clipLen;
				// adjust the remaining texel length.
				texelLenRem = texelLen - curU;
			}

			// We have clipped the segment on the left side.
			clipLeft = -1;
			// Update dX and dZ
			dx = x1 - x0;
			dz = z1 - z0;
		}
		// The wall segment extends past the right clip line.
		if (x1 > right1)
		{
			// Compute the coordinate where x0 + s*dx = z0 + s*dz
			// Solve for s = (x0 - y0)/(dz - dx)
			// Substitute: x = x0 + ((x0 - z0)/(dz - dx))*dx = (x0*z1 - z0*x1) / (dz - dx)
			s32 xz = mul16(x0, z1) - mul16(z0, x1);
			s32 dyx = dz - dx;
			if (dyx != 0)
			{
				xz = div16(xz, dyx);
			}

			// Compute the parametric intersection of the segment and the left frustum line
			// where s is in the range of [0.0, 1.0]
			// Note we are computing from the right side, i.e. distance from (x1,y1).
			s32 s = 0;
			if (dz != 0 && RMath::abs(dz) > RMath::abs(dx))
			{
				s = div16(xz - z1, dz);
			}
			else if (dx != 0)
			{
				s = div16(xz - x1, dx);
			}

			// Update the x1,y1 coordinate of the segment.
			x1 = xz;
			z1 = xz;
			if (s != 0)
			{
				s32 adjLen = texelLen + mul16(texelLenRem, s);
				s32 adjLenMinU = adjLen - curU;

				texelLen = adjLen;
				texelLenRem = adjLenMinU;
			}

			// We have clipped the segment on the right side.
			clipRight = -1;
			// Update dX and dZ
			dx = x1 - x0;
			dz = z1 - z0;
		}

		//////////////////////////////////////////////////
		// Clip the Wall Segment by the near plane.
		//////////////////////////////////////////////////
		if ((z0 < 0 || z1 < 0) && segmentCrossesLine(0, 0, 0, -s_halfHeight, x0, x0, x1, z1) != 0)
		{
			wall->visible = 0;
			return;
		}
		if (z0 < ONE_16 && z1 < ONE_16)
		{
			if (clipLeft != 0)
			{
				clipX0_Near = -1;
				x0 = -ONE_16;
			}
			else
			{
				x0 = div16(x0, z0);
			}
			if (clipRight != 0)
			{
				x1 = ONE_16;
				clipX1_Near = -1;
			}
			else
			{
				x1 = div16(x1, z1);
			}
			dx = x1 - x0;
			dz = 0;
			z0 = ONE_16;
			z1 = ONE_16;
		}
		else if (z0 < ONE_16)
		{
			if (clipLeft != 0)
			{
				if (dz != 0)
				{
					s32 left = div16(z0, dz);
					x0 += mul16(dx, left);
					//curU += mul16(texelLenRem, left);

					dx = x1 - x0;
					texelLenRem = texelLen - curU;
				}
				z0 = ONE_16;
				clipX0_Near = -1;
				dz = z1 - ONE_16;
			}
			else
			{
				// BUG: This is NOT correct but matches the original implementation.
				// I am leaving it as-is to match the original DOS renderer.
				// Fortunately hitting this case is VERY rare in practice.
				x0 = div16(x0, z0);
				z0 = ONE_16;
				dz = z1 - ONE_16;
				dx -= x0;
			}
		}
		else if (z1 < ONE_16)
		{
			if (clipRight != 0)
			{
				if (dz != 0)
				{
					s32 s = div16(ONE_16 - x1, dz);
					x1 += mul16(dx, s);
					texelLen += mul16(texelLenRem, s);
					texelLenRem = texelLen - curU;
					dx = x1 - x0;
				}
				z1 = ONE_16;
				dz = ONE_16 - z0;
				clipX1_Near = -1;
			}
			else
			{
				// BUG: This is NOT correct but matches the original implementation.
				// I am leaving it as-is to match the original DOS renderer.
				// Fortunately hitting this case is VERY rare in practice.
				x1 = div16(x1, z1);
				z1 = ONE_16;
				dx = x1 - x0;
				dz = z1 - z0;
			}
		}
		
		//////////////////////////////////////////////////
		// Project.
		//////////////////////////////////////////////////
		s32 x0proj  = div16(mul16(x0, s_focalLength), z0) + s_halfWidth;
		s32 x1proj  = div16(mul16(x1, s_focalLength), z1) + s_halfWidth;
		s32 x0pixel = round16(x0proj);
		s32 x1pixel = round16(x1proj) - 1;

		// Handle near plane clipping by adjusting the walls to avoid holes.
		if (clipX0_Near != 0 && x0pixel > s_minScreenX)
		{
			x0 = -ONE_16;
			dx = x1 + ONE_16;
			x0pixel = s_minScreenX;
		}
		if (clipX1_Near != 0 && x1pixel < s_maxScreenX)
		{
			dx = ONE_16 - x0;
			x1pixel = s_maxScreenX;
		}

		// The wall is backfacing if x0 > x1
		if (x0pixel > x1pixel)
		{
			wall->visible = 0;
			return;
		}
		// The wall is completely outside of the screen.
		if (x0pixel > s_maxScreenX || x1pixel < s_minScreenX)
		{
			wall->visible = 0;
			return;
		}
		if (s_nextWall == MAX_SEG)
		{
			TFE_System::logWrite(LOG_ERROR, "ClassicRenderer", "Wall_Process : Maximum processed walls exceeded!");
			wall->visible = 0;
			return;
		}
	
		RWallSegment* wallSeg = &s_wallSegListSrc[s_nextWall];
		s_nextWall++;

		if (x0pixel < s_minScreenX)
		{
			x0pixel = s_minScreenX;
		}
		if (x1pixel > s_maxScreenX)
		{
			x1pixel = s_maxScreenX;
		}

		wallSeg->srcWall = wall;
		wallSeg->wallX0_raw = x0pixel;
		wallSeg->wallX1_raw = x1pixel;
		wallSeg->z0 = z0;
		wallSeg->z1 = z1;
		wallSeg->uCoord0 = curU;
		wallSeg->wallX0 = x0pixel;
		wallSeg->wallX1 = x1pixel;
		wallSeg->x0View = x0;

		s32 slope, den, orient;
		if (RMath::abs(dx) > RMath::abs(dz))
		{
			slope = div16(dz, dx);
			den = dx;
			orient = WORIENT_DZ_DX;
		}
		else
		{
			slope = div16(dx, dz);
			den = dz;
			orient = WORIENT_DX_DZ;
		}

		wallSeg->slope  = slope;
		wallSeg->uScale = div16(texelLenRem, den);
		wallSeg->orient = orient;
		/*if (x0pixel == x1pixel)
		{
			wallSeg->slope = 0;
		}*/

		wall->visible = 1;
	}

	s32 wall_mergeSort(RWallSegment* segOutList, s32 availSpace, s32 start, s32 count)
	{
		count = min(count, s_maxWallCount);

		s32 outIndex = 0;
		s32 srcIndex = 0;
		s32 splitWallCount = 0;
		s32 splitWallIndex = -count;

		RWallSegment* srcSeg = &s_wallSegListSrc[start];
		RWallSegment* curSegOut = segOutList;

		RWallSegment  tempSeg;
		RWallSegment* newSeg = &tempSeg;
		RWallSegment  splitWalls[MAX_SPLIT_WALLS];
				
		while (1)
		{
			RWall* srcWall = srcSeg->srcWall;
			// TODO : fix this 
			//bool processed = (s_drawFrame == srcWall->drawFrame);
			bool processed = false;
			bool insideWindow = ((srcSeg->z0 >= s_minSegZ || srcSeg->z1 >= s_minSegZ) && srcSeg->wallX0 <= s_windowMaxX && srcSeg->wallX1 >= s_windowMinX);
			if (!processed && insideWindow)
			{
				// Copy the source segment into "newSeg" so it can be modified.
				*newSeg = *srcSeg;

				// Clip the segment 'newSeg' to the current window.
				if (newSeg->wallX0 < s_windowMinX) { newSeg->wallX0 = s_windowMinX; }
				if (newSeg->wallX1 > s_windowMaxX) { newSeg->wallX1 = s_windowMaxX; }

				// Check 'newSeg' versus all of the segments already added for this sector.
				RWallSegment* sortedSeg = segOutList;
				s32 segHidden = 0;
				for (s32 n = 0; n < outIndex && segHidden == 0; n++, sortedSeg++)
				{
					// Trivially skip segments that do not overlap in screenspace.
					bool segOverlap = newSeg->wallX0 <= sortedSeg->wallX1 && sortedSeg->wallX0 <= newSeg->wallX1;
					if (!segOverlap) { continue; }

					RWall* outSrcWall = sortedSeg->srcWall;
					RWall* newSrcWall = newSeg->srcWall;

					vec2* outV0 = outSrcWall->v0;
					vec2* outV1 = outSrcWall->v1;
					vec2* newV0 = newSrcWall->v0;
					vec2* newV1 = newSrcWall->v1;

					s32 newMinZ = min(newV0->z, newV1->z);
					s32 newMaxZ = max(newV0->z, newV1->z);
					s32 outMinZ = min(outV0->z, outV1->z);
					s32 outMaxZ = max(outV0->z, outV1->z);
					s32 side;

					if (newSeg->wallX0 <= sortedSeg->wallX0 && newSeg->wallX1 >= sortedSeg->wallX1)
					{
						if (newMaxZ < outMinZ || newMinZ > outMaxZ)
						{
							// This is a clear case, the 'newSeg' is clearly in front of or behind the current 'sortedSeg'
							side = (newV0->z < outV0->z) ? FRONT : BACK;
						}
						else if (newV0->z < outV0->z)
						{
							side = FRONT;
							if ((segmentCrossesLine(outV0->x, outV0->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0 ||	// (outV0, 0) does NOT cross (newV0, newV1)
								 segmentCrossesLine(outV1->x, outV1->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0) &&	// (outV1, 0) does NOT cross (newV0, newV1)
								(segmentCrossesLine(newV0->x, newV0->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0 ||	// (newV0, 0) crosses (outV0, outV1)
								 segmentCrossesLine(newV1->x, newV1->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0))		// (newV1, 0) crosses (outV0, outV1)
							{
								side = BACK;
							}
						}
						else  // newV0->z >= outV0->z
						{
							side = BACK;
							if ((segmentCrossesLine(newV0->x, newV0->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) != 0 ||	// (newV0, 0) does NOT cross (outV0, outV1)
								 segmentCrossesLine(newV1->x, newV1->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) != 0) &&	// (newV1, 0) does NOT cross (outV0, outV1)
								(segmentCrossesLine(outV0->x, outV0->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) == 0 ||	// (outV0, 0) crosses (newV0, newV1)
								 segmentCrossesLine(outV1->x, outV1->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) == 0))	// (outV1, 0) crosses (newV0, newV1)
							{
								side = FRONT;
							}
						}

						// 'newSeg' is in front of 'sortedSeg' and it completely hides it.
						if (side == FRONT)
						{
							s32 copyCount = outIndex - 1 - n;
							RWallSegment* outCur = sortedSeg;
							RWallSegment* outNext = sortedSeg + 1;

							// We are deleting outCur since it is completely hidden by moving all segments from outNext onwards to outCur.
							memmove(outCur, outNext, copyCount * sizeof(RWallSegment));

							// We are deleting outCur since it is completely hidden by 'newSeg'
							// Back up 1 step in the loop, so that outNext is processed (it will hold the same location as outCur before deletion).
							curSegOut = curSegOut - 1;
							sortedSeg = sortedSeg - 1;
							outIndex--;
							n--;
						}
						// 'newSeg' is behind 'sortedSeg' and they overlap.
						else    // (side == BACK)
						{
							// If 'newSeg' is behind 'sortedSeg' but it sticks out on both sides, then
							// 'newSeg' must be split.
							// |NNN|OOOOOOO|SSS|  -> N = newSeg, O = sortedSeg, S = splitSeg from newSeg.
							if (sortedSeg->wallX0 > newSeg->wallX0 && sortedSeg->wallX1 < newSeg->wallX1)
							{
								if (splitWallCount == MAX_SPLIT_WALLS)
								{
									TFE_System::logWrite(LOG_ERROR, "RendererClassic", "Wall_MergeSort : Maximum split walls exceeded!");
									segHidden = 0xffff;
									newSeg->wallX1 = sortedSeg->wallX0 - 1;
									break;
								}
								else
								{
									RWallSegment* splitWall = &splitWalls[splitWallCount];
									*splitWall = *newSeg;
									splitWall->wallX0 = sortedSeg->wallX1 + 1;
									splitWallCount++;

									newSeg->wallX1 = sortedSeg->wallX0 - 1;
								}
							}
							// if 'newSeg' is to the left, adjust it to end where 'sortedSeg' starts.
							// |NNN|OOOOOO|  -> N = newSeg, O = sortedSeg
							else if (sortedSeg->wallX0 > newSeg->wallX0)
							{
								newSeg->wallX1 = sortedSeg->wallX0 - 1;
							}
							// if 'newSeg' is to the right, adjust it to start where 'sortedSeg' ends.
							// |OOOOO|NNN|  -> N = newSeg, O = sortedSeg
							else
							{
								newSeg->wallX0 = sortedSeg->wallX1 + 1;
							}
						}
					}
					else  // (newSeg->wallX0 > sortedSeg->wallX0 || newSeg->wallX1 < sortedSeg->wallX1)
					{
						if (newSeg->wallX0 >= sortedSeg->wallX0 && newSeg->wallX1 <= sortedSeg->wallX1)
						{
							if (newMaxZ < outMinZ || newMinZ > outMaxZ)
							{
								side = (newV0->z < outV0->z) ? FRONT : BACK;
							}
							else if (newV0->z < outV0->z)
							{
								side = FRONT;
								if ((segmentCrossesLine(outV0->x, outV0->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0 ||
									 segmentCrossesLine(outV1->x, outV1->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0) &&
									(segmentCrossesLine(newV0->x, newV0->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0 ||
									 segmentCrossesLine(newV1->x, newV1->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0))
								{
									side = BACK;
								}
							}
							else  // (newV0->z >= outV0->z)
							{
								side = BACK;
								if ((segmentCrossesLine(newV0->x, newV0->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) != 0 ||
									 segmentCrossesLine(newV1->x, newV1->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) != 0) &&
									(segmentCrossesLine(outV0->x, outV0->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) == 0 ||
									 segmentCrossesLine(outV1->x, outV1->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) == 0))
								{
									side = FRONT;
								}
							}

							if (side == BACK)
							{
								// The new segment is hidden behind the 'sortedSeg' and can be discarded.
								segHidden = 0xffff;
								break;
							}
							// side == FRONT
							else if (newSeg->wallX0 > sortedSeg->wallX0 && newSeg->wallX1 <= sortedSeg->wallX1)
							{
								if (splitWallCount == MAX_SPLIT_WALLS)
								{
									TFE_System::logWrite(LOG_ERROR, "RendererClassic", "Wall_MergeSort : Maximum split walls exceeded!");
									segHidden = 0xffff;
									break;
								}
								else
								{
									// Split sortedSeg into 2 and insert newSeg in between.
									// { sortedSeg | newSeg | splitWall (from sortedSeg) }
									RWallSegment* splitWall = &splitWalls[splitWallCount];
									splitWallCount++;

									*splitWall = *sortedSeg;
									splitWall->wallX0 = newSeg->wallX1 + 1;
									sortedSeg->wallX1 = newSeg->wallX0 - 1;
								}
							}
							else if (newSeg->wallX0 > sortedSeg->wallX0)
							{
								sortedSeg->wallX1 = newSeg->wallX0 - 1;
							}
							else  // (newSeg->wallX0 <= sortedSeg->wallX0)
							{
								sortedSeg->wallX0 = newSeg->wallX1 + 1;
							}
						}
						// (newSeg->wallX0 < sortedSeg->wallX0 || newSeg->wallX1 > sortedSeg->wallX1)
						else if (newSeg->wallX1 >= sortedSeg->wallX0 && newSeg->wallX1 <= sortedSeg->wallX1)
						{
							if (newMinZ > outMaxZ)
							{
								if (newV1->z >= outV0->z)
								{
									newSeg->wallX1 = sortedSeg->wallX0 - 1;
								}
								else
								{
									sortedSeg->wallX0 = newSeg->wallX1 + 1;
								}
							}
							else if (segmentCrossesLine(newV1->x, newV1->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0 ||
								     segmentCrossesLine(outV0->x, outV0->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0)
							{
								newSeg->wallX1 = sortedSeg->wallX0 - 1;
							}
							// Is this real?
							else
							{
								sortedSeg->wallX0 = newSeg->wallX1 + 1;
							}
						}
						else if (newMaxZ < outMinZ || newMinZ > outMaxZ)
						{
							if (newV0->z >= outV1->z)
							{
								newSeg->wallX0 = sortedSeg->wallX1 + 1;
							}
							else
							{
								sortedSeg->wallX1 = newSeg->wallX0 - 1;
							}
						}
						else if (segmentCrossesLine(newV0->x, newV0->z, 0, 0, outV0->x, outV0->z, outV1->x, outV1->z) == 0 ||
							     segmentCrossesLine(outV1->x, outV1->z, 0, 0, newV0->x, newV0->z, newV1->x, newV1->z) != 0)
						{
							newSeg->wallX0 = sortedSeg->wallX1 + 1;
						}
						else
						{
							sortedSeg->wallX1 = newSeg->wallX0 - 1;
						}
					}
				} // for (s32 n = 0; n < outIndex && segHidden == 0; n++, sortedSeg++)

				// If the new segment is still visible and not back facing.
				if (segHidden == 0 && newSeg->wallX0 <= newSeg->wallX1)
				{
					if (outIndex == availSpace)
					{
						TFE_System::logWrite(LOG_ERROR, "RendererClassic", "Wall_MergeSort : Maximum merged walls exceeded!");
					}
					else
					{
						// Copy the temporary segment to the current segment.
						*curSegOut = *newSeg;
						curSegOut++;
						outIndex++;
					}
				}
			}  // if (!processed && insideWindow)
			splitWallIndex++;
			srcIndex++;
			if (srcIndex < count)
			{
				srcSeg++;
			}
			else if (splitWallIndex < splitWallCount)
			{
				srcSeg = &splitWalls[splitWallIndex];
			}
			else
			{
				break;
			}
		}  // while (1)

		return outIndex;
	}
	
	void wall_drawSolid(RWallSegment* wallSegment)
	{
		RWall* srcWall = wallSegment->srcWall;
		RSector* sector = srcWall->sector;
		TextureFrame* texture = srcWall->midTex;

		s32 ceilingHeight = sector->ceilingHeight;
		s32 floorHeight = sector->floorHeight;

		s32 ceilEyeRel  = ceilingHeight - s_eyeHeight;
		s32 floorEyeRel = floorHeight   - s_eyeHeight;

		s32 z0 = wallSegment->z0;
		s32 z1 = wallSegment->z1;

		s32 y0C = div16(mul16(ceilEyeRel,  s_focalLenAspect), z0) + s_halfHeight;
		s32 y0F = div16(mul16(floorEyeRel, s_focalLenAspect), z0) + s_halfHeight;

		s32 y1C = div16(mul16(ceilEyeRel,  s_focalLenAspect), z1) + s_halfHeight;
		s32 y1F = div16(mul16(floorEyeRel, s_focalLenAspect), z1) + s_halfHeight;

		s32 y0C_pixel = round16(y0C);
		s32 y1C_pixel = round16(y1C);

		s32 y0F_pixel = round16(y0F);
		s32 y1F_pixel = round16(y1F);

		s32 x = wallSegment->wallX0;
		s32 length = wallSegment->wallX1 - wallSegment->wallX0 + 1;
		s32 numerator = solveForZ_Numerator(wallSegment);

		// For some reason we only early-out if the ceiling is below the view.
		if (y0C_pixel > s_windowMaxY && y1C_pixel > s_windowMaxY)
		{
			s32 yMax = (s_windowMaxY + 1) << 16;
			//addWallPartScreen(length, x, 0, yMax, 0, yMax);

			for (s32 i = 0; i < length; i++, x++)
			{
				s_depth1d[x] = solveForZ(wallSegment, x, numerator);
				s_columnTop[x] = s_windowMaxY;
			}

			srcWall->visible = 0;
			//srcWall->y1 = -1;
			return;
		}

		s_texHeightMask = texture ? texture->height - 1 : 0;
		TextureFrame* signTex = srcWall->signTex;
		if (signTex)	// ecx
		{
			// 1fd683:
		}

		s32 wallDeltaX = (wallSegment->wallX1_raw - wallSegment->wallX0_raw) << 16;
		s32 dYdXtop = 0, dYdXbot = 0;
		if (wallDeltaX != 0)
		{
			dYdXtop = div16(y1C - y0C, wallDeltaX);
			dYdXbot = div16(y1F - y0F, wallDeltaX);
		}

		s32 clippedXDelta = (wallSegment->wallX0 - wallSegment->wallX0_raw) << 16;
		if (clippedXDelta != 0)
		{
			y0C += mul16(dYdXtop, clippedXDelta);
			y0F += mul16(dYdXbot, clippedXDelta);
		}
		flat_addEdges(length, wallSegment->wallX0, dYdXbot, y0F, dYdXtop, y0C);

		const s32 texWidth = texture ? texture->width : 0;
		const bool flipHorz = (srcWall->flags1 & WF1_FLIP_HORIZ) != 0;
				
		for (s32 i = 0; i < length; i++, x++)
		{
			s32 top = round16(y0C);
			s32 bot = round16(y0F);
			s_columnBot[x] = bot + 1;
			s_columnTop[x] = top - 1;

			if (top < s_windowTop[x])
			{
				top = s_windowTop[x];
			}
			if (bot > s_windowBot[x])
			{
				bot = s_windowBot[x];
			}
			s_yPixelCount = bot - top + 1;

			s32 dxView = 0;
			s32 z = solveForZ(wallSegment, x, numerator, &dxView);
			s_depth1d[x] = z;

			s32 uScale = wallSegment->uScale;
			s32 uCoord0 = wallSegment->uCoord0 + srcWall->midUOffset;
			s32 uCoord = uCoord0 + ((wallSegment->orient == WORIENT_DZ_DX) ? mul16(dxView, uScale) : mul16(z - z0, uScale));

			if (s_yPixelCount > 0)
			{
				// texture wrapping, assumes texWidth is a power of 2.
				s32 texelU = (uCoord >> 16) & (texWidth - 1);
				// flip texture coordinate if flag set.
				if (flipHorz) { texelU = texWidth - texelU - 1; }

				// Calculate the vertical texture coordinate start and increment.
				s32 wallHeightPixels = y0F - y0C + ONE_16;
				s32 wallHeightTexels = srcWall->midTexelHeight;

				// s_vCoordStep = tex coord "v" step per y pixel step -> dVdY;
				// Note the result is in x.16 fixed point, which is why it must be shifted by 16 again before divide.
				s_vCoordStep = div16(wallHeightTexels, wallHeightPixels);

				// texel offset from the actual fixed point y position and the truncated y position.
				s32 vPixelOffset = y0F - (bot << 16) + HALF_16;

				// scale the texel offset based on the v coord step.
				// the result is the sub-texel offset
				s32 v0 = mul16(s_vCoordStep, vPixelOffset);
				s_vCoordFixed = v0 + srcWall->midVOffset;

				// Texture image data = imageStart + u * texHeight
				// ***Checks against texture are because all walls are being forced to render as solid***
				// TODO: Use wall render flags and then remove checks
				if (texture)
				{
					s_texImage = texture->image + (texelU << texture->logSizeY);
				}

				// Skip for now and just write directly to the screen...
				// columnOutStart + (x*320 + top) * 80;
				//s_curColumnOut = s_columnOut[x] + s_scaleX80[top];
				s_columnLight = computeLighting(z, srcWall->wallLight);
				s_columnOut = &s_display[top * s_width + x];

				// draw the column
				if (s_columnLight)
				{
					drawColumn_Lit(top, bot);
				}
				else
				{
					drawColumn_Fullbright(top, bot);
				}

				if (signTex)
				{
				}
			}

			y0C += dYdXtop;
			y0F += dYdXbot;
		}

		//srcWall->y1 = -1;
	}

	void wall_drawMask(RWallSegment* wallSegment)
	{
		RWall* srcWall = wallSegment->srcWall;
		RSector* sector = srcWall->sector;
		RSector* nextSector = srcWall->nextSector;

		s32 z0 = wallSegment->z0;
		s32 z1 = wallSegment->z1;
		u32 flags1 = sector->flags1;
		u32 nextFlags1 = nextSector->flags1;

		s32 cProj0, cProj1;
		if ((flags1 & SEC_FLAGS1_EXTERIOR) && (nextFlags1 & SEC_FLAGS1_EXT_ADJ))  // ceiling
		{
			cProj0 = cProj1 = (s_windowMinY << 16);
		}
		else
		{
			s32 ceilRel = sector->ceilingHeight - s_eyeHeight;
			cProj0 = div16(mul16(ceilRel, s_focalLenAspect), z0) + s_halfHeight;
			cProj1 = div16(mul16(ceilRel, s_focalLenAspect), z1) + s_halfHeight;
		}

		s32 c0pixel = round16(cProj0);
		s32 c1pixel = round16(cProj1);
		if (c0pixel > s_windowMaxY && c1pixel > s_windowMaxY)
		{
			s32 x = wallSegment->wallX0;
			s32 length = wallSegment->wallX1 - wallSegment->wallX0 + 1;
			flat_addEdges(length, x, 0, (s_windowMaxY + 1) << 16, 0, (s_windowMaxY + 1) << 16);
			const s32 numerator = solveForZ_Numerator(wallSegment);
			for (s32 i = 0; i < length; i++, x++)
			{
				s_depth1d[x] = solveForZ(wallSegment, x, numerator);
			}

			srcWall->visible = 0;
			srcWall->drawFlags = -1;
			return;
		}

		s32 fProj0, fProj1;
		if ((sector->flags1 & SEC_FLAGS1_PIT) && (nextFlags1 & SEC_FLAGS1_EXT_FLOOR_ADJ))	// floor
		{
			fProj0 = fProj1 = (s_windowMaxY << 16);
		}
		else
		{
			s32 floorRel = sector->floorHeight - s_eyeHeight;
			fProj0 = div16(mul16(floorRel, s_focalLenAspect), z0) + s_halfHeight;
			fProj1 = div16(mul16(floorRel, s_focalLenAspect), z1) + s_halfHeight;
		}

		s32 f0pixel = round16(fProj0);
		s32 f1pixel = round16(fProj1);
		if (f0pixel < s_windowMinY && f1pixel < s_windowMinY)
		{
			s32 x = wallSegment->wallX0;
			s32 length = wallSegment->wallX1 - wallSegment->wallX0 + 1;
			flat_addEdges(length, x, 0, (s_windowMinY - 1) << 16, 0, (s_windowMinY - 1) << 16);

			const s32 numerator = solveForZ_Numerator(wallSegment);
			for (s32 i = 0; i < length; i++, x++)
			{
				s_depth1d[x] = solveForZ(wallSegment, x, numerator);
				s_columnBot[x] = s_windowMinY;
			}
			srcWall->visible = 0;
			srcWall->drawFlags = -1;
			return;
		}

		s32 xStartOffset = (wallSegment->wallX0 - wallSegment->wallX0_raw) << 16;
		s32 length = wallSegment->wallX1 - wallSegment->wallX0 + 1;

		s32 numerator = solveForZ_Numerator(wallSegment);
		s32 lengthRaw = (wallSegment->wallX1_raw - wallSegment->wallX0_raw) << 16;
		s32 dydxCeil = 0;
		s32 dydxFloor = 0;
		if (lengthRaw != 0)
		{
			dydxCeil = div16(cProj1 - cProj0, lengthRaw);
			dydxFloor = div16(fProj1 - fProj0, lengthRaw);
		}
		s32 y0 = cProj0;
		s32 y1 = fProj0;
		s32 x = wallSegment->wallX0;
		if (xStartOffset != 0)
		{
			y0 = mul16(dydxCeil, xStartOffset) + cProj0;
			y1 = mul16(dydxFloor, xStartOffset) + fProj0;
		}

		flat_addEdges(length, x, dydxFloor, y1, dydxCeil, y0);
		s32 nextFloor = nextSector->floorHeight;
		s32 nextCeil  = nextSector->ceilingHeight;
		// There is an opening in this wall to the next sector.
		if (nextFloor > nextCeil)
		{
			// TODO:
			//func_1fb0c4(length, x, dydxFloor, y1, dydxCeil, y0, wallSegment);
		}
		if (length != 0)
		{
			for (s32 i = 0; i < length; i++, x++)
			{
				s32 y0_pixel = round16(y0);
				s32 y1_pixel = round16(y1);
				s_columnTop[x] = y0_pixel - 1;
				s_columnBot[x] = y1_pixel + 1;

				s_depth1d[x] = solveForZ(wallSegment, x, numerator);
				y0 += dydxCeil;
				y1 += dydxFloor;
			}
		}

		srcWall->drawFlags = -1;
	}

	void wall_drawBottom(RWallSegment* wallSegment)
	{
		RWall* wall = wallSegment->srcWall;
		RSector* sector = wall->sector;
		RSector* nextSector = wall->nextSector;
		TextureFrame* tex = wall->botTex;

		s32 z0 = wallSegment->z0;
		s32 z1 = wallSegment->z1;

		s32 cProj0, cProj1;
		if ((sector->flags1 & SEC_FLAGS1_EXTERIOR) && (nextSector->flags1 & SEC_FLAGS1_EXT_ADJ))
		{
			cProj1 = s_windowMinY << 16;
			cProj0 = cProj1;
		}
		else
		{
			s32 ceilRel = sector->ceilingHeight - s_eyeHeight;
			cProj0 = div16(mul16(ceilRel, s_focalLenAspect), z0) + s_halfHeight;
			cProj1 = div16(mul16(ceilRel, s_focalLenAspect), z1) + s_halfHeight;
		}

		s32 cy0 = round16(cProj0);
		s32 cy1 = round16(cProj1);
		if (cy0 > s_windowMaxY && cy1 >= s_windowMaxY)
		{
			wall->visible = 0;
			s32 x = wallSegment->wallX0;
			s32 length = wallSegment->wallX1 - x + 1;

			flat_addEdges(length, x, 0, s_windowMaxY + 1, 0, s_windowMaxY + 1);

			s32 num = solveForZ_Numerator(wallSegment);
			for (s32 i = 0; i < length; i++, x++)
			{
				s_depth1d[x] = solveForZ(wallSegment, x, num);
				s_columnTop[x] = s_windowMaxY;
			}
			//wall->y1 = -1;
			return;
		}

		s32 floorRel = sector->floorHeight - s_eyeHeight;
		s32 fProj0 = div16(mul16(floorRel, s_focalLenAspect), z0) + s_halfHeight;
		s32 fProj1 = div16(mul16(floorRel, s_focalLenAspect), z1) + s_halfHeight;
		s32 fy0 = round16(fProj0);
		s32 fy1 = round16(fProj1);
		if (fy0 < s_windowMinY && fy1 < s_windowMinY)
		{
			// Wall is above the top of the screen.
			wall->visible = 0;
			s32 x = wallSegment->wallX0;
			s32 length = wallSegment->wallX1 - x + 1;

			flat_addEdges(length, x, 0, (s_windowMinY - 1) << 16, 0, (s_windowMinY - 1) << 16);

			s32 num = solveForZ_Numerator(wallSegment);
			for (s32 i = 0; i < length; i++, x++)
			{
				s_depth1d[x] = solveForZ(wallSegment, x, num);
				s_columnBot[x] = s_windowMinY;
			}
			//wall->y1 = -1;
			return;
		}

		s32 floorRelNext = nextSector->floorHeight - s_eyeHeight;
		s32 fNextProj0 = div16(mul16(floorRelNext, s_focalLenAspect), z0) + s_halfHeight;
		s32 fNextProj1 = div16(mul16(floorRelNext, s_focalLenAspect), z1) + s_halfHeight;
		s32 xOffset = wallSegment->wallX0 - wallSegment->wallX0_raw;
		s32 length = wallSegment->wallX1 - wallSegment->wallX0 + 1;
		s32 lengthRaw = wallSegment->wallX1_raw - wallSegment->wallX0_raw;
		s32 lengthRawFixed = lengthRaw << 16;
		s32 xOffsetFixed = xOffset << 16;

		s32 floorNext_dYdX = 0;
		s32 floor_dYdX = 0;
		s32 ceil_dYdX = 0;
		if (lengthRawFixed != 0)
		{
			floorNext_dYdX = div16(fNextProj1 - fNextProj0, lengthRawFixed);
			floor_dYdX = div16(fProj1 - fProj0, lengthRawFixed);
			ceil_dYdX = div16(cProj1 - cProj0, lengthRawFixed);
		}
		if (xOffsetFixed)
		{
			fNextProj0 += mul16(floorNext_dYdX, xOffsetFixed);
			fProj0 += mul16(floor_dYdX, xOffsetFixed);
			cProj0 += mul16(ceil_dYdX, xOffsetFixed);
		}

		s32 yTop = fNextProj0;
		s32 yC = cProj0;
		s32 yBot = fProj0;
		s32 x = wallSegment->wallX0;
		flat_addEdges(length, wallSegment->wallX0, floor_dYdX, fProj0, ceil_dYdX, cProj0);

		s32 yTop0 = round16(fNextProj0);
		s32 yTop1 = round16(fNextProj1);
		if ((yTop0 > s_windowMinY || yTop1 > s_windowMinY) && sector->ceilingHeight < nextSector->floorHeight)
		{
			// TODO
			//func_1fb0c4(length, wallSegment->wallX0, floorNext_dYdX, fNextProj0, ceil_dYdX, cProj0, wallSegment);
		}

		if (yTop0 > s_windowMaxY && yTop1 > s_windowMaxY)
		{
			s32 bot = s_windowMaxY + 1;
			s32 num = solveForZ_Numerator(wallSegment);
			for (s32 i = 0; i < length; i++, x++, yC += ceil_dYdX)
			{
				s32 yC_pixel = min(round16(yC), s_windowBot[x]);
				s_columnTop[x] = yC_pixel - 1;
				s_columnBot[x] = bot;
				s_depth1d[x] = solveForZ(wallSegment, x, num);
			}
			//wall->y1 = -1;
			return;
		}

		s32 u0 = wallSegment->uCoord0;
		s32 num = solveForZ_Numerator(wallSegment);
		s_texHeightMask = tex->height - 1;
		s32 flipHorz = (wall->flags1 & WF1_FLIP_HORIZ) ? -1 : 0;
		s32 illumSign = (wall->flags1 & WF1_ILLUM_SIGN) ? -1 : 0;
		TextureFrame* signTex = wall->signTex;
		if (wall->signTex)
		{
			// TODO
		}

		if (length > 0)
		{
			for (s32 i = 0; i < length; i++, x++)
			{
				s32 yTop_pixel = round16(yTop);
				s32 yC_pixel   = round16(yC);
				s32 yBot_pixel = round16(yBot);

				s_columnTop[x] = yC_pixel - 1;
				s_columnBot[x] = yBot_pixel + 1;

				if (yTop_pixel < s_windowTop[x])
				{
					yTop_pixel = s_windowTop[x];
				}
				if (yBot_pixel > s_windowBot[x])
				{
					yBot_pixel = s_windowBot[x];
				}
				s_yPixelCount = yBot_pixel - yTop_pixel + 1;

				// Calculate perspective correct Z and U (texture coordinate).
				s32 dxView;
				s32 z = solveForZ(wallSegment, x, num, &dxView);
				s32 u;
				if (wallSegment->orient == WORIENT_DZ_DX)
				{
					u = u0 + mul16(dxView, wallSegment->uScale) + wall->botUOffset;
				}
				else
				{
					s32 dz = z - z0;
					u = u0 + mul16(dz, wallSegment->uScale) + wall->botUOffset;
				}
				s_depth1d[x] = z;
				if (s_yPixelCount > 0)
				{
					s32 widthMask = tex->width - 1;
					s32 texelU = (u >> 16) & widthMask;
					if (flipHorz != 0)
					{
						texelU = widthMask - texelU;
					}

					s_vCoordStep = div16(wall->botTexelHeight, yBot - yTop + ONE_16);
					s32 v0 = mul16(yBot - (yBot_pixel << 16) + HALF_16, s_vCoordStep);
					s_vCoordFixed = v0 + wall->botVOffset;
					s_texImage = &tex->image[texelU << tex->logSizeY];
					s_columnOut = &s_display[yTop_pixel * s_width + x];
					s_columnLight = computeLighting(z, wall->wallLight);
					if (s_columnLight)
					{
						drawColumn_Lit(yTop_pixel, yBot_pixel);
					}
					else
					{
						drawColumn_Fullbright(yTop_pixel, yBot_pixel);
					}
					if (signTex)
					{
						// TODO
					}
				}
				yTop += floorNext_dYdX;
				yBot += floor_dYdX;
				yC += ceil_dYdX;
			}
		}
		//wall->y1 = -1;
	}

	void wall_drawTop(RWallSegment* wallSegment)
	{
		// For now just mask out the area.
		wall_drawMask(wallSegment);
	}

	void wall_drawTopAndBottom(RWallSegment* wallSegment)
	{
		// For now just mask out the area.
		wall_drawMask(wallSegment);
	}
	
	// Determines if segment A is disjoint from the line formed by B - i.e. they do not intersect.
	// Returns 1 if segment A does NOT cross line B or 0 if it does.
	s32 segmentCrossesLine(s32 ax0, s32 ay0, s32 ax1, s32 ay1, s32 bx0, s32 by0, s32 bx1, s32 by1)
	{
		// Convert from 16 fractional bits to 12.
		bx0 >>= 4;
		by0 >>= 4;
		ax1 >>= 4;
		ax0 >>= 4;
		ay0 >>= 4;
		ay1 >>= 4;
		bx1 >>= 4;
		by1 >>= 4;

		// mul16() functions on 12 bit values is equivalent to: a * b / 16
		// [ (a1-b0)x(b1-b0) ].[ (a0-b0)x(b1 - b0) ]
		// In 2D x = "perp product"
		s_segmentCross = mul16(mul16(ax1 - bx0, by1 - by0) - mul16(ay1 - by0, bx1 - bx0),
			                   mul16(ax0 - bx0, by1 - by0) - mul16(ay0 - by0, bx1 - bx0));

		return s_segmentCross > 0 ? 1 : 0;
	}
		
	// When solving for Z, part of the computation can be done once per wall.
	s32 solveForZ_Numerator(RWallSegment* wallSegment)
	{
		const s32 z0 = wallSegment->z0;

		s32 numerator;
		if (wallSegment->orient == WORIENT_DZ_DX)
		{
			numerator = z0 - mul16(wallSegment->slope, wallSegment->x0View);
		}
		else  // WORIENT_DX_DZ
		{
			numerator = wallSegment->x0View - mul16(wallSegment->slope, z0);
		}

		return numerator;
	}
	
	// Solve for perspective correct Z at the current x pixel coordinate.
	s32 solveForZ(RWallSegment* wallSegment, s32 x, s32 numerator, s32* outViewDx/*=nullptr*/)
	{
		s32 z;	// perspective correct z coordinate at the current x pixel coordinate.
		if (wallSegment->orient == WORIENT_DZ_DX)
		{
			// Solve for viewspace X at the current pixel x coordinate in order to get dx in viewspace.
			s32 den = s_column_Y_Over_X[x] - wallSegment->slope;
			// Avoid divide by zero.
			if (den == 0) { den = 1; }

			s32 xView = div16(numerator, den);
			// Use the saved x0View to get dx in viewspace.
			s32 dxView = xView - wallSegment->x0View;
			// avoid recalculating for u coordinate computation.
			if (outViewDx) { *outViewDx = dxView; }
			// Once we have dx in viewspace, we multiply by the slope (dZ/dX) in order to get dz.
			s32 dz = mul16(dxView, wallSegment->slope);
			// Then add z0 to get z(x) that is perspective correct.
			z = wallSegment->z0 + dz;
		}
		else  // WORIENT_DX_DZ
		{
			// Directly solve for Z at the current pixel x coordinate.
			s32 den = s_column_X_Over_Y[x] - wallSegment->slope;
			// Avoid divide by 0.
			if (den == 0) { den = 1; }

			z = div16(numerator, den);
		}

		return z;
	}

	void drawColumn_Fullbright(s32 y0, s32 y1)
	{
		s32 vCoordFixed = s_vCoordFixed;
		u8* tex = s_texImage;

		s32 v = floor16(vCoordFixed) & s_texHeightMask;
		s32 end = s_yPixelCount - 1;

		s32 offset = end * s_width;
		for (s32 i = end; i >= 0; i--, offset -= s_width)
		{
			const u8 c = tex[v];
			vCoordFixed += s_vCoordStep;
			v = floor16(vCoordFixed) & s_texHeightMask;
			s_columnOut[offset] = c;
		}
	}

	void drawColumn_Lit(s32 y0, s32 y1)
	{
		s32 vCoordFixed = s_vCoordFixed;
		u8* tex = s_texImage;

		s32 v = floor16(vCoordFixed) & s_texHeightMask;
		s32 end = s_yPixelCount - 1;

		s32 offset = end * s_width;
		for (s32 i = end; i >= 0; i--, offset -= s_width)
		{
			const u8 c = s_columnLight[tex[v]];
			vCoordFixed += s_vCoordStep;
			v = floor16(vCoordFixed) & s_texHeightMask;
			s_columnOut[offset] = c;
		}
	}
}
