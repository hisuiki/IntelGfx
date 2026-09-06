/* SPDX-License-Identifier: MIT */
// Shows what the GPU and the processor are each doing, on the same time base.
// The two together are what a graphics problem usually needs: a renderer that
// keeps a processor busy while the GPU idles is not short of GPU, and this is
// the cheapest way to see which of the two is the one waiting.

#include <Application.h>
#include <Bitmap.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <MessageRunner.h>
#include <OS.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Device.h"
#include "DeviceRoster.h"
#include "IntelGfxABI.h"

using namespace IntelGfx;

static const char* kSignature = "application/x-vnd.IntelGfx-Monitor";
static const uint32 kTick = 'tick';
static const bigtime_t kInterval = 200000;
static const int32 kHistory = 600;
static const uint32 kMaxCores = 64;

// The GPU's clock frequency register (GEN6_RPSTAT1). On generation 9 the
// CAGF field counts in thirds of a 50 MHz step rather than whole ones.
// GPU utilisation itself comes from the driver's per-context timestamp
// rather than from a power residency counter, so it stays meaningful
// regardless of whether the GPU enters RC6.
static const uint32 kFrequencyStatus = 0xa01c;
static const uint32 kFrequencyShift = 23;
static const uint32 kFrequencyMask = 0x1ff;
static const uint32 kFrequencyStep = 50;
static const uint32 kFrequencyScaler = 3;


// Samples both devices. Every reading is a difference against the previous
// one over the time that actually elapsed, so a late tick reports the right
// fraction rather than a spike.
class Sampler {
public:
	Sampler()
		:
		fHasGPU(false),
		fCores(0),
		fLastTime(0)
	{
		memset(fLastActive, 0, sizeof(fLastActive));
	}

	void Init()
	{
		system_info system;
		if (get_system_info(&system) == B_OK)
			fCores = min_c((uint32)system.cpu_count, kMaxCores);

		BMessage reply, item;
		if (ListDevices(reply) == B_OK) {
			for (int32 i = 0; reply.FindMessage("device", i, &item) == B_OK; i++) {
				const char* path = NULL;
				int32 status;
				if (item.FindString("path", &path) != B_OK
					|| item.FindInt32("status", &status) != B_OK || status != B_OK)
					continue;
				if (fDevice.Open(path) != B_OK)
					continue;
				auto probe = Request<GpuActivity>();
				if (fDevice.Ioctl(kGpuActivity, &probe) == B_OK) {
					fHasGPU = true;
					fPath = path;
					break;
				}
			}
		}

		// Prime the counters so the first sample measures a real interval.
		Sample();
		// Bring-up aid: which device answered, and whether its counters read
		// at all. A graph alone cannot tell a genuinely idle GPU from one
		// whose registers this build never managed to reach.
		if (getenv("INTEL_GFX_MONITOR_TRACE") != NULL) {
			fprintf(stderr, "monitor: %s, %" B_PRIu32 " cores, %"
				B_PRIu32 " MHz\n",
				fHasGPU ? fPath.String() : "no IntelGfx device", fCores,
				fMegahertz);
		}
	}

	struct Client {
		team_id team;
		uint32 contexts;
		uint64 ticks;
		float busy;
		BString name;
	};

	uint32 ClientCount() const { return fClientCount; }
	const Client& ClientAt(uint32 i) const { return fClients[i]; }

	bool HasGPU() const { return fHasGPU; }
	uint32 Cores() const { return fEnabledCores; }
	const char* Path() const { return fPath.String(); }

	float GPUBusy() const { return fGPUBusy; }
	uint32 Megahertz() const { return fMegahertz; }
	float CPUBusy() const { return fCPUBusy; }
	float CoreBusy(uint32 core) const { return fCoreBusy[core]; }

