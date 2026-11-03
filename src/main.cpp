#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <algorithm>
#include <cmath>
#ifdef GEODE_IS_ANDROID
#include <EGL/egl.h>
#endif

using namespace geode::prelude;

static bool g_enabled = true;
static bool g_substep = true;
static int g_fps = 240;

static void applyFPS() {
	auto mod = Mod::get();
	g_enabled = mod->getSettingValue<bool>("enabled");
	g_substep = mod->getSettingValue<bool>("substep");
	g_fps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));

	double fps = g_enabled ? g_fps : 60.0;
	CCDirector::get()->setAnimationInterval(1.0 / fps);

#ifdef GEODE_IS_ANDROID
	// Ask the driver not to block on the screen's refresh (VSync).
	// Must run on the GL thread; many drivers ignore it.
	EGLDisplay display = eglGetCurrentDisplay();
	if (display != EGL_NO_DISPLAY) {
		bool off = g_enabled && mod->getSettingValue<bool>("disable-vsync");
		eglSwapInterval(display, off ? 0 : 1);
	}
#endif
	log::info("Force FPS: target {} (substep: {})", fps, g_substep);
}

// Splits each screen frame into several game updates, so the game loop
// runs at the target FPS even when the screen caps how often frames happen.
// Example: 90 Hz screen, target 240 -> each frame (~11.1 ms) runs ~3 updates.
class $modify(ForceFPSScheduler, CCScheduler) {
	void update(float dt) {
		if (!g_enabled || !g_substep || g_fps <= 0 || dt <= 0.f) {
			CCScheduler::update(dt);
			return;
		}
		int steps = static_cast<int>(std::round(dt * g_fps));
		steps = std::clamp(steps, 1, 16); // cap to avoid a death spiral on lag spikes
		float sub = dt / steps;
		for (int i = 0; i < steps; i++) {
			CCScheduler::update(sub);
		}
	}
};

$on_mod(Loaded) {
	Loader::get()->queueInMainThread([] { applyFPS(); });
	listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
	listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("substep", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("disable-vsync", [](bool) { applyFPS(); });
}
