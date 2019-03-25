//--------------------------------------------------------------------------------
// NVIDIA(R) GVDB VOXELS
// Copyright 2019
//
// Redistribution and use in source and binary forms, with or without modification, 
// are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice, this 
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this 
//    list of conditions and the following disclaimer in the documentation and/or 
//    other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors may 
//    be used to endorse or promote products derived from this software without specific 
//   prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
// SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
// OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
// TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, 
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Version 1.0: Sergen Eren 25/3/2019
//----------------------------------------------------------------------------------



// GVDB library
#include "gvdb.h"
#include "file_png.h"

#include <stdlib.h>
#include <stdio.h>
#include "cuda.h"

using namespace nvdb;

// OptiX scene
#include "optix_scene.h"	

// Sample utils
#include "main.h"			// window system 
#include "nv_gui.h"
#include <GL/glew.h>

VolumeGVDB gvdb; 
OptixScene optx;

CUmodule cuCustom; 
CUfunction cuPathTraceKernel; 


class Sample : public NVPWindow {
public:
	virtual bool init();
	virtual void display();
	virtual void reshape(int w, int h);
	virtual void motion(int x, int y, int dx, int dy);
	virtual void mouse(NVPWindow::MouseButton button, NVPWindow::ButtonAction state, int mods, int x, int y);

	void		RebuildOptixGraph(int shading);
	void		draw_topology();

	int			gl_screen_tex;			// screen texture
	int			mouse_down;				// mouse down status
	Vector3DF	delta;					// mouse delta
	int			frame;					// current frame
	int			sample;					// current sample
	int			max_samples;			// sample convergence
	int			m_shading;
	bool		m_render_optix;
	Vector3DF	m_translate;

	int			mat_surf1;				// material id for surface objects
	int			mat_deep;				// material id for volumetric objects
};

void Sample::RebuildOptixGraph(int shading)
{
	optx.ClearGraph();

	nvprintf("Adding OptiX materials.\n");

	/// Add surface material
	mat_surf1 = optx.AddMaterial("optix_trace_surface", "trace_surface", "trace_shadow");
	MaterialParams* matp = optx.getMaterialParams(mat_surf1);
	matp->light_width = 0.5;
	matp->shadow_width = 0.5;
	matp->diff_color = Vector3DF(.5f, .54f, .5f);
	matp->spec_color = Vector3DF(.7f, .7f, .7f);
	matp->spec_power = 80.0;
	matp->env_color = Vector3DF(0, 0, 0);
	matp->refl_width = 0.3f;
	matp->refl_color = Vector3DF(.8, .8, .8);
	matp->refr_width = 0.0f;
	matp->refr_color = Vector3DF(0, 0, 0);
	matp->refr_ior = 1.2f;
	matp->refr_amount = 1.0f;
	matp->refr_offset = 15.0f;
	optx.SetMaterialParams(mat_surf1, matp);

	/// Add deep volume material
	mat_deep = optx.AddMaterial("optix_trace_deep", "trace_deep", "trace_shadow");
	optx.SetMaterialParams(mat_deep, matp);

	// Add GVDB volume to the OptiX scene
	nvprintf("Adding GVDB Volume to OptiX graph.\n");
	int matid;
	char isect;
	switch (shading) {
	case SHADE_TRILINEAR:	matid = mat_surf1;	isect = 'S';	break;
	case SHADE_VOLUME:		matid = mat_deep;	isect = 'D';	break;
	case SHADE_EMPTYSKIP:	matid = mat_surf1;	isect = 'E';	break;
	}
	Vector3DF volmin = gvdb.getWorldMin();
	Vector3DF volmax = gvdb.getWorldMax();
	Matrix4F xform;
	xform.Identity();
	int atlas_glid = gvdb.getAtlasGLID(0);
	optx.AddVolume(atlas_glid, volmin, volmax, xform, matid, isect);

	// Add polygonal model to the OptiX scene
	Model* m = gvdb.getScene()->getModel(0);
	xform.Identity();
	optx.AddPolygons(m, 0, xform);

	// Set Transfer Function (once before validate)
	Vector4DF* src = gvdb.getScene()->getTransferFunc();
	optx.SetTransferFunc(src);

	// Validate OptiX graph
	nvprintf("Validating OptiX.\n");
	optx.ValidateGraph();

	// Assign GVDB data to OptiX	
	nvprintf("Update GVDB Volume.\n");
	optx.UpdateVolume(&gvdb);

	nvprintf("Rebuilding Optix.. \n");
}