	// Keeps one entry per team that has the device open, and turns the
	// difference in its GPU ticks into the share of the interval it used.
	void _UpdateClients(const GpuActivity& activity, bigtime_t elapsed)
	{
		Client updated[kMaxActivityClients];
		uint32 count = 0;
		for (uint32 i = 0; i < activity.count && i < kMaxActivityClients; i++) {
			const ActivityClient& source = activity.clients[i];
			Client& entry = updated[count++];
			entry.team = source.team;
			entry.contexts = source.contexts;
			entry.ticks = source.ticks;
			entry.busy = 0.0f;

			uint64 previous = 0;
			bool known = false;
			for (uint32 j = 0; j < fClientCount; j++) {
				if (fClients[j].team != source.team)
					continue;
				previous = fClients[j].ticks;
				entry.name = fClients[j].name;
				known = true;
				break;
			}
			if (!known) {
				team_info info;
				if (get_team_info(source.team, &info) == B_OK) {
					const char* leaf = strrchr(info.args, '/');
					entry.name = leaf != NULL ? leaf + 1 : info.args;
					int32 space = entry.name.FindFirst(' ');
					if (space > 0)
						entry.name.Truncate(space);
				} else
					entry.name.SetToFormat("team %" B_PRId32, source.team);
			}
			if (known && elapsed > 0 && activity.timestampHz != 0
				&& source.ticks >= previous) {
				double busy = (double)(source.ticks - previous) * 1000000.0
					/ (double)activity.timestampHz / (double)elapsed;
				entry.busy = (float)max_c(0.0, min_c(1.0, busy));
			}
		}
		for (uint32 i = 0; i < count; i++)
			fClients[i] = updated[i];
		fClientCount = count;
	}

	void Sample()
	{
		bigtime_t now = system_time();
		bigtime_t elapsed = now - fLastTime;
		fLastTime = now;
		bool measured = elapsed > 0;

		if (fHasGPU) {
			// GPU time comes from the driver, which reads it out of the
			// engine's own context timestamp. That is time the hardware spent
			// running work, so unlike a power residency counter it stays
			// meaningful whether or not the GPU is entering RC6.
			auto activity = Request<GpuActivity>();
			if (fDevice.Ioctl(kGpuActivity, &activity) == B_OK && measured) {
				uint64 ticks = activity.totalTicks - fLastTicks;
				fLastTicks = activity.totalTicks;
				if (activity.timestampHz != 0) {
					double busy = (double)ticks * 1000000.0
						/ (double)activity.timestampHz / (double)elapsed;
					fGPUBusy = (float)max_c(0.0, min_c(1.0, busy));
				}
				_UpdateClients(activity, elapsed);
			}
			uint32 status = 0;
			if (fDevice.Read(kFrequencyStatus, status) == B_OK) {
				fMegahertz = ((status >> kFrequencyShift) & kFrequencyMask)
					* kFrequencyStep / kFrequencyScaler;
			}
		}

		cpu_info cpus[kMaxCores];
		if (fCores > 0 && get_cpu_info(0, fCores, cpus) == B_OK) {
			// A disabled processor still occupies a slot and still reports the
			// active time it had before it went offline. Averaging over it
			// divides by processors that cannot be busy, which understates the
			// load by however many are switched off.
			bigtime_t total = 0;
			uint32 enabled = 0;
			for (uint32 i = 0; i < fCores; i++) {
				bigtime_t active = cpus[i].active_time - fLastActive[i];
				fLastActive[i] = cpus[i].active_time;
				if (!cpus[i].enabled) {
					fCoreBusy[i] = 0.0f;
					continue;
				}
				enabled++;
				if (!measured)
					continue;
				float busy = (float)active / (float)elapsed;
				fCoreBusy[i] = max_c(0.0f, min_c(1.0f, busy));
				total += active;
			}
			fEnabledCores = enabled;
			if (measured && enabled > 0) {
				float busy = (float)total / (float)elapsed / (float)enabled;
				fCPUBusy = max_c(0.0f, min_c(1.0f, busy));
			}
		}
	}

private:
	Device fDevice;
	BString fPath;
	bool fHasGPU;
	uint32 fCores;

