#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <chrono>
#include <thread>
#ifdef GEODE_IS_ANDROID
#include <EGL/egl.h>
#endif

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

static bool g_enabled = true;
static int g_fps = 240;
// True while you are inside a level. The mod only changes FPS there.
static bool g_inLevel = false;
// The frame interval GD itself asked for, restored when the mod is off.
static double g_gameInterval = 1.0 / 60.0;
static Clock::time_point g_nextFrame{};

static bool active() {
	return g_enabled && g_inLevel && g_fps > 0;
}

static void applyFPS() {
	auto mod = Mod::get();
	g_enabled = mod->getSettingValue<bool>("enabled");
	g_fps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));
	g_nextFrame = Clock::time_point{};

	// Goes through the hook below, which swaps in the target inside levels.
	CCDirector::get()->setAnimationInterval(g_gameInterval);

#ifdef GEODE_IS_ANDROID
	// With VSync on, Android holds frames to the screen's refresh rate
	// (90 on your phone). Turning it off lets GD run at the target instead.
	EGLDisplay display = eglGetCurrentDisplay();
	if (display != EGL_NO_DISPLAY) {
		bool off = active() && mod->getSettingValue<bool>("disable-vsync");
		eglSwapInterval(display, off ? 0 : 1);
	}
#endif
}

// Frame limiter: holds every frame to exactly 1/target seconds, so GD's own
// FPS counter stays at the target instead of jumping above it.
static void limitFrame() {
	if (!active()) return;
	auto const interval = std::chrono::duration_cast<Clock::duration>(
		std::chrono::duration<double>(1.0 / g_fps));
	auto now = Clock::now();

	// First frame, or fell far behind (loading, lag spike): start fresh
	// instead of rushing frames to catch up.
	if (g_nextFrame == Clock::time_point{} || now > g_nextFrame + interval * 2) {
		g_nextFrame = now + interval;
		return;
	}
	if (now < g_nextFrame) {
		// Sleep most of the wait (saves battery), then wait out the last
		// fraction of a millisecond precisely.
		auto const spinWindow = std::chrono::microseconds(500);
		if (g_nextFrame - now > spinWindow) {
			std::this_thread::sleep_for(g_nextFrame - now - spinWindow);
		}
		while (Clock::now() < g_nextFrame) {
			std::this_thread::yield();
		}
	}
	g_nextFrame += interval;
}

class $modify(ForceFPSDirector, CCDirector) {
	// GD (and Android, when you leave and come back to the app) can reset
	// the frame interval. Remember what it wanted, but keep the target.
	void setAnimationInterval(double interval) {
		if (interval > 0.0) g_gameInterval = interval;
		if (active()) interval = 1.0 / g_fps;
		CCDirector::setAnimationInterval(interval);
	}

	void drawScene() {
		// Switch on when a level starts and back to normal when you leave it.
		bool const inLevel = PlayLayer::get() != nullptr;
		if (inLevel != g_inLevel) {
			g_inLevel = inLevel;
			applyFPS();
		}
		limitFrame();
		CCDirector::drawScene();
	}
};

// Texture quality: GD picks this automatically on phones and has no menu
// option for it. Runs right before GD loads its textures, so changing the
// setting needs a restart of GD.
class $modify(ForceFPSLoadingLayer, LoadingLayer) {
	bool init(bool refresh) {
		auto choice = Mod::get()->getSettingValue<std::string>("texture-quality");
		TextureQuality target = kTextureQualityHigh;
		if (choice == "Medium") target = kTextureQualityMedium;
		else if (choice == "Low") target = kTextureQualityLow;

		auto director = CCDirector::get();
		// Only ever lowers quality, never raises it past what GD picked.
		if (target < director->getLoadedTextureQuality()) {
			GameManager::get()->m_texQuality = static_cast<int>(target);
			director->updateContentScale(target);
			log::info("Force FPS: texture quality set to {}", choice);
		}
		return LoadingLayer::init(refresh);
	}
};

$on_mod(Loaded) {
	Loader::get()->queueInMainThread([] {
		double current = CCDirector::get()->getAnimationInterval();
		if (current > 0.0) g_gameInterval = current;
		applyFPS();
	});
	listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
	listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("disable-vsync", [](bool) { applyFPS(); });
}
