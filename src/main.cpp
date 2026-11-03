#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#ifdef GEODE_IS_ANDROID
#include <EGL/egl.h>
#endif

using namespace geode::prelude;

static bool g_enabled = true;
static bool g_substep = true;
static bool g_inLevels = false;
static int g_fps = 240;
static double g_accum = 0.0;
// Seconds left to run without extra updates after the phone fell behind.
static double g_cooldown = 0.0;
// The frame interval GD itself asked for, restored when the mod is off.
static double g_gameInterval = 1.0 / 60.0;

// Accurate counter: averages over half a second so it doesn't twitch.
// FPS = frames your screen shows, TPS = game updates per second.
static bool g_showCounter = true;
static CCLabelBMFont* g_label = nullptr;
static double g_counterTime = 0.0;
static int g_frames = 0;
static int g_ticks = 0;

static void updateCounter(float dt, int ticks) {
	if (!g_showCounter) {
		if (g_label) g_label->setVisible(false);
		return;
	}
	if (!g_label) {
		g_label = CCLabelBMFont::create("", "bigFont.fnt");
		g_label->setAnchorPoint({0.f, 1.f});
		g_label->setScale(0.35f);
		g_label->setOpacity(200);
		auto size = CCDirector::get()->getWinSize();
		g_label->setPosition({5.f, size.height - 5.f});
		g_label->setZOrder(9999);
		OverlayManager::get()->addChild(g_label);
	}
	g_label->setVisible(true);
	g_frames += 1;
	g_ticks += ticks;
	g_counterTime += dt;
	if (g_counterTime >= 0.5) {
		int fps = static_cast<int>(std::round(g_frames / g_counterTime));
		int tps = static_cast<int>(std::round(g_ticks / g_counterTime));
		g_label->setString(fmt::format("FPS {}  |  TPS {}", fps, tps).c_str());
		g_counterTime = 0.0;
		g_frames = 0;
		g_ticks = 0;
	}
}

static void applyFPS() {
	g_accum = 0.0;
	auto mod = Mod::get();
	g_enabled = mod->getSettingValue<bool>("enabled");
	g_substep = mod->getSettingValue<bool>("substep");
	g_inLevels = mod->getSettingValue<bool>("substep-in-levels");
	g_fps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));
	g_showCounter = mod->getSettingValue<bool>("show-counter");

	// Goes through the hook below, which swaps in the target FPS when enabled.
	CCDirector::get()->setAnimationInterval(g_gameInterval);
	double fps = g_enabled ? g_fps : 1.0 / g_gameInterval;

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
		// Inside levels GD already runs physics at 240 TPS by itself, and
		// running the whole level update several times per frame is what
		// crashed after deaths and in heavy levels. Off there by default.
		bool const inLevel = !g_inLevels && (PlayLayer::get() || LevelEditorLayer::get());
		if (!g_enabled || !g_substep || inLevel || g_fps <= 0 || dt <= 0.f) {
			g_accum = 0.0;
			CCScheduler::update(dt);
			updateCounter(dt, 1);
			return;
		}
		double const step = 1.0 / g_fps;

		// Anti-lag: after a hitch (dying, loading, heavy levels) or while the
		// phone can't keep up, run a single normal update instead of piling on
		// extra ones, which is what turned small hitches into big lag spikes.
		if (g_cooldown > 0.0) g_cooldown -= dt;
		if (dt > 1.0 / 30.0 || g_cooldown > 0.0) {
			g_accum = 0.0;
			CCScheduler::update(dt);
			updateCounter(dt, 1);
			return;
		}

		g_accum += dt;
		int steps = static_cast<int>(g_accum / step);
		if (steps <= 0) {
			// target is below the screen rate: just pass time through
			CCScheduler::update(static_cast<float>(g_accum));
			g_accum = 0.0;
			updateCounter(dt, 1);
			return;
		}
		// Only as many as needed for a normal frame (240 FPS on 90 Hz = 3).
		int const maxSteps = std::max(2, static_cast<int>(std::ceil(g_fps / 60.0)));
		if (steps > maxSteps) {
			steps = maxSteps;
			g_accum = steps * step; // drop the backlog instead of catching up
		}

		auto const start = std::chrono::steady_clock::now();
		auto director = CCDirector::get();
		auto scene = director->getRunningScene();
		int done = 0;
		for (int i = 0; i < steps; i++) {
			// Stop extra updates as soon as the game changes scene (entering,
			// leaving or restarting a level) or pauses. Updating a layer that
			// is being torn down is what caused crashes.
			if (i > 0 && (director->getNextScene() != nullptr
				|| director->getRunningScene() != scene
				|| director->isPaused())) {
				break;
			}
			CCScheduler::update(static_cast<float>(step));
			done++;
		}
		if (done < steps) {
			g_accum = 0.0; // drop leftover time after a scene change
		} else {
			g_accum -= steps * step;
		}
		// If the updates alone used most of the frame, the phone is falling
		// behind: back off for a second so the game stays smooth.
		double const spent = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - start).count();
		if (spent > dt * 0.7) g_cooldown = 1.0;
		updateCounter(dt, done);
	}
};

// GD (and Android, when you leave and return to the app) can reset the frame
// interval. Remember what it wanted, but keep our target while enabled.
class $modify(ForceFPSDirector, CCDirector) {
	void setAnimationInterval(double interval) {
		if (interval > 0.0) g_gameInterval = interval;
		if (g_enabled && g_fps > 0) interval = 1.0 / g_fps;
		CCDirector::setAnimationInterval(interval);
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
	listenForSettingChanges<bool>("substep", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("substep-in-levels", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("show-counter", [](bool) { applyFPS(); });
	listenForSettingChanges<bool>("disable-vsync", [](bool) { applyFPS(); });
}
