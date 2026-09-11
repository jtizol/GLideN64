#include <stdio.h>
#include <cstdlib>
// pinball_cab: GLIDEN64_STREAM_SOCKET support -- see the m_streamFd comment on
// DisplayWindowMupen64plus below. Same technique, same wire protocol, and the same two bugs
// already found and fixed for MAME's own equivalent patch (engine/mame/src/osd/modules/render/
// drawogl.cpp) -- a blocking send() stalling the emulator's own render thread, and a
// non-blocking send() still returning a PARTIAL byte count -- fixed here from the start rather
// than rediscovered, since this is the exact same OS/socket-buffer-ceiling situation, not a
// GLideN64-specific one.
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include <Graphics/Context.h>
#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/opengl_Utils.h>
#include <Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.h>
#include <mupenplus/GLideN64_mupenplus.h>
#include <GLideN64.h>
#include <Config.h>
#include <N64.h>
#include <gSP.h>
#include <Log.h>
#include <Revision.h>
#include <FrameBuffer.h>
#include <GLideNUI/GLideNUI.h>
#include <DisplayWindow.h>

#ifdef VC
#include <bcm_host.h>
#endif

using namespace opengl;

class DisplayWindowMupen64plus : public DisplayWindow
{
public:
	DisplayWindowMupen64plus() {}

private:
	void _setAttributes();
	void _getDisplaySize();

	bool _start() override;
	void _stop() override;
	void _restart() override;
	void _swapBuffers() override;
	void _saveScreenshot() override;
	void _saveBufferContent(graphics::ObjectHandle _fbo, CachedTexture *_pTexture) override;
	bool _resizeWindow() override;
	void _changeWindow() override;
	void _readScreen(void **_pDest, long *_pWidth, long *_pHeight) override {}
	void _readScreen2(void * _dest, int * _width, int * _height, int _front) override;
#ifdef M64P_GLIDENUI
	bool _supportsWithRateFunctions = true;
#endif // M64P_GLIDENUI
	graphics::ObjectHandle _getDefaultFramebuffer() override;

	// pinball_cab: real gameplay video piped into cabinet-dashboard's Shared Playarea, the same
	// in-process-capture approach (and wire protocol) as MAME's own MAME_STREAM_SOCKET patch --
	// see drawogl.cpp's identical member comment and docs/decisions/emulators-and-mame.md. Only
	// mupen64plus_DisplayWindow.cpp (this file, the actual frontend GLideN64 runs under here) is
	// patched -- windows_DisplayWindow.cpp is a different platform entirely and this repo only
	// builds for macOS via the mupen64plus frontend.
	int m_streamFd = -1;
	std::string m_streamPath;          // kept so the connection can be remade
	unsigned int m_streamRetryAt = 0;  // swap counter of the next reconnect attempt
	std::vector<uint8_t> m_streamPixels;   // glReadPixels scratch buffer, GL's bottom-up RGBA
	std::vector<uint8_t> m_streamBuf;      // wire buffer: 8-byte header + row-flipped RGBA

	// pinball_cab: frame-time sampler. A report of "a slight repeatable lag in Mario Golf's shot
	// camera" could not be answered at all -- there was no performance data from a real session
	// anywhere, only guesses about which of several plausible causes it was. This is the cheapest
	// possible fix: we are already in the swap path every frame for the video stream above, so
	// timing it costs one clock read per frame. Percentiles, not an average: a stutter is
	// exactly the tail an average hides.
	// Same `perf ` line shape as the MAME and VPX samplers -- one format, one parser, checked by
	// scripts/check-perf-logging.py.
	std::vector<double> m_perfFrameMs;
	std::chrono::steady_clock::time_point m_perfLast{}, m_perfWindowStart{};
	void _samplePerf();

	// pinball_cab: SPRITES DRAWN INTO THE GAME'S OWN FRAME.
	//
	// The cabinet wants to put its own things on the screen during a game -- whose turn it is,
	// an avatar beside a score -- and the obvious route, a transparent window floating on top,
	// has a hard limit that only shows up in use: a mirror of the GAME's window never contains
	// it. Window capture copies that window's buffer, and an overlay is by definition a
	// different window. It also means a second video path (the streamed copy) running just to
	// have something to draw on, which costs frames on a skill game.
	//
	// Drawing here instead makes the badge part of the picture: it is in the window, in any
	// mirror of it, in the stream, and in a screenshot, with no second copy of anything and no
	// latency at all. Composited BEFORE the stream capture below, deliberately, so the phone and
	// the cabinet see the same frame rather than two versions of it.
	//
	// The dashboard decides what and where (it knows the avatars, the per-game placement in
	// game-data.json, and whose turn it is); this end just blits what it is told, in framebuffer
	// pixels. That split is what makes it reusable: a new overlay for a new game is data, not
	// another C++ patch.
	struct Sprite {
		int id = 0;
		// Placement is FRACTIONS of the framebuffer, y from the top -- not pixels. The dashboard
		// cannot know this buffer's pixel size (a Retina drawable is twice its window's points,
		// and the stream it otherwise sees is downscaled to a fixed 960x540), so pixels would be
		// right on one display and wrong on another. Fractions need no negotiation at all.
		float fx = 0.f, fy = 0.f, fw = 0.f, fh = 0.f;
		int pxw = 0, pxh = 0;                 // the IMAGE's own size, to read the file
		std::vector<uint8_t> rgba;            // pxw*pxh*4, straight alpha
		bool dirty = true;                    // needs (re)upload to its texture
		unsigned int tex = 0;
	};
	std::vector<Sprite> m_sprites;
	std::string m_cmdBuf;                     // partial command line from the socket
	unsigned int m_spriteProgram = 0, m_spriteVao = 0, m_spriteVbo = 0;
	bool m_spriteInitFailed = false;
	void _pollSpriteCommands();
	void _drawSprites();
	void _connectStream();
};

DisplayWindow & DisplayWindow::get()
{
	static DisplayWindowMupen64plus video;
	return video;
}

// pinball_cab: see the m_perfFrameMs comment in the class declaration for why this exists.
// Every PERF_WINDOW_S it prints one line and starts over; the vector never grows past a window.
// fprintf + fflush, not std::cout: this has to survive a `pkill -9` (the cabinet's normal way of
// stopping a game), which is also why scripts/logtee.py hands us a PTY.
void DisplayWindowMupen64plus::_samplePerf()
{
	constexpr double PERF_WINDOW_S = 10.0;
	const auto now = std::chrono::steady_clock::now();
	if (m_perfLast.time_since_epoch().count() != 0) {
		m_perfFrameMs.push_back(std::chrono::duration<double, std::milli>(now - m_perfLast).count());
	} else {
		m_perfWindowStart = now;
	}
	m_perfLast = now;

	const double windowS = std::chrono::duration<double>(now - m_perfWindowStart).count();
	if (windowS < PERF_WINDOW_S || m_perfFrameMs.size() < 2)
		return;

	std::vector<double> sorted = m_perfFrameMs;
	std::sort(sorted.begin(), sorted.end());
	const auto pct = [&sorted](double p) {
		const size_t i = std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()));
		return sorted[i];
	};
	std::printf("perf fps=%.1f ms_p50=%.1f ms_p95=%.1f ms_max=%.1f n=%zu window_s=%.1f\n",
		static_cast<double>(sorted.size()) / windowS, pct(0.50), pct(0.95), sorted.back(),
		sorted.size(), windowS);
	std::fflush(stdout);
	m_perfFrameMs.clear();
	m_perfWindowStart = now;
}

// pinball_cab: the sprite compositor. See the Sprite member comment for why this draws into the
// game's frame rather than a window over it.
//
// GL 3.3 core (this file's own _setAttributes asks for it), so there is no fixed-function path:
// one tiny program, one VAO, one dynamic VBO, and a texture per sprite. Every GL call goes
// through GLideN64's own wrappers (GLFunctions.h macros) exactly like the rest of this file --
// the threaded-GL wrapper is not optional, and calling raw gl* here would work until someone
// enabled it.
//
// STATE IS SAVED AND RESTORED around the draw. GLideN64 is mid-pipeline when this runs and does
// not expect anyone else to have touched the context; leaving blending on (or a program bound)
// would corrupt the NEXT frame in ways that look like a renderer bug, not an overlay bug.
static const char *kSpriteVs =
	"#version 330 core\n"
	"layout(location=0) in vec2 aPos;\n"
	"layout(location=1) in vec2 aUv;\n"
	"out vec2 vUv;\n"
	"void main(){ vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *kSpriteFs =
	"#version 330 core\n"
	"in vec2 vUv;\n"
	"uniform sampler2D uTex;\n"
	"out vec4 fragColor;\n"
	"void main(){ fragColor = texture(uTex, vUv); }\n";

static unsigned int _compile(unsigned int type, const char *src)
{
	unsigned int sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, nullptr);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = {0};
		glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
		std::printf("sprite shader failed: %s\n", log);
		std::fflush(stdout);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

void DisplayWindowMupen64plus::_drawSprites()
{
	if (m_sprites.empty() || m_spriteInitFailed)
		return;

	if (m_spriteProgram == 0) {
		unsigned int vs = _compile(GL_VERTEX_SHADER, kSpriteVs);
		unsigned int fs = _compile(GL_FRAGMENT_SHADER, kSpriteFs);
		if (vs == 0 || fs == 0) { m_spriteInitFailed = true; return; }
		m_spriteProgram = glCreateProgram();
		glAttachShader(m_spriteProgram, vs);
		glAttachShader(m_spriteProgram, fs);
		glLinkProgram(m_spriteProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = 0;
		glGetProgramiv(m_spriteProgram, GL_LINK_STATUS, &ok);
		if (!ok) { m_spriteInitFailed = true; return; }
		glGenVertexArrays(1, &m_spriteVao);
		glGenBuffers(1, &m_spriteVbo);
		glBindVertexArray(m_spriteVao);
		glBindBuffer(GL_ARRAY_BUFFER, m_spriteVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (const GLvoid *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
			(const GLvoid *)(sizeof(float) * 2));
		glBindVertexArray(0);
	}

	// EVERY piece of state this touches, saved so it can be put back exactly.
	//
	// The first version of this saved four things and looked fine. It was corrupting the GAME's
	// rendering: the black boxes that appeared behind Mario Golf's white text were our
	// glBlendFunc left set, and the badge itself came out as a CRESCENT on screens where
	// GLideN64 had left a scissor box smaller than the window. Neither failure points at this
	// function -- they look like the emulator being broken -- so the rule here is to save
	// anything touched, not anything believed to matter.
	//
	// GL_ARRAY_BUFFER is the subtle one: it is NOT part of vertex-array-object state, so
	// restoring the VAO does not restore it, and GLideN64's next glBufferSubData would have
	// landed in our vertex buffer.
	GLboolean wasBlend = glIsEnabled(GL_BLEND);
	GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
	GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean wasCull = glIsEnabled(GL_CULL_FACE);
	GLint prevProgram = 0, prevVao = 0, prevTex = 0, prevFbo = 0, prevArrayBuf = 0;
	GLint prevActiveTex = GL_TEXTURE0, prevUnpack = 4;
	GLint prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
	GLint prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
	GLint prevViewport[4] = { 0, 0, 0, 0 };
	GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
	glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
	glGetIntegerv(GL_VIEWPORT, prevViewport);
	glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);

	// The DEFAULT framebuffer, not whatever GLideN64 last bound: the badge belongs on the image
	// about to be shown, not on one of the renderer's intermediate targets.
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	// The same rectangle the frame stream reads back, so a fraction means the same thing to the
	// dashboard, to the captured still, and to the cabinet's screen. Inheriting whatever
	// viewport the renderer happened to leave set made placement depend on the last thing the
	// GAME drew.
	if (m_screenWidth > 0 && m_screenHeight > 0)
		glViewport(0, m_heightOffset, m_screenWidth, m_screenHeight);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glActiveTexture(GL_TEXTURE0);
	glUseProgram(m_spriteProgram);
	glBindVertexArray(m_spriteVao);
	glBindBuffer(GL_ARRAY_BUFFER, m_spriteVbo);

	for (Sprite &sp : m_sprites) {
		if (sp.pxw <= 0 || sp.pxh <= 0 || sp.rgba.size() < (size_t)sp.pxw * sp.pxh * 4)
			continue;
		if (sp.tex == 0)
			glGenTextures(1, &sp.tex);
		glBindTexture(GL_TEXTURE_2D, sp.tex);
		if (sp.dirty) {
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sp.pxw, sp.pxh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
				sp.rgba.data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			sp.dirty = false;
		}

		// Fractions (y from the top, as every caller thinks of a screen) -> clip space.
		const float x0 = sp.fx * 2.f - 1.f;
		const float x1 = (sp.fx + sp.fw) * 2.f - 1.f;
		const float y0 = 1.f - sp.fy * 2.f;
		const float y1 = 1.f - (sp.fy + sp.fh) * 2.f;
		const float verts[24] = {
			x0, y0, 0.f, 0.f,   x1, y0, 1.f, 0.f,   x1, y1, 1.f, 1.f,
			x0, y0, 0.f, 0.f,   x1, y1, 1.f, 1.f,   x0, y1, 0.f, 1.f,
		};
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	// Put it all back, in the reverse of the order it was taken.
	glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
	glBindTexture(GL_TEXTURE_2D, prevTex);
	glActiveTexture((GLenum)prevActiveTex);
	glBindBuffer(GL_ARRAY_BUFFER, prevArrayBuf);
	glBindVertexArray(prevVao);
	glUseProgram(prevProgram);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevFbo);
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
	glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
	glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
	if (!wasBlend) glDisable(GL_BLEND);
	if (wasDepth) glEnable(GL_DEPTH_TEST);
	if (wasScissor) glEnable(GL_SCISSOR_TEST);
	if (wasCull) glEnable(GL_CULL_FACE);
}

// Commands from the dashboard, on the SAME socket the frames go out on -- it is already
// connected, already non-blocking, and already touched every frame. See state_tap.c, which made
// the same call for the same reasons.
//
//   sprite <id> <fx> <fy> <fw> <fh> <pxw> <pxh> <path>
//       fx..fh  placement, as fractions of the framebuffer (y from the top)
//       pxw/pxh the image's own pixel size
//       path    a raw RGBA file, pxw*pxh*4 bytes
//   clear <id>
//   clear-all
void DisplayWindowMupen64plus::_pollSpriteCommands()
{
	if (m_streamFd < 0)
		return;
	for (;;) {
		char chunk[512];
		ssize_t n = ::recv(m_streamFd, chunk, sizeof(chunk), MSG_DONTWAIT);
		if (n <= 0)
			return;                  // EAGAIN is the normal case: nobody sends most frames
		m_cmdBuf.append(chunk, (size_t)n);
		for (;;) {
			const size_t nl = m_cmdBuf.find('\n');
			if (nl == std::string::npos) {
				// A caller that never sends a newline must not grow this without bound.
				if (m_cmdBuf.size() > 4096)
					m_cmdBuf.clear();
				break;
			}
			const std::string line = m_cmdBuf.substr(0, nl);
			m_cmdBuf.erase(0, nl + 1);
			int id = 0, pxw = 0, pxh = 0;
			float fx = 0.f, fy = 0.f, fw = 0.f, fh = 0.f;
			char path[512] = {0};
			if (std::sscanf(line.c_str(), "sprite %d %f %f %f %f %d %d %511s",
					&id, &fx, &fy, &fw, &fh, &pxw, &pxh, path) == 8) {
				std::vector<uint8_t> rgba((size_t)pxw * pxh * 4);
				FILE *f = std::fopen(path, "rb");
				if (f == nullptr) {
					std::printf("sprite %d: cannot open %s\n", id, path);
					std::fflush(stdout);
					continue;
				}
				const size_t got = std::fread(rgba.data(), 1, rgba.size(), f);
				std::fclose(f);
				if (got != rgba.size()) {
					std::printf("sprite %d: short read (%zu of %zu) from %s\n",
						id, got, rgba.size(), path);
					std::fflush(stdout);
					continue;        // a half-written file is never a sprite
				}
				std::printf("sprite %d accepted %dx%d at %.3f,%.3f %.3fx%.3f\n",
					id, pxw, pxh, fx, fy, fw, fh);
				std::fflush(stdout);
				Sprite *found = nullptr;
				for (Sprite &sp : m_sprites)
					if (sp.id == id) { found = &sp; break; }
				if (found == nullptr) {
					m_sprites.push_back(Sprite());
					found = &m_sprites.back();
					found->id = id;
				}
				found->fx = fx; found->fy = fy; found->fw = fw; found->fh = fh;
				found->pxw = pxw; found->pxh = pxh;
				found->rgba.swap(rgba);
				found->dirty = true;
			} else if (std::sscanf(line.c_str(), "clear %d", &id) == 1) {
				for (size_t i = 0; i < m_sprites.size(); i++) {
					if (m_sprites[i].id == id) {
						if (m_sprites[i].tex != 0)
							glDeleteTextures(1, &m_sprites[i].tex);
						m_sprites.erase(m_sprites.begin() + i);
						break;
					}
				}
			} else if (line.rfind("clear-all", 0) == 0) {
				for (Sprite &sp : m_sprites)
					if (sp.tex != 0) glDeleteTextures(1, &sp.tex);
				m_sprites.clear();
			}
		}
	}
}

void DisplayWindowMupen64plus::_setAttributes()
{
	LOG(LOG_VERBOSE, "_setAttributes");

	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_CORE);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MAJOR_VERSION, 3);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_CONTEXT_MINOR_VERSION, 3);

	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_DOUBLEBUFFER, 1);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_SWAP_CONTROL, config.video.verticalSync);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_BUFFER_SIZE, 32);
	FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_DEPTH_SIZE, 16);
	if (config.video.multisampling > 0 && config.frameBufferEmulation.enable == 0) {
		FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLEBUFFERS, 1);
		if (config.video.multisampling <= 2)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 2);
		else if (config.video.multisampling <= 4)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 4);
		else if (config.video.multisampling <= 8)
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 8);
		else
			FunctionWrapper::CoreVideo_GL_SetAttribute(M64P_GL_MULTISAMPLESAMPLES, 16);
	}
}

bool DisplayWindowMupen64plus::_start()
{
	FunctionWrapper::setThreadedMode(config.video.threadedVideo);
	auto returnValue = FunctionWrapper::CoreVideo_Init();
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "Error in CoreVideo_Init. Error code: %d", returnValue);
		FunctionWrapper::CoreVideo_Quit();
		return false;
	}

	_setAttributes();

	m_bFullscreen = config.video.fullscreen > 0;
	m_screenWidth = config.video.windowedWidth;
	m_screenHeight = config.video.windowedHeight;
	m_screenRefresh = config.video.fullscreenRefresh;

	_getDisplaySize();
	_setBufferSize();

	LOG(LOG_VERBOSE, "Setting video mode %dx%d", m_screenWidth, m_screenHeight);
	const m64p_video_flags flags = M64VIDEOFLAG_SUPPORT_RESIZING;
#ifdef M64P_GLIDENUI
	returnValue = FunctionWrapper::CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
	if (returnValue != M64ERR_SUCCESS)
	{
		_supportsWithRateFunctions = false;
#endif // M64P_GLIDENUI
		returnValue = FunctionWrapper::CoreVideo_SetVideoMode(m_screenWidth, m_screenHeight, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);
#ifdef M64P_GLIDENUI
	}
#endif // M64P_GLIDENUI
	if (returnValue != M64ERR_SUCCESS) {
		LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
		FunctionWrapper::CoreVideo_Quit();
		return false;
	}

	// pinball_cab: connect to the dashboard's frame-receiving socket if launch-n64.py set one
	// up for us -- see the m_streamFd comment in the class declaration above. Connected once
	// per launch here (video-system startup), not in the trivial default constructor, mirroring
	// this same lifecycle point already being where CoreVideo_Init/_setAttributes happen.
	if (const char *sockPath = std::getenv("GLIDEN64_STREAM_SOCKET"))
		m_streamPath = sockPath;
	_connectStream();

	char caption[128];
#ifdef PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug. Revision %s", pluginName, PLUGIN_REVISION);
# else // _DEBUG
	sprintf(caption, "%s. Revision %s", pluginName, PLUGIN_REVISION);
# endif // _DEBUG
#else // PLUGIN_REVISION
# ifdef _DEBUG
	sprintf(caption, "%s debug", pluginName);
# else // _DEBUG
	sprintf(caption, "%s", pluginName);
# endif // _DEBUG
#endif // PLUGIN_REVISION
	CoreVideo_SetCaption(caption);

	return true;
}

// pinball_cab: (re)connect to the dashboard's frame socket.
//
// This used to happen exactly once, at video-system startup. The dashboard is restarted often --
// a code change, a crash, a port already in use -- and a single connect meant a running game
// lost its overlay and its frame stream for the rest of the session every time, with no symptom
// pointing at the cause. Retried from the swap path instead, roughly every two seconds.
void DisplayWindowMupen64plus::_connectStream()
{
	if (m_streamFd >= 0 || m_streamPath.empty())
		return;

	int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, m_streamPath.c_str(), sizeof(addr.sun_path) - 1);
	if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
		::close(fd);
		return;                 // deliberately silent: this runs every ~2s while nobody listens
	}

	// See MAME's drawogl.cpp for the identical comment: without this, a send() after the
	// dashboard's reader has gone away raises SIGPIPE, whose default disposition kills the
	// whole process.
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

	// Non-blocking, same reasoning as MAME's own patch: this render thread IS mupen64plus's
	// emulation thread too, so a blocking send() here would stall gameplay itself.
	int flags = ::fcntl(fd, F_GETFL, 0);
	::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	int sndbuf = 8 * 1024 * 1024;
	::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	m_streamFd = fd;
	// Sprites live on the dashboard's side of this socket; a new listener has no idea what was
	// on screen, so drop what we are drawing and let it tell us again.
	m_sprites.clear();
	m_cmdBuf.clear();
	printf("gliden64: frame stream (re)connected to %s\n", m_streamPath.c_str());
	fflush(stdout);
}

void DisplayWindowMupen64plus::_stop()
{
	if (m_streamFd >= 0)
	{
		::close(m_streamFd);
		m_streamFd = -1;
	}
	FunctionWrapper::CoreVideo_Quit();
}

void DisplayWindowMupen64plus::_restart()
{
#ifdef M64P_GLIDENUI
	m_resizeWidth = 0;
	m_resizeHeight = 0;
#endif // M64P_GLIDENUI
}

void DisplayWindowMupen64plus::_swapBuffers()
{
	// if emulator defined a render callback function, call it before buffer swap
	if (renderCallback != nullptr) {
		gfxContext.resetShaderProgram();
		if (config.frameBufferEmulation.N64DepthCompare == Config::dcDisable) {
			gfxContext.setViewport(0, getHeightOffset(), getScreenWidth(), getScreenHeight());
			gSP.changed |= CHANGED_VIEWPORT;
		}
		gDP.changed |= CHANGED_COMBINE;
		(*renderCallback)((gDP.changed&CHANGED_CPU_FB_WRITE) == 0 ? 1 : 0);
	}

	// pinball_cab: capture the completed frame for the dashboard BEFORE swapping -- GL_BACK
	// still holds exactly what was just drawn at this point, same reasoning as MAME's identical
	// capture point in drawogl.cpp::draw(). Reads from m_heightOffset, not row 0, matching
	// _readScreen2()'s own existing capture above -- some titles render into a sub-rect of the
	// window (letterboxing), and starting at row 0 would capture the wrong rows/black bars for
	// those. See docs/decisions/emulators-and-mame.md for the wire protocol and the two bugs
	// (blocking send stalls the emulator; a non-blocking send can still be PARTIAL) this
	// duplicates the fix for rather than rediscovering.
	_samplePerf();

	// Reconnect if the dashboard went away and came back. Every ~2s at 60fps: cheap enough that
	// a connect() against a missing socket costs nothing, frequent enough that a server restart
	// costs a couple of seconds of overlay rather than the rest of the session.
	if (m_streamFd < 0 && !m_streamPath.empty() && ++m_streamRetryAt >= 120) {
		m_streamRetryAt = 0;
		_connectStream();
	}

	// Commands first, then draw, then capture -- so a sprite that arrived this frame is on the
	// picture the stream sends, not one frame behind it.
	_pollSpriteCommands();
	_drawSprites();

	if (m_streamFd >= 0 && m_screenWidth > 0 && m_screenHeight > 0)
	{
		constexpr int STREAM_OUT_W = 480;
		constexpr int STREAM_OUT_H = 270;

		const size_t srcFrameBytes = size_t(m_screenWidth) * size_t(m_screenHeight) * 4;
		m_streamPixels.resize(srcFrameBytes);
		glPixelStorei(GL_PACK_ALIGNMENT, 1); // RGBA is already 4-byte aligned per pixel, but
		                                      // matches _readScreen2()'s own explicit set rather
		                                      // than relying on whatever the driver defaulted to.
		glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGBA, GL_UNSIGNED_BYTE, m_streamPixels.data());

		const size_t outFrameBytes = size_t(STREAM_OUT_W) * size_t(STREAM_OUT_H) * 4;
		m_streamBuf.resize(8 + outFrameBytes);
		m_streamBuf[0] = uint8_t(STREAM_OUT_W >> 24); m_streamBuf[1] = uint8_t(STREAM_OUT_W >> 16);
		m_streamBuf[2] = uint8_t(STREAM_OUT_W >> 8);  m_streamBuf[3] = uint8_t(STREAM_OUT_W);
		m_streamBuf[4] = uint8_t(STREAM_OUT_H >> 24); m_streamBuf[5] = uint8_t(STREAM_OUT_H >> 16);
		m_streamBuf[6] = uint8_t(STREAM_OUT_H >> 8);  m_streamBuf[7] = uint8_t(STREAM_OUT_H);
		const size_t srcRowBytes = size_t(m_screenWidth) * 4;
		uint8_t *dst = m_streamBuf.data() + 8;
		for (int dy = 0; dy < STREAM_OUT_H; dy++)
		{
			// (STREAM_OUT_H - 1 - dy): output row 0 is the TOP of the image; glReadPixels' row 0
			// is the BOTTOM (OpenGL convention) -- this both flips and downscales in one pass,
			// same technique as MAME's drawogl.cpp.
			const unsigned int srcY = std::min(m_screenHeight - 1, (unsigned int)(((STREAM_OUT_H - 1 - dy) * (long long)m_screenHeight) / STREAM_OUT_H));
			const uint8_t *srcRow = m_streamPixels.data() + srcRowBytes * size_t(srcY);
			for (int dx = 0; dx < STREAM_OUT_W; dx++)
			{
				const unsigned int srcX = std::min(m_screenWidth - 1, (unsigned int)((dx * (long long)m_screenWidth) / STREAM_OUT_W));
				std::memcpy(dst, srcRow + size_t(srcX) * 4, 4);
				dst += 4;
			}
		}

		// ONE non-blocking send attempt, all-or-nothing -- same reasoning as MAME's drawogl.cpp:
		// a full send loop would block this render thread (mupen64plus's emulation thread) until
		// the kernel socket buffer drains, and a "successful" non-blocking send can still return
		// a PARTIAL byte count, which would silently corrupt this wire format's framing forever
		// after (no mid-frame resync marker). Anything other than a complete send is treated as
		// fatal to this connection -- close it, don't try to resume mid-frame.
		ssize_t n = ::send(m_streamFd, m_streamBuf.data(), m_streamBuf.size(), MSG_DONTWAIT);
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		{
			::close(m_streamFd);
			m_streamFd = -1;
		}
		else if (n > 0 && size_t(n) != m_streamBuf.size())
		{
			::close(m_streamFd);
			m_streamFd = -1;
		}
	}

	//Don't let the command queue grow too big buy waiting on no more swap buffers being queued
	FunctionWrapper::WaitForSwapBuffersQueued();

	FunctionWrapper::CoreVideo_GL_SwapBuffers();
}

void DisplayWindowMupen64plus::_saveScreenshot()
{
}

void DisplayWindowMupen64plus::_saveBufferContent(graphics::ObjectHandle /*_fbo*/, CachedTexture* /*_pTexture*/)
{
}

bool DisplayWindowMupen64plus::_resizeWindow()
{
#ifdef M64P_GLIDENUI
	if (m_resizeWidth == 0 && m_resizeHeight == 0) {
		return true;
	}
#endif // M64P_GLIDENUI

	_setAttributes();

	m_width = m_screenWidth = m_resizeWidth;
	m_height = m_screenHeight = m_resizeHeight;

	_setBufferSize();
	opengl::Utils::isGLError(); // reset GL error.
	return true;
}

void DisplayWindowMupen64plus::_changeWindow()
{
#ifdef M64P_GLIDENUI
	if (_supportsWithRateFunctions) {
		m64p_error returnValue;
		m_bFullscreen = !m_bFullscreen;
		if (m_bFullscreen) {
			m_screenWidth = config.video.fullscreenWidth;
			m_screenHeight = config.video.fullscreenHeight;
			m_screenRefresh = config.video.fullscreenRefresh;
		} else {
			m_screenWidth = config.video.windowedWidth;
			m_screenHeight = config.video.windowedHeight;
		}

		m64p_video_flags flags = {};
		returnValue = FunctionWrapper::CoreVideo_SetVideoModeWithRate(m_screenWidth, m_screenHeight, m_screenRefresh, 0, m_bFullscreen ? M64VIDEO_FULLSCREEN : M64VIDEO_WINDOWED, flags);

		if (returnValue != M64ERR_SUCCESS) {
			LOG(LOG_ERROR, "Error setting videomode %dx%d @ %d. Error code: %d", m_screenWidth, m_screenHeight, m_screenRefresh, returnValue);
			FunctionWrapper::CoreVideo_Quit();
		}
	} else {
#endif // M64P_GLIDENUI
		CoreVideo_ToggleFullScreen();
#ifdef M64P_GLIDENUI
	}
#endif // M64P_GLIDENUI
}