	uint64 fLastTicks = 0;
	Client fClients[kMaxActivityClients];
	uint32 fClientCount = 0;
	uint32 fEnabledCores = 0;
	bigtime_t fLastTime;
	bigtime_t fLastActive[kMaxCores];

	float fGPUBusy = 0.0f;
	uint32 fMegahertz = 0;
	float fCPUBusy = 0.0f;
	float fCoreBusy[kMaxCores] = {};
};


// One scrolling graph: a filled area under a line, with the newest sample at
// the right edge. The whole view is painted on every draw so that nothing
// erases to the background first and flickers.
class GraphView : public BView {
public:
	GraphView(const char* title, rgb_color colour)
		:
		BView(title, B_WILL_DRAW | B_FRAME_EVENTS | B_SUPPORTS_LAYOUT),
		fTitle(title),
		fColour(colour),
		fCount(0),
		fCoreCount(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		memset(fHistory, 0, sizeof(fHistory));
		memset(fCores, 0, sizeof(fCores));
	}

	void Push(float value, const char* readout)
	{
		if (fCount < kHistory) {
			fHistory[fCount++] = value;
		} else {
			memmove(fHistory, fHistory + 1, (kHistory - 1) * sizeof(float));
			fHistory[kHistory - 1] = value;
		}
		fReadout = readout;
		Invalidate();
	}

	// Optional per-core detail, drawn as a row of bars along the bottom.
	void SetCores(const float* cores, uint32 count)
	{
		fCoreCount = min_c(count, kMaxCores);
		for (uint32 i = 0; i < fCoreCount; i++)
			fCores[i] = cores[i];
	}

	virtual BSize MinSize() { return BSize(240, 110); }
	virtual BSize MaxSize() { return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED); }
	virtual BSize PreferredSize() { return BSize(560, 150); }

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
		rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
		bool dark = panel.Brightness() < 127;
		rgb_color plot = tint_color(panel, dark ? 0.82f : 1.08f);
		rgb_color grid = tint_color(plot, dark ? 1.25f : 0.92f);

		SetDrawingMode(B_OP_COPY);
		SetHighColor(panel);
		FillRect(bounds);

		font_height height;
		GetFontHeight(&height);
		float lineHeight = ceilf(height.ascent + height.descent + height.leading);
		float top = lineHeight + 6.0f;
		float bottom = bounds.bottom - 2.0f;
		if (fCoreCount > 0)
			bottom -= 12.0f;
		BRect graph(2.0f, top, bounds.right - 2.0f, bottom);
		if (!graph.IsValid())
			return;

		SetHighColor(plot);
		FillRect(graph);

		// Quarters, so the eye can read a level without a legend.
		SetHighColor(grid);
		for (int32 i = 1; i < 4; i++) {
			float y = graph.top + graph.Height() * i / 4.0f;
			StrokeLine(BPoint(graph.left, y), BPoint(graph.right, y));
		}

		if (fCount > 1) {
			float step = graph.Width() / (float)(kHistory - 1);
			float first = graph.right - step * (float)(fCount - 1);
			BPoint area[kHistory + 2];
			int32 points = 0;
			area[points++] = BPoint(first, graph.bottom);
			for (int32 i = 0; i < fCount; i++) {
				area[points++] = BPoint(first + step * (float)i,
					graph.bottom - graph.Height() * fHistory[i]);
			}
			area[points++] = BPoint(graph.right, graph.bottom);

			rgb_color fill = fColour;
			fill.alpha = 90;
			SetDrawingMode(B_OP_ALPHA);
			SetHighColor(fill);
			FillPolygon(area, points);

			SetDrawingMode(B_OP_COPY);
			SetHighColor(fColour);
			SetPenSize(1.5f);
			StrokePolygon(area + 1, points - 2, false);
			SetPenSize(1.0f);
		}

		if (fCoreCount > 0) {
			// One bar per core, so a single saturated core is visible even
			// while the average across all of them looks unremarkable.
			float width = bounds.Width() - 4.0f;
			float each = width / (float)fCoreCount;
			for (uint32 i = 0; i < fCoreCount; i++) {
				BRect bar(2.0f + each * i, bounds.bottom - 9.0f,
					2.0f + each * (i + 1) - 1.0f, bounds.bottom - 1.0f);
				SetHighColor(plot);
				FillRect(bar);
				bar.top = bar.bottom - bar.Height() * fCores[i];
				SetHighColor(fColour);
				FillRect(bar);
			}
		}

		SetDrawingMode(B_OP_OVER);
		SetHighColor(text);
		MovePenTo(BPoint(2.0f, height.ascent));
		DrawString(fTitle.String());
		if (fReadout.Length() > 0) {
			float width = StringWidth(fReadout.String());
			MovePenTo(BPoint(bounds.right - 2.0f - width, height.ascent));
			DrawString(fReadout.String());
		}
	}

