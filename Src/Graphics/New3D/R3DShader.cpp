#include "R3DShader.h"
#include "Graphics/Shader.h"

namespace New3D {

static const char *vertexShaderR3D = R"glsl(

#version 120

// uniforms
uniform float	modelScale;

// attributes
attribute vec4	inVertex;
attribute vec3	inNormal;
attribute vec2	inTexCoord;
attribute vec4	inColour;
attribute vec3	inFaceNormal;		// used to emulate r3d culling 
attribute float	inFixedShade;

// outputs to fragment shader
varying vec3	fsViewVertex;
varying vec3	fsViewNormal;		// per vertex normal vector
varying vec2	fsTexCoord;
varying vec4	fsColor;
varying float	fsDiscard;			// can't have varying bool (glsl spec)
varying float	fsFixedShade;

float CalcBackFace(in vec3 viewVertex)
{
	vec3 vt = viewVertex - vec3(0.0);
	vec3 vn = (mat3(gl_ModelViewMatrix) * inFaceNormal);

	// dot product of face normal with view direction
	return dot(vt, vn);
}

void main(void)
{
	fsViewVertex	= vec3(gl_ModelViewMatrix * inVertex);
	fsViewNormal	= (mat3(gl_ModelViewMatrix) * inNormal) / modelScale;
	fsDiscard		= CalcBackFace(fsViewVertex);
	fsColor    		= inColour;
	fsTexCoord		= inTexCoord;
	fsFixedShade	= inFixedShade;
	gl_Position		= gl_ModelViewProjectionMatrix * inVertex;
}
)glsl";

static const char *fragmentShaderR3D = R"glsl(

#version 120

uniform sampler2D tex1;			// base tex
uniform sampler2D tex2;			// micro tex (optional)

// texturing
uniform bool	textureEnabled;
uniform bool	microTexture;
uniform float	microTextureScale;
uniform vec2	baseTexSize;
uniform bool	textureInverted;
uniform bool	textureAlpha;
uniform bool	alphaTest;
uniform bool	discardAlpha;

// general
uniform vec3	fogColour;
uniform vec4	spotEllipse;		// spotlight ellipse position: .x=X position (screen coordinates), .y=Y position, .z=half-width, .w=half-height)
uniform vec2	spotRange;			// spotlight Z range: .x=start (viewspace coordinates), .y=limit
uniform vec3	spotColor;			// spotlight RGB color
uniform vec3	spotFogColor;		// spotlight RGB color on fog
uniform vec3	lighting[2];		// lighting state (lighting[0] = sun direction, lighting[1].x,y = diffuse, ambient intensities from 0-1.0)
uniform bool	lightEnabled;		// lighting enabled (1.0) or luminous (0.0), drawn at full intensity
uniform bool	sunClamp;			// not used by daytona and la machine guns
uniform bool	intensityClamp;		// some games such as daytona and 
uniform bool	specularEnabled;	// specular enabled
uniform float	specularValue;		// specular coefficient
uniform float	shininess;			// specular shininess
uniform float	fogIntensity;
uniform float	fogDensity;
uniform float	fogStart;
uniform float	fogAttenuation;
uniform float	fogAmbient;
uniform bool	fixedShading;
uniform int		hardwareStep;

//interpolated inputs from vertex shader
varying float	fsFogFactor;
varying vec3	fsViewVertex;
varying vec3	fsViewNormal;		// per vertex normal vector
varying vec4	fsColor;
varying vec2	fsTexCoord;
varying float	fsDiscard;
varying float	fsFixedShade;

