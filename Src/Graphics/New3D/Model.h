#ifndef _MODEL_H_
#define _MODEL_H_

#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include <string.h>
#include "Texture.h"
#include "Mat4.h"

namespace New3D {

struct ClipVertex
{
	float pos[4];
};

struct ClipPoly
{
	ClipVertex list[12];		// what's the max number we can hit for a triangle + 4 planes?
	int count = 0;
};

struct Vertex					// half vertex
{
	float pos[4];
	float normal[3];
	float texcoords[2];
	float fixedShade;
};

struct FVertex : Vertex			// full vertex including face attributes
{
	float faceNormal[3];
	UINT8 faceColour[4];

	FVertex& operator=(const Vertex& vertex) 
	{
		memcpy(this, &vertex, sizeof(Vertex));
		return *this;
	}
};

struct R3DPoly
{
	Vertex v[4];			// just easier to have them as an array
	float faceNormal[3];	// we need this to help work out poly winding, i assume the h/w uses this instead of calculating normals itself
	UINT8 faceColour[4];	// per face colour
	int number = 4;
};

struct Poly						// our polys are always 3 triangles, unlike the real h/w
{
	Poly() {};	// default

	Poly(bool firstTriangle, const R3DPoly& r3dPoly) {

		if (firstTriangle) {
			p1 = r3dPoly.v[0];
			p2 = r3dPoly.v[1];
			p3 = r3dPoly.v[2];
		}
		else {
			p1 = r3dPoly.v[0];
			p2 = r3dPoly.v[2];
			p3 = r3dPoly.v[3];
		}

		// copy face attributes
		for (int i = 0; i < 4; i++) {
			p1.faceColour[i] = r3dPoly.faceColour[i];
			p2.faceColour[i] = r3dPoly.faceColour[i];
			p3.faceColour[i] = r3dPoly.faceColour[i];
		}

		for (int i = 0; i < 3; i++) {
			p1.faceNormal[i] = r3dPoly.faceNormal[i];
			p2.faceNormal[i] = r3dPoly.faceNormal[i];
			p3.faceNormal[i] = r3dPoly.faceNormal[i];
		}
	}

	FVertex p1;
	FVertex p2;
	FVertex p3;
};

struct Mesh
{
	//helper funcs
	bool Render(bool alpha) 
	{
		if (alpha) {
			if (!textureAlpha && !polyAlpha) {
				return false;
			}
		}
		else {
			if (polyAlpha) {
				return false;
			}
		}

		return true;
	}

	// texture
	int format, x, y, width, height = 0;
	bool mirrorU = false;
	bool mirrorV = false;
	bool inverted = false;

	// microtexture
	bool	microTexture		= false;
	int		microTextureID		= 0;
	float	microTextureScale	= 0;

	// attributes
	bool doubleSided	= false;
	bool textured		= false;
	bool polyAlpha		= false;		// specified in the rgba colour
	bool textureAlpha	= false;		// use alpha in texture
	bool alphaTest		= false;		// discard fragment based on alpha (ogl does this with fixed function)
	bool clockWise		= true;			// we need to check if the matrix will change the winding
	bool layered		= false;		// stencil poly
	bool highPriority	= false;		// rendered over the top

	// lighting
	bool fixedShading	= false;
	bool lighting		= false;
	bool specular		= false;
	float shininess		= 0;
	float specularValue = 0;
	
	// fog
	float fogIntensity = 1.0f;

	// opengl resources
	int vboOffset		= 0;			// this will be calculated later
	int triangleCount	= 0;
};

struct SortingMesh : public Mesh		// This struct temporarily holds the model data, before it gets copied to the main buffer
{
	std::vector<Poly> polys;
};

struct Model
{
	std::shared_ptr<std::vector<Mesh>> meshes;	// this reason why this is a shared ptr to an array, is that multiple models might use the same meshes

	//which memory are we in
	bool dynamic = true;

	// texture offsets for model
	int textureOffsetX = 0;
	int textureOffsetY = 0;
	int page = 0;

	//matrices
	float modelMat[16];

	//model scale step 1.5+
	float scale = 1.0f;
};

struct Viewport
{
	int		vpX;					// these are the original hardware values
	int		vpY;
	int		vpWidth;
	int		vpHeight;
	float	angle_left;
	float	angle_right;
	float	angle_top;
	float	angle_bottom;

	Mat4	projectionMatrix;		// projection matrix, we will calc this later when we have scene near/far vals

	float	lightingParams[6];		// lighting parameters (see RenderViewport() and vertex shader)
	bool	sunClamp;				// unknown how this is set
	bool	intensityClamp;			// unknown how this is set
	float	spotEllipse[4];			// spotlight ellipse (see RenderViewport())
	float	spotRange[2];			// Z range
	float	spotColor[3];			// color
	float	fogParams[7];			// fog parameters (...)
	float	scrollFog;				// a transparency value that determines if fog is blended over the bottom 2D layer
	int		losPosX, losPosY;		// line of sight position
	int		x, y;					// viewport coordinates (scaled and in OpenGL format)
	int		width, height;			// viewport dimensions (scaled for display surface size)
	int		priority;				// priority
	int		select;					// viewport select?
	int		number;					// viewport number
	float	spotFogColor[3];		// spotlight color on fog
	float	scrollAtt;

	int		hardwareStep;			// not really a viewport param but will do here
};

enum class Clip { INSIDE, OUTSIDE, INTERCEPT, NOT_SET };

class NodeAttributes
{
public:

	NodeAttributes();

	bool Push();
	bool Pop();
	bool StackLimit();
	void Reset();

	int currentTexOffsetX;
	int currentTexOffsetY;
	int currentPage;
	Clip currentClipStatus;
	float currentModelScale;

private:

	struct NodeAttribs
	{
		int texOffsetX;
		int texOffsetY;
		int page;
		Clip clip;
		float modelScale;
	};
	std::vector<NodeAttribs> m_vecAttribs;
};

struct Node
{
	Viewport viewport;
	std::vector<Model> models;
};

} // New3D


#endif