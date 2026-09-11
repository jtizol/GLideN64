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
	{
		m_streamFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (m_streamFd >= 0)
		{
			struct sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
			if (::connect(m_streamFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0)
			{
				::close(m_streamFd);
				m_streamFd = -1;
			}
			else
			{
				// See MAME's drawogl.cpp for the identical comment: without this, a send() after
				// the dashboard's reader has gone away raises SIGPIPE, whose default disposition
				// kills the whole process.
				int one = 1;
				::setsockopt(m_streamFd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

				// Non-blocking, same reasoning as MAME's own patch: this render thread IS
				// mupen64plus's emulation thread too (renderCallback runs from inside
				// _swapBuffers, same call site the frame capture below sits next to), so a
				// blocking send() here would stall gameplay itself, not just the video path.
				int flags = ::fcntl(m_streamFd, F_GETFL, 0);
				::fcntl(m_streamFd, F_SETFL, flags | O_NONBLOCK);

				// Best-effort send buffer, same sizing/reasoning as MAME's patch.
				int sndbuf = 8 * 1024 * 1024;
				::setsockopt(m_streamFd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
			}
		}
	}

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