vec4 GetTextureValue()
{
	vec4 tex1Data = texture2D( tex1, fsTexCoord.st);

	if(textureInverted) {
		tex1Data.rgb = vec3(1.0) - vec3(tex1Data.rgb);
	}

	if (microTexture) {
		vec2 scale    = (baseTexSize / 128.0) * microTextureScale;
		vec4 tex2Data = texture2D( tex2, fsTexCoord.st * scale);
		tex1Data = (tex1Data+tex2Data)/2.0;
	}

	if (alphaTest) {
		if (tex1Data.a < (8.0/16.0)) {
			discard;
		}
	}

	if(discardAlpha && textureAlpha) {
		if (tex1Data.a < 1.0) {
			discard;
		}
	}

	if (textureAlpha == false) {
		tex1Data.a = 1.0;
	}

	return tex1Data;
}

void Step15Luminous(inout vec4 colour)
{
	// luminous polys seem to behave very differently on step 1.5 hardware
	// when fixed shading is enabled the colour is modulated by the vp ambient + fixed shade value
	// when disabled it appears to be multiplied by 1.5, presumably to allow a higher range
	if(hardwareStep==0x15) {
		if(!lightEnabled && textureEnabled) {
			if(fixedShading) {
				colour.rgb *= 1.0 + fsFixedShade + lighting[1].y;
			}
			else {
				colour.rgb *= vec3(1.5);
			}
		}
	}
}

float CalcFog()
{
	float z		= -fsViewVertex.z;
	float fog	= fogIntensity * clamp(fogStart + z * fogDensity, 0.0, 1.0);

	return fog;
}

void main()
{
	vec4 tex1Data;
	vec4 colData;
	vec4 finalData;
	vec4 fogData;

	if(fsDiscard>=0) {
		discard;		//emulate back face culling here
	}

	fogData = vec4(fogColour.rgb * fogAmbient, CalcFog());
	tex1Data = vec4(1.0, 1.0, 1.0, 1.0);

	if(textureEnabled) {
		tex1Data = GetTextureValue();
	}

	colData = fsColor;
	Step15Luminous(colData);			// no-op for step 2.0+	
	finalData = tex1Data * colData;

	if (finalData.a < (1.0/16.0)) {		// basically chuck out any totally transparent pixels value = 1/16 the smallest transparency level h/w supports
		discard;
	}

	float ellipse;
	ellipse = length((gl_FragCoord.xy - spotEllipse.xy) / spotEllipse.zw);
	ellipse = pow(ellipse, 2.0);  // decay rate = square of distance from center
	ellipse = 1.0 - ellipse;      // invert
	ellipse = max(0.0, ellipse);  // clamp

	// Compute spotlight and apply lighting
	float enable, absExtent, d, inv_r, range;

	// start of spotlight
	enable = step(spotRange.x, -fsViewVertex.z);

	if (spotRange.y == 0.0) {
		range = 0.0;
	}
	else {
		absExtent = abs(spotRange.y);

		d = spotRange.x + absExtent + fsViewVertex.z;
		d = min(d, 0.0);

		// slope of decay function
		inv_r = 1.0 / (1.0 + absExtent);

		// inverse-linear falloff
		// Reference: https://imdoingitwrong.wordpress.com/2011/01/31/light-attenuation/
		// y = 1 / (d/r + 1)^2
		range = 1.0 / pow(d * inv_r - 1.0, 2.0);
		range *= enable;
	}

	float lobeEffect = range * ellipse;
	float lobeFogEffect = enable * ellipse;

	if (lightEnabled) {
		vec3   lightIntensity;
		vec3   sunVector;     // sun lighting vector (as reflecting away from vertex)
		float  sunFactor;     // sun light projection along vertex normal (0.0 to 1.0)

		// Sun angle
		sunVector = lighting[0];

		// Compute diffuse factor for sunlight
		if(fixedShading) {
			sunFactor = fsFixedShade;
		}
		else {
			sunFactor = dot(sunVector, fsViewNormal);
		}

		// Clamp ceil, fix for upscaled models without "modelScale" defined
		sunFactor = clamp(sunFactor,-1.0,1.0);

		// Optional clamping, value is allowed to be negative
		if(sunClamp) {
			sunFactor = max(sunFactor,0.0);
		}

		// Total light intensity: sum of all components 
		lightIntensity = vec3(sunFactor*lighting[1].x + lighting[1].y);   // diffuse + ambient

		lightIntensity.rgb += spotColor*lobeEffect;

		// Upper clamp is optional, step 1.5+ games will drive brightness beyond 100%
		if(intensityClamp) {
			lightIntensity = min(lightIntensity,1.0);
		}

		finalData.rgb *= lightIntensity;

		// for now assume fixed shading doesn't work with specular
		if (specularEnabled) {

			float exponent, NdotL, specularFactor;
			vec4 biasIndex, expIndex, multIndex;

			// Always clamp floor to zero, we don't want deep black areas
			NdotL = max(0.0,sunFactor);

			expIndex = vec4(8.0, 16.0, 32.0, 64.0);
			multIndex = vec4(2.0, 2.0, 3.0, 4.0);
			biasIndex = vec4(0.95, 0.95, 1.05, 1.0);
			exponent = expIndex[int(shininess)] / biasIndex[int(shininess)];

			specularFactor = pow(NdotL, exponent);
			specularFactor *= multIndex[int(shininess)];
			specularFactor *= biasIndex[int(shininess)];
			
			specularFactor *= specularValue;
			specularFactor *= lighting[1].x;

			if (colData.a < 1.0) {
				/// Specular hi-light affects translucent polygons alpha channel ///
				finalData.a = max(finalData.a, specularFactor);
			}

			finalData.rgb += vec3(specularFactor);
		}
	}

	// Final clamp: we need it for proper shading in dimmed light and dark ambients
	finalData.rgb = min(finalData.rgb, vec3(1.0));

	// Spotlight on fog
	vec3 lSpotFogColor = spotFogColor * fogAttenuation * fogColour.rgb * lobeFogEffect;

	 // Fog & spotlight applied
	finalData.rgb = mix(finalData.rgb, fogData.rgb + lSpotFogColor, fogData.a);

	gl_FragColor = finalData;
}
)glsl";