private:
	BString fTitle;
	BString fReadout;
	rgb_color fColour;
	float fHistory[kHistory];
	int32 fCount;
	float fCores[kMaxCores];
	uint32 fCoreCount;
};


class MonitorWindow : public BWindow {
public:
	MonitorWindow()
		:
		BWindow(BRect(120, 120, 700, 500), "IntelGfx Activity",
			B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fSampler.Init();

		fGPU = new GraphView("GPU", (rgb_color){ 0, 140, 210, 255 });
		fCPU = new GraphView("CPU", (rgb_color){ 220, 130, 30, 255 });

		BLayoutBuilder::Group<>(this, B_VERTICAL, 4)
			.SetInsets(4)
			.Add(fGPU)
			.Add(fCPU);

		fRunner = new BMessageRunner(BMessenger(this), new BMessage(kTick),
			kInterval);
	}

	virtual ~MonitorWindow() { delete fRunner; }

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kTick) {
			BWindow::MessageReceived(message);
			return;
		}

		fSampler.Sample();

		BString readout;
		if (fSampler.HasGPU()) {
			// A frequency of zero is not a frequency: the render domain is
			// powered down, and its registers read back as nothing rather
			// than as a speed. Saying so beats printing "0 MHz".
			if (fSampler.Megahertz() == 0) {
				readout.SetToFormat("%.0f%%  ·  idle",
					fSampler.GPUBusy() * 100.0f);
			} else {
				readout.SetToFormat("%.0f%%  ·  %" B_PRIu32 " MHz",
					fSampler.GPUBusy() * 100.0f, fSampler.Megahertz());
			}
			fGPU->Push(fSampler.GPUBusy(), readout.String());
		} else {
			// Say so rather than drawing a flat line that looks like an idle
			// GPU: no Intel device answered, so nothing here was measured.
			fGPU->Push(0.0f, "no IntelGfx device");
		}

		float cores[kMaxCores];
		for (uint32 i = 0; i < fSampler.Cores(); i++)
			cores[i] = fSampler.CoreBusy(i);
		fCPU->SetCores(cores, fSampler.Cores());
		readout.SetToFormat("%.0f%%  ·  %" B_PRIu32 " cores",
			fSampler.CPUBusy() * 100.0f, fSampler.Cores());
		fCPU->Push(fSampler.CPUBusy(), readout.String());
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	Sampler fSampler;
	GraphView* fGPU;
	GraphView* fCPU;
	BMessageRunner* fRunner;
};


class MonitorApplication : public BApplication {
public:
	MonitorApplication()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun() { (new MonitorWindow())->Show(); }
};


int
main()
{
	MonitorApplication application;
	application.Run();
	return 0;
}