bool Sample::init()
{
	int w = getWidth(), h = getHeight();			// window width & height
	mouse_down = -1;
	gl_screen_tex = -1;
	frame = 0;
	sample = 0;
	max_samples = 1024;
	m_render_optix = true;
	m_shading = SHADE_VOLUME;
	m_translate.Set(-200, 0, -400);

	// Initialize Optix Scene
	if (m_render_optix)
		optx.InitializeOptix(w, h);

	// Initialize GVDB
	gvdb.SetVerbose(true);
	gvdb.SetProfile(false, false);
	gvdb.SetCudaDevice(m_render_optix ? GVDB_DEV_CURRENT : GVDB_DEV_FIRST);		// Use OptiX context already created
	gvdb.Initialize();
	gvdb.AddPath("../source/shared_assets/");
	gvdb.AddPath("../shared_assets/");
	gvdb.AddPath(ASSET_PATH);

	// Must set GVDB to create OpenGL atlases, since OptiX uses 
	// opengl to access textures in optix intersection programs.
	gvdb.UseOpenGLAtlas(true);

	// Load polygons
	// This loads an obj file into scene memory on cpu.

	char scnpath[1024];

	
	printf("Loading polygon model.\n");
	gvdb.getScene()->AddModel("lucy.obj", 1.0, 0, 0, 0);
	gvdb.CommitGeometry(0);					// Send the polygons to GPU as OpenGL VBO	
	


	// Load VDB
	if (!gvdb.getScene()->FindFile("wdas_cloud_eighth.vdb", scnpath)) {
		gprintf("Cannot find vdb file.\n");
		gerror();
	}

	printf("Loading VDB. %s\n", scnpath);
	gvdb.SetChannelDefault(16, 16, 1);
	if (!gvdb.LoadVDB(scnpath)) {                	// Load OpenVDB format			
		gerror();
	}

	gvdb.Measure(true);

	// Set volume params		
	gvdb.SetTransform(Vector3DF(0, 90, 0), Vector3DF(.25, .25, .25), Vector3DF(0, 0, 0), m_translate);
	gvdb.SetEpsilon(0.001, 256);
	gvdb.getScene()->SetSteps(0.1f, 64, 0.2f);			// SCN_PSTEP, SCN_SSTEP, SCN_FSTEP - Raycasting steps
	gvdb.getScene()->SetExtinct(-1.0f, 1.0f, 0.1f);		// SCN_EXTINCT, SCN_ALBEDO - Volume extinction	
	gvdb.getScene()->SetVolumeRange(0.001f, 0.0f, 0.3f);		// Threshold: Isoval, Vmin, Vmax
	gvdb.getScene()->SetCutoff(0.0001f, 0.0001f, 0.0f);		// SCN_MINVAL, SCN_ALPHACUT
	gvdb.getScene()->SetBackgroundClr(0.1f, 0.2f, 0.4f, 1);

	gvdb.getScene()->LinearTransferFunc(0.00f, 1.0f, Vector4DF(0.f, 0.f, 0.f, 0.0f), Vector4DF(1.0f, 1.0f, 1.f, 0.1f));

	gvdb.CommitTransferFunc();

	// Create Camera 
	Camera3D* cam = new Camera3D;
	cam->setFov(30.0);
	cam->setOrbit(Vector3DF(-20, 30, 0), Vector3DF(0, 0, 0), 500, 1.0);
	gvdb.getScene()->SetCamera(cam);

	// Create Light
	Light* lgt = new Light;
	lgt->setOrbit(Vector3DF(45, 45, 0), Vector3DF(0, 0, 0), 200, 1.0);
	gvdb.getScene()->SetLight(0, lgt);

	// Add render buffer
	nvprintf("Creating screen buffer. %d x %d\n", w, h);
	gvdb.AddRenderBuf(0, w, h, 4);

	// Create opengl texture for display
	// This is a helper func in sample utils
	// which creates or resizes an opengl 2D texture.
	createScreenQuadGL(&gl_screen_tex, w, h);

	// Rebuild the Optix scene graph with GVDB
	if (m_render_optix)
		RebuildOptixGraph(m_shading);

	return true;
}

void Sample::reshape(int w, int h)
{
	// Resize the opengl screen texture
	glViewport(0, 0, w, h);
	createScreenQuadGL(&gl_screen_tex, w, h);

	// Resize the GVDB render buffers
	gvdb.ResizeRenderBuf(0, w, h, 4);

	if (m_render_optix)
		optx.ResizeOutput(w, h);

	postRedisplay();
}