R3DShader::R3DShader(const Util::Config::Node &config)
  : m_config(config)
{
	m_shaderProgram		= 0;
	m_vertexShader		= 0;
	m_fragmentShader	= 0;

	Start();	// reset attributes
}

void R3DShader::Start()
{
	m_textured1			= false;
	m_textured2			= false;
	m_textureAlpha		= false;		// use alpha in texture
	m_alphaTest			= false;		// discard fragment based on alpha (ogl does this with fixed function)
	m_lightEnabled		= false;
	m_specularEnabled	= false;
	m_layered			= false;
	m_textureInverted	= false;
	m_fixedShading		= false;
	m_modelScale		= 1.0f;
	m_shininess			= 0;
	m_specularValue		= 0;
	m_microTexScale		= 0;

	m_baseTexSize[0] = 0;
	m_baseTexSize[1] = 0;

	m_dirtyMesh		= true;			// dirty means all the above are dirty, ie first run
	m_dirtyModel	= true;
}

bool R3DShader::LoadShader(const char* vertexShader, const char* fragmentShader)
{
	const char* vShader;
	const char* fShader;
	bool success;

	if (vertexShader) {
		vShader = vertexShader;
	}
	else {
		vShader = vertexShaderR3D;
	}

	if (fragmentShader) {
		fShader = fragmentShader;
	}
	else {
		fShader = fragmentShaderR3D;
	}

	success = LoadShaderProgram(&m_shaderProgram, &m_vertexShader, &m_fragmentShader, m_config["VertexShader"].ValueAs<std::string>(), m_config["FragmentShader"].ValueAs<std::string>(), vShader, fShader);

	m_locTexture1		= glGetUniformLocation(m_shaderProgram, "tex1");
	m_locTexture2		= glGetUniformLocation(m_shaderProgram, "tex2");
	m_locTexture1Enabled= glGetUniformLocation(m_shaderProgram, "textureEnabled");
	m_locTexture2Enabled= glGetUniformLocation(m_shaderProgram, "microTexture");
	m_locTextureAlpha	= glGetUniformLocation(m_shaderProgram, "textureAlpha");
	m_locAlphaTest		= glGetUniformLocation(m_shaderProgram, "alphaTest");
	m_locMicroTexScale	= glGetUniformLocation(m_shaderProgram, "microTextureScale");
	m_locBaseTexSize	= glGetUniformLocation(m_shaderProgram, "baseTexSize");
	m_locTextureInverted= glGetUniformLocation(m_shaderProgram, "textureInverted");

	m_locFogIntensity	= glGetUniformLocation(m_shaderProgram, "fogIntensity");
	m_locFogDensity		= glGetUniformLocation(m_shaderProgram, "fogDensity");
	m_locFogStart		= glGetUniformLocation(m_shaderProgram, "fogStart");
	m_locFogColour		= glGetUniformLocation(m_shaderProgram, "fogColour");
	m_locFogAttenuation	= glGetUniformLocation(m_shaderProgram, "fogAttenuation");
	m_locFogAmbient		= glGetUniformLocation(m_shaderProgram, "fogAmbient");

	m_locLighting		= glGetUniformLocation(m_shaderProgram, "lighting");
	m_locLightEnabled	= glGetUniformLocation(m_shaderProgram, "lightEnabled");
	m_locSunClamp		= glGetUniformLocation(m_shaderProgram, "sunClamp");
	m_locIntensityClamp = glGetUniformLocation(m_shaderProgram, "intensityClamp");
	m_locShininess		= glGetUniformLocation(m_shaderProgram, "shininess");
	m_locSpecularValue	= glGetUniformLocation(m_shaderProgram, "specularValue");
	m_locSpecularEnabled= glGetUniformLocation(m_shaderProgram, "specularEnabled");
	m_locFixedShading	= glGetUniformLocation(m_shaderProgram, "fixedShading");

	m_locSpotEllipse	= glGetUniformLocation(m_shaderProgram, "spotEllipse");
	m_locSpotRange		= glGetUniformLocation(m_shaderProgram, "spotRange");
	m_locSpotColor		= glGetUniformLocation(m_shaderProgram, "spotColor");
	m_locSpotFogColor	= glGetUniformLocation(m_shaderProgram, "spotFogColor");
	m_locModelScale		= glGetUniformLocation(m_shaderProgram, "modelScale");

	m_locHardwareStep	= glGetUniformLocation(m_shaderProgram, "hardwareStep");
	m_locDiscardAlpha	= glGetUniformLocation(m_shaderProgram, "discardAlpha");
	
	return success;
}

