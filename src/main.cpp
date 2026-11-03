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
static double g_accum = 0.0;

static void applyFPS() {
	g_accum = 0.0;
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

// Fixed-timestep game loop: time from each screen frame goes into an
// accumulator, and the game is updated in exact 1/FPS slices. This keeps the
// update rate steady at the target (e.g. 240) instead of jumping around with
// small changes in frame timing.
class $modify(ForceFPSScheduler, CCScheduler) {
	void update(float dt) {
		if (!g_enabled || !g_substep || g_fps <= 0 || dt <= 0.f) {
			g_accum = 0.0;
			CCScheduler::update(dt);
			return;
		}
		double const step = 1.0 / g_fps;
		g_accum += dt;

		int steps = static_cast<int>(g_accum / step);
		if (steps <= 0) {
			// target is below the screen rate: just pass time through
			CCScheduler::update(static_cast<float>(g_accum));
			g_accum = 0.0;
			return;
		}
		constexpr int kMaxSteps = 12; // stops lag spikes from snowballing
		if (steps > kMaxSteps) {
			steps = kMaxSteps;
			g_accum = steps * step; // drop the backlog instead of catching up
		}
		for (int i = 0; i < steps; i++) {
			CCScheduler::update(static_cast<float>(step));
		}
		g_accum -= steps * step;
	}
};

$on_mod(Loaded) {
	Loader::get()->queueInMainThread([] { applyFPS(); });
	listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFPS(); });
	listenForSettingChanges<bool>("enabled", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("substep", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("disable-vsync", [](bool) { applyFPS(); });
}