void Sample::draw_topology()
{
	Vector3DF clrs[10];
	clrs[0] = Vector3DF(0, 0, 1);			// blue
	clrs[1] = Vector3DF(0, 1, 0);			// green
	clrs[2] = Vector3DF(1, 0, 0);			// red
	clrs[3] = Vector3DF(1, 1, 0);			// yellow
	clrs[4] = Vector3DF(1, 0, 1);			// purple
	clrs[5] = Vector3DF(0, 1, 1);			// aqua
	clrs[6] = Vector3DF(1, 0.5, 0);		// orange
	clrs[7] = Vector3DF(0, 0.5, 1);		// green-blue
	clrs[8] = Vector3DF(0.7f, 0.7f, 0.7f);	// grey

	Camera3D* cam = gvdb.getScene()->getCamera();

	start3D(gvdb.getScene()->getCamera());		// start 3D drawing
	Vector3DF bmin, bmax;
	Node* node;
	Node* node2;

	for (int lev = 0; lev < 5; lev++) {				// draw all levels
		int node_cnt = gvdb.getNumTotalNodes(lev);
		for (int n = 0; n < node_cnt; n++) {			// draw all nodes at this level
			node = gvdb.getNodeAtLevel(n, lev);
			if (!int(node->mFlags)) continue;

			bmin = gvdb.getWorldMin(node);		// get node bounding box
			bmax = gvdb.getWorldMax(node);		// draw node as a box
			drawBox3D(bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z, clrs[lev].x, clrs[lev].y, clrs[lev].z, 1);
		}
	}
	end3D();										// end 3D drawing
}

void Sample::display()
{
	// Update sample convergence
	if (m_render_optix)
		optx.SetSample(frame, sample);

	clearScreenGL();

	if (++sample < max_samples) {
		postRedisplay();
	}
	else {
		++frame;
		sample = 0;
	}

	if (m_render_optix) {

		optx.Render(&gvdb, m_shading, 0);
		optx.ReadOutputTex(gl_screen_tex);

	}
	else {
		gvdb.Render(m_shading, 0, 0);
		gvdb.ReadRenderTexGL(0, gl_screen_tex);
	}

	// Render screen-space quad with texture
	// This is a helper func in sample utils which 
	// renders an opengl 2D texture to the screen.
	renderScreenQuadGL(gl_screen_tex);

	postRedisplay();
}

void Sample::motion(int x, int y, int dx, int dy)
{
	// Get camera for GVDB Scene
	Camera3D* cam = gvdb.getScene()->getCamera();
	bool shift = (getMods() & NVPWindow::KMOD_SHIFT);		// Shift-key to modify light

	switch (mouse_down) {
	case NVPWindow::MOUSE_BUTTON_LEFT: {

		if (shift) {
			// Move volume
			m_translate.x -= dx;
			m_translate.z -= dy;
			gvdb.SetTransform(Vector3DF(0, 0, 0), Vector3DF(.25, .25, .25), Vector3DF(0, 0, 0), m_translate);
		}
		else {
			// Adjust orbit angles
			Vector3DF angs = cam->getAng();
			delta.Set(dx*0.2f, -dy * 0.2f, 0);
			angs += delta;
			cam->setOrbit(angs, cam->getToPos(), cam->getOrbitDist(), cam->getDolly());
		}
		sample = 0;
		postRedisplay();	// Update display
	} break;

	case NVPWindow::MOUSE_BUTTON_MIDDLE: {
		// Adjust target pos		
		cam->moveRelative(float(dx) * cam->getOrbitDist() / 1000, float(-dy) * cam->getOrbitDist() / 1000, 0);
		sample = 0;
		postRedisplay();	// Update display
	} break;

	case NVPWindow::MOUSE_BUTTON_RIGHT: {
		if (shift) {
			// Move volume
			m_translate.y += dy;
			gvdb.SetTransform(Vector3DF(0, 0, 0), Vector3DF(.25, .25, .25), Vector3DF(0, 0, 0), m_translate);
		}
		else {
			// Adjust dist
			float dist = cam->getOrbitDist();
			dist -= dy;
			cam->setOrbit(cam->getAng(), cam->getToPos(), dist, cam->getDolly());
		}
		sample = 0;
		postRedisplay();	// Update display
	} break;
	}
}

void Sample::mouse(NVPWindow::MouseButton button, NVPWindow::ButtonAction state, int mods, int x, int y)
{
	// Track when we are in a mouse drag
	mouse_down = (state == NVPWindow::BUTTON_PRESS) ? button : -1;
}

int sample_main(int argc, const char** argv)
{
	Sample sample_obj;
	return sample_obj.run("NVIDIA(R) GVDB Voxels - gPathTrace", "path_trace", argc, argv, 1024, 768, 4, 5);
}

void sample_print(int argc, char const *argv)
{
}