GLint R3DShader::GetVertexAttribPos(const char* attrib)
{
	return glGetAttribLocation(m_shaderProgram, attrib);	// probably should cache this but only called 1x per frame anyway
}

void R3DShader::SetShader(bool enable)
{
	if (enable) {
		glUseProgram(m_shaderProgram);
		Start();
		DiscardAlpha(false);	// need some default
	}
	else {
		glUseProgram(0);
	}
}

void R3DShader::SetMeshUniforms(const Mesh* m)
{
	if (m == nullptr) {
		return;			// sanity check
	}

	if (m_dirtyMesh) {
		glUniform1i(m_locTexture1, 0);
		glUniform1i(m_locTexture2, 1);
	}

	if (m_dirtyMesh || m->textured != m_textured1) {
		glUniform1i(m_locTexture1Enabled, m->textured);
		m_textured1 = m->textured;
	}

	if (m_dirtyMesh || m->microTexture != m_textured2) {
		glUniform1i(m_locTexture2Enabled, m->microTexture);
		m_textured2 = m->microTexture;
	}

	if (m_dirtyMesh || m->microTextureScale != m_microTexScale) {
		glUniform1f(m_locMicroTexScale, m->microTextureScale);
		m_microTexScale = m->microTextureScale;
	}

	if (m_dirtyMesh || m->microTexture && (m_baseTexSize[0] != m->width || m_baseTexSize[1] != m->height)) {
		m_baseTexSize[0] = (float)m->width;
		m_baseTexSize[1] = (float)m->height;
		glUniform2fv(m_locBaseTexSize, 1, m_baseTexSize);
	}

	if (m_dirtyMesh || m->inverted != m_textureInverted) {
		glUniform1i(m_locTextureInverted, m->inverted);
		m_textureInverted = m->inverted;
	}

	if (m_dirtyMesh || m->alphaTest != m_alphaTest) {
		glUniform1i(m_locAlphaTest, m->alphaTest);
		m_alphaTest = m->alphaTest;
	}

	if (m_dirtyMesh || m->textureAlpha != m_textureAlpha) {
		glUniform1i(m_locTextureAlpha, m->textureAlpha);
		m_textureAlpha = m->textureAlpha;
	}

	if (m_dirtyMesh || m->fogIntensity != m_fogIntensity) {
		glUniform1f(m_locFogIntensity, m->fogIntensity);
		m_fogIntensity = m->fogIntensity;
	}

	if (m_dirtyMesh || m->lighting != m_lightEnabled) {
		glUniform1i(m_locLightEnabled, m->lighting);
		m_lightEnabled = m->lighting;
	}

	if (m_dirtyMesh || m->shininess != m_shininess) {
		glUniform1f(m_locShininess, m->shininess);
		m_shininess = m->shininess;
	}

	if (m_dirtyMesh || m->specular != m_specularEnabled) {
		glUniform1i(m_locSpecularEnabled, m->specular);
		m_specularEnabled = m->specular;
	}

	if (m_dirtyMesh || m->specularValue != m_specularValue) {
		glUniform1f(m_locSpecularValue, m->specularValue);
		m_specularValue = m->specularValue;
	}

	if (m_dirtyMesh || m->fixedShading != m_fixedShading) {
		glUniform1i(m_locFixedShading, m->fixedShading);
		m_fixedShading = m->fixedShading;
	}

	if (m_dirtyMesh || m->layered != m_layered) {
		m_layered = m->layered;
		if (m_layered) {
			glEnable(GL_STENCIL_TEST);
		}
		else {
			glDisable(GL_STENCIL_TEST);
		}
	}

	m_dirtyMesh = false;
}