void DisplayWindowMupen64plus::_getDisplaySize()
{
#ifdef VC
	if( m_bFullscreen ) {
		// Use VC get_display_size function to get the current screen resolution
		u32 fb_width;
		u32 fb_height;
		auto returnValue = graphics_get_display_size(0 /* LCD */, &fb_width, &fb_height);
		if (returnValue < 0)
			LOG(LOG_ERROR, "Failed to get display size. Error code: %d", returnValue);
		else {
			LOG(LOG_VERBOSE, "Display size %dx%d", fb_width, fb_height);
			m_screenWidth = fb_width;
			m_screenHeight = fb_height;
		}
	}
#endif
}

void DisplayWindowMupen64plus::_readScreen2(void * _dest, int * _width, int * _height, int _front)
{
	if (_width == nullptr || _height == nullptr)
		return;

	*_width = m_screenWidth;
	*_height = m_screenHeight;

	if (_dest == nullptr)
		return;

	// GL_PACK_ALIGNMENT defaults to 4, so with the 3 byte format below the
	// driver pads every row up to a 4 byte boundary and writes past the
	// width*height*3 buffer whenever width*3 is not a multiple of 4. _dest is
	// owned by the emulator core, so the overrun lands in its memory.
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

#if !defined(OS_ANDROID) && !defined(OS_IOS)
	GLint oldMode;
	glGetIntegerv(GL_READ_BUFFER, &oldMode);
	gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
	if (_front != 0)
		glReadBuffer(GL_FRONT);
	else
		glReadBuffer(GL_BACK);
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGB, GL_UNSIGNED_BYTE, _dest);
	if (graphics::BufferAttachmentParam(oldMode) == graphics::bufferAttachment::COLOR_ATTACHMENT0) {
		FrameBuffer * pBuffer = frameBufferList().getCurrent();
		if (pBuffer != nullptr)
			gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, pBuffer->m_FBO);
	}
	glReadBuffer(oldMode);
#else
	u8 *pBufferData = (u8*)malloc((*_width)*(*_height) * 4);
	if (pBufferData == nullptr)
		return;
	u8 *pDest = (u8*)_dest;
	glReadPixels(0, m_heightOffset, m_screenWidth, m_screenHeight, GL_RGBA, GL_UNSIGNED_BYTE, pBufferData);

	//Convert RGBA to RGB
	for (s32 y = 0; y < *_height; ++y) {
		u8 *ptr = pBufferData + ((*_width) * 4 * y);
		for (s32 x = 0; x < *_width; ++x) {
			pDest[x * 3] = ptr[0]; // red
			pDest[x * 3 + 1] = ptr[1]; // green
			pDest[x * 3 + 2] = ptr[2]; // blue
			ptr += 4;
		}
		pDest += (*_width) * 3;
	}

	free(pBufferData);
#endif
}

graphics::ObjectHandle DisplayWindowMupen64plus::_getDefaultFramebuffer()
{
	if (CoreVideo_GL_GetDefaultFramebuffer != nullptr)
		return graphics::ObjectHandle(CoreVideo_GL_GetDefaultFramebuffer());
	return graphics::ObjectHandle::null;
}