void R3DShader::SetViewportUniforms(const Viewport *vp)
{	
	//didn't bother caching these, they don't get frequently called anyway
	glUniform1f	(m_locFogDensity, vp->fogParams[3]);
	glUniform1f	(m_locFogStart, vp->fogParams[4]);
	glUniform3fv(m_locFogColour, 1, vp->fogParams);
	glUniform1f	(m_locFogAttenuation, vp->fogParams[5]);
	glUniform1f	(m_locFogAmbient, vp->fogParams[6]);

	glUniform3fv(m_locLighting, 2, vp->lightingParams);
	glUniform1i (m_locSunClamp, vp->sunClamp);
	glUniform1i (m_locIntensityClamp, vp->intensityClamp);
	glUniform4fv(m_locSpotEllipse, 1, vp->spotEllipse);
	glUniform2fv(m_locSpotRange, 1, vp->spotRange);
	glUniform3fv(m_locSpotColor, 1, vp->spotColor);
	glUniform3fv(m_locSpotFogColor, 1, vp->spotFogColor);

	glUniform1i (m_locHardwareStep, vp->hardwareStep);
}

void R3DShader::SetModelStates(const Model* model)
{
	if (m_dirtyModel || model->scale != m_modelScale) {
		glUniform1f(m_locModelScale, model->scale);
		m_modelScale = model->scale;
	}

	m_dirtyModel = false;
}

void R3DShader::DiscardAlpha(bool discard)
{
	glUniform1i(m_locDiscardAlpha, discard);
}

} // New3D
