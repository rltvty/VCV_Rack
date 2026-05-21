#include <chrono>
#include <vector>

#include "plugin.hpp"

namespace {


struct StickyAudioPort : audio::Port {
	std::string desiredDeviceName;
	float desiredSampleRate = 0.f;
	int desiredBlockSize = 0;
	bool autoReconnect = true;
	bool streamActive = false;
	double nextReconnectAttempt = 0.0;
	double reconnectIntervalSeconds = 5.0;
	bool reconnectMissingLogged = false;
	bool reconnectCandidateLogged = false;

	bool hasLiveDevice() {
		return streamActive && getDevice();
	}

	bool currentDeviceIsDiscoverable() {
		if (!getDriver() || getDeviceId() < 0)
			return false;

		std::vector<int> deviceIds = getDeviceIds();
		bool foundCurrentId = false;
		for (int deviceId : deviceIds) {
			if (deviceId == getDeviceId()) {
				foundCurrentId = true;
				break;
			}
		}
		if (!foundCurrentId)
			return false;

		std::string currentName = getDeviceName(getDeviceId());
		if (currentName.empty())
			return false;
		if (!desiredDeviceName.empty() && currentName != desiredDeviceName)
			return false;

		int availableInputs = getDeviceNumInputs(getDeviceId());
		int availableOutputs = getDeviceNumOutputs(getDeviceId());
		if (maxInputs > 0 && availableInputs <= inputOffset)
			return false;
		if (maxOutputs > 0 && availableOutputs <= outputOffset)
			return false;

		return true;
	}

	bool pollConnectionState() {
		if (!streamActive || !getDevice())
			return false;
		if (currentDeviceIsDiscoverable())
			return false;

		WARN("RltvtyAudioIO detected live device disappearance for \"%s\" (device ID %d)",
			desiredDeviceName.empty() ? getDevice()->getName().c_str() : desiredDeviceName.c_str(),
			getDeviceId());
		reconnectMissingLogged = false;
		reconnectCandidateLogged = false;
		setDeviceId(-1);
		scheduleReconnect(0.0);
		return true;
	}

	void syncLockFromDevice() {
		if (!hasLiveDevice())
			return;
		desiredDeviceName = getDevice()->getName();
		desiredSampleRate = getSampleRate();
		desiredBlockSize = getBlockSize();
		reconnectMissingLogged = false;
		reconnectCandidateLogged = false;
	}

	void clearLock() {
		desiredDeviceName.clear();
		desiredSampleRate = 0.f;
		desiredBlockSize = 0;
		nextReconnectAttempt = 0.0;
		reconnectMissingLogged = false;
		reconnectCandidateLogged = false;
	}

	void scheduleReconnect(double seconds = -1.0) {
		double delay = (seconds >= 0.0) ? seconds : reconnectIntervalSeconds;
		nextReconnectAttempt = system::getTime() + delay;
	}

	bool reconnectIfAvailable(bool force = false) {
		if (!autoReconnect || desiredDeviceName.empty() || !getDriver() || hasLiveDevice())
			return false;
		double now = system::getTime();
		if (!force && now < nextReconnectAttempt)
			return false;
		scheduleReconnect();

		if (getDeviceId() >= 0) {
			INFO("RltvtyAudioIO clearing stale device state for \"%s\" before reconnect", desiredDeviceName.c_str());
			setDeviceId(-1);
		}

		int minInputs = std::max(0, inputOffset + maxInputs);
		int minOutputs = std::max(0, outputOffset + maxOutputs);

		std::vector<int> matchingDeviceIds;
		for (int deviceId : getDeviceIds()) {
			if (getDeviceName(deviceId) != desiredDeviceName)
				continue;
			if (getDeviceNumInputs(deviceId) < minInputs)
				continue;
			if (getDeviceNumOutputs(deviceId) < minOutputs)
				continue;
			matchingDeviceIds.push_back(deviceId);
		}

		if (matchingDeviceIds.empty()) {
			if (!reconnectMissingLogged) {
				INFO("RltvtyAudioIO reconnect waiting for device named \"%s\"", desiredDeviceName.c_str());
				reconnectMissingLogged = true;
			}
			reconnectCandidateLogged = false;
			return false;
		}

		reconnectMissingLogged = false;

		if (!reconnectCandidateLogged) {
			std::string candidates;
			for (size_t i = 0; i < matchingDeviceIds.size(); i++) {
				if (i > 0)
					candidates += ", ";
				candidates += string::f("%d", matchingDeviceIds[i]);
			}
			INFO("RltvtyAudioIO reconnect found device \"%s\" with candidate IDs [%s]", desiredDeviceName.c_str(), candidates.c_str());
			reconnectCandidateLogged = true;
		}

		for (int deviceId : matchingDeviceIds) {
			INFO("RltvtyAudioIO reconnect trying device \"%s\" with ID %d", desiredDeviceName.c_str(), deviceId);
			setDeviceId(deviceId);
			if (!hasLiveDevice()) {
				WARN("RltvtyAudioIO reconnect failed for device \"%s\" with ID %d", desiredDeviceName.c_str(), deviceId);
				continue;
			}
			if (desiredSampleRate > 0.f)
				setSampleRate(desiredSampleRate);
			if (desiredBlockSize > 0)
				setBlockSize(desiredBlockSize);
			syncLockFromDevice();
			INFO("RltvtyAudioIO reconnect succeeded for device \"%s\"", desiredDeviceName.c_str());
			return true;
		}

		return false;
	}

	json_t* toJsonLocked() {
		if (getDevice())
			syncLockFromDevice();

		json_t* rootJ = audio::Port::toJson();
		if (!desiredDeviceName.empty())
			json_object_set_new(rootJ, "lockedDeviceName", json_string(desiredDeviceName.c_str()));
		if (desiredSampleRate > 0.f)
			json_object_set_new(rootJ, "lockedSampleRate", json_real(desiredSampleRate));
		if (desiredBlockSize > 0)
			json_object_set_new(rootJ, "lockedBlockSize", json_integer(desiredBlockSize));
		json_object_set_new(rootJ, "autoReconnect", json_boolean(autoReconnect));
		return rootJ;
	}

	void fromJsonLocked(json_t* rootJ) {
		int driverId = -1;
		json_t* driverJ = json_object_get(rootJ, "driver");
		if (driverJ)
			driverId = json_integer_value(driverJ);

		json_t* lockedDeviceNameJ = json_object_get(rootJ, "lockedDeviceName");
		if (lockedDeviceNameJ)
			desiredDeviceName = json_string_value(lockedDeviceNameJ);
		else {
			json_t* deviceNameJ = json_object_get(rootJ, "deviceName");
			if (deviceNameJ)
				desiredDeviceName = json_string_value(deviceNameJ);
			else
				desiredDeviceName.clear();
		}

		desiredSampleRate = 0.f;
		json_t* lockedSampleRateJ = json_object_get(rootJ, "lockedSampleRate");
		if (lockedSampleRateJ)
			desiredSampleRate = json_number_value(lockedSampleRateJ);
		else {
			json_t* sampleRateJ = json_object_get(rootJ, "sampleRate");
			if (sampleRateJ)
				desiredSampleRate = json_number_value(sampleRateJ);
		}

		desiredBlockSize = 0;
		json_t* lockedBlockSizeJ = json_object_get(rootJ, "lockedBlockSize");
		if (lockedBlockSizeJ)
			desiredBlockSize = json_integer_value(lockedBlockSizeJ);
		else {
			json_t* blockSizeJ = json_object_get(rootJ, "blockSize");
			if (blockSizeJ)
				desiredBlockSize = json_integer_value(blockSizeJ);
		}

		json_t* autoReconnectJ = json_object_get(rootJ, "autoReconnect");
		if (autoReconnectJ)
			autoReconnect = json_boolean_value(autoReconnectJ);
		else
			autoReconnect = true;

		json_t* inputOffsetJ = json_object_get(rootJ, "inputOffset");
		if (inputOffsetJ)
			inputOffset = json_integer_value(inputOffsetJ);

		json_t* outputOffsetJ = json_object_get(rootJ, "outputOffset");
		if (outputOffsetJ)
			outputOffset = json_integer_value(outputOffsetJ);

		setDriverId(driverId);
		// Avoid silently binding to the driver's default device when restoring a patch.
		setDeviceId(-1);
		scheduleReconnect(0.0);
		reconnectIfAvailable(true);
	}
};


template <int NUM_MODULE_INPUTS, int NUM_MODULE_OUTPUTS>
struct SplitAudioPort : StickyAudioPort {
	static constexpr int ENGINE_INPUT_CHANNELS = (NUM_MODULE_INPUTS > 0) ? NUM_MODULE_INPUTS : 1;
	static constexpr int ENGINE_OUTPUT_CHANNELS = (NUM_MODULE_OUTPUTS > 0) ? NUM_MODULE_OUTPUTS : 1;

	Module* module;

	dsp::DoubleRingBuffer<dsp::Frame<ENGINE_INPUT_CHANNELS>, 32768> engineInputBuffer;
	dsp::DoubleRingBuffer<dsp::Frame<ENGINE_OUTPUT_CHANNELS>, 32768> engineOutputBuffer;

	dsp::SampleRateConverter<ENGINE_INPUT_CHANNELS> inputSrc;
	dsp::SampleRateConverter<ENGINE_OUTPUT_CHANNELS> outputSrc;

	int deviceNumInputs = 0;
	int deviceNumOutputs = 0;
	float deviceSampleRate = 0.f;
	int requestedEngineFrames = 0;

	SplitAudioPort(Module* module) {
		this->module = module;
		maxOutputs = NUM_MODULE_INPUTS;
		maxInputs = NUM_MODULE_OUTPUTS;
		inputSrc.setQuality(6);
		outputSrc.setQuality(6);
	}

	void setMaster(bool master = true) {
		if (master) {
			APP->engine->setMasterModule(module);
		}
		else if (isMaster()) {
			APP->engine->setMasterModule(NULL);
		}
	}

	bool isMaster() {
		return APP->engine->getMasterModule() == module;
	}

	void processInput(const float* input, int inputStride, int frames) override {
		deviceNumInputs = std::min(getNumInputs(), NUM_MODULE_OUTPUTS);
		deviceNumOutputs = std::min(getNumOutputs(), NUM_MODULE_INPUTS);
		deviceSampleRate = getSampleRate();

		if (!APP->engine->getMasterModule())
			setMaster();
		bool master = isMaster();

		if (master)
			APP->engine->setSuggestedSampleRate(deviceSampleRate);

		float engineSampleRate = APP->engine->getSampleRate();
		float sampleRateRatio = engineSampleRate / deviceSampleRate;

		int maxEngineFrames = (int) std::ceil(frames * sampleRateRatio * 2.0) - 1;
		if (!master && (int) engineOutputBuffer.size() > maxEngineFrames)
			engineOutputBuffer.clear();

		if (NUM_MODULE_OUTPUTS > 0 && deviceNumInputs > 0) {
			if (master)
				engineOutputBuffer.clear();
			outputSrc.setRates(deviceSampleRate, engineSampleRate);
			outputSrc.setChannels(deviceNumInputs);
			int inputFrames = frames;
			int outputFrames = engineOutputBuffer.capacity();
			outputSrc.process(input, inputStride, &inputFrames, (float*) engineOutputBuffer.endData(), ENGINE_OUTPUT_CHANNELS, &outputFrames);
			engineOutputBuffer.endIncr(outputFrames);
			requestedEngineFrames = engineOutputBuffer.size();
		}
		else {
			requestedEngineFrames = std::max((int) std::ceil(frames * sampleRateRatio) - (int) engineInputBuffer.size(), 0);
		}
	}

	void processBuffer(const float* input, int inputStride, float* output, int outputStride, int frames) override {
		if (isMaster() && requestedEngineFrames > 0)
			APP->engine->stepBlock(requestedEngineFrames);
	}

	void processOutput(float* output, int outputStride, int frames) override {
		float engineSampleRate = APP->engine->getSampleRate();
		float sampleRateRatio = engineSampleRate / deviceSampleRate;

		if (NUM_MODULE_INPUTS > 0 && deviceNumOutputs > 0) {
			inputSrc.setRates(engineSampleRate, deviceSampleRate);
			inputSrc.setChannels(deviceNumOutputs);
			int inputFrames = engineInputBuffer.size();
			int outputFrames = frames;
			inputSrc.process((const float*) engineInputBuffer.startData(), ENGINE_INPUT_CHANNELS, &inputFrames, output, outputStride, &outputFrames);
			engineInputBuffer.startIncr(inputFrames);
			for (int i = 0; i < outputFrames; i++) {
				for (int j = 0; j < deviceNumOutputs; j++) {
					output[i * outputStride + j] = clamp(output[i * outputStride + j], -1.f, 1.f);
				}
			}
			for (int i = outputFrames; i < frames; i++) {
				for (int j = 0; j < deviceNumOutputs; j++) {
					output[i * outputStride + j] = 0.f;
				}
			}
		}

		int maxEngineFrames = (int) std::ceil(frames * sampleRateRatio * 2.0) - 1;
		if ((int) engineInputBuffer.size() > maxEngineFrames)
			engineInputBuffer.clear();
	}

	void onStartStream() override {
		streamActive = true;
		engineInputBuffer.clear();
		engineOutputBuffer.clear();
	}

	void onStopStream() override {
		bool wasActive = streamActive;
		streamActive = false;
		deviceNumInputs = 0;
		deviceNumOutputs = 0;
		deviceSampleRate = 0.f;
		engineInputBuffer.clear();
		engineOutputBuffer.clear();
		if (wasActive && !desiredDeviceName.empty()) {
			INFO("RltvtyAudioIO stream stopped for device \"%s\"", desiredDeviceName.c_str());
			scheduleReconnect(0.0);
		}
		if (APP->engine->getMasterModule() == module)
			APP->engine->setMasterModule_NoLock(NULL);
	}
};


static int moduleWidthHp(int channels) {
	switch (channels) {
		case 2: return 5;
		case 8: return 27;
		default: return 34;
	}
}

static std::string panelTitle(int moduleInputs, int moduleOutputs) {
	if (moduleOutputs > 0)
		return string::f("Audio In %d", moduleOutputs);
	return string::f("Audio Out %d", moduleInputs);
}

static std::string getChannelDetailText(std::string name, int channels, int offset, bool isInput) {
	std::string text = name;
	text += " (";
	if (channels > 0) {
		text += string::f("%d-%d %s", offset + 1, offset + channels, isInput ? "in" : "out");
	}
	text += ")";
	return text;
}


template <int NUM_MODULE_INPUTS, int NUM_MODULE_OUTPUTS>
struct SplitAudioDeviceChoice : AudioDeviceChoice {
	struct ValueItem : MenuItem {
		StickyAudioPort* port;
		int deviceId = -1;
		int inputOffset = 0;
		int outputOffset = 0;
		std::string desiredDeviceName;

		void onAction(const ActionEvent& e) override {
			port->inputOffset = inputOffset;
			port->outputOffset = outputOffset;
			port->desiredDeviceName = desiredDeviceName;
			port->scheduleReconnect(0.0);
			port->reconnectMissingLogged = false;
			port->reconnectCandidateLogged = false;
			port->setDeviceId(deviceId);
			if (port->getDevice())
				port->syncLockFromDevice();
		}
	};

	void onAction(const ActionEvent& e) override {
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Audio device"));

		StickyAudioPort* stickyPort = dynamic_cast<StickyAudioPort*>(port);
		assert(stickyPort);

		ValueItem* noneItem = new ValueItem;
		noneItem->port = stickyPort;
		noneItem->deviceId = -1;
		noneItem->text = "(No device)";
		noneItem->rightText = CHECKMARK(port->getDeviceId() < 0 && stickyPort->desiredDeviceName.empty());
		menu->addChild(noneItem);

		const bool wantsInputs = (NUM_MODULE_OUTPUTS > 0);
		const int channelBlock = wantsInputs ? std::max(1, port->maxInputs) : std::max(1, port->maxOutputs);

		for (int deviceId : port->getDeviceIds()) {
			int totalChannels = wantsInputs ? port->getDeviceNumInputs(deviceId) : port->getDeviceNumOutputs(deviceId);
			if (totalChannels <= 0)
				continue;

			std::string name = port->getDeviceName(deviceId);
			for (int offset = 0; offset < totalChannels; offset += channelBlock) {
				int channels = math::clamp(totalChannels - offset, 0, channelBlock);
				if (channels <= 0)
					break;

				ValueItem* item = new ValueItem;
				item->port = stickyPort;
				item->deviceId = deviceId;
				item->inputOffset = wantsInputs ? offset : 0;
				item->outputOffset = wantsInputs ? 0 : offset;
				item->desiredDeviceName = name;
				item->text = getChannelDetailText(name, channels, offset, wantsInputs);
				item->rightText = CHECKMARK(
					port->getDeviceId() == deviceId &&
					port->inputOffset == item->inputOffset &&
					port->outputOffset == item->outputOffset
				);
				menu->addChild(item);
			}
		}
	}

	void step() override {
		StickyAudioPort* stickyPort = dynamic_cast<StickyAudioPort*>(port);
		if (!stickyPort) {
			text = "No device";
			color.a = 0.5;
			return;
		}

		const bool wantsInputs = (NUM_MODULE_OUTPUTS > 0);
		std::string detail;
		if (stickyPort->hasLiveDevice()) {
			int channels = wantsInputs ? port->getNumInputs() : port->getNumOutputs();
			int offset = wantsInputs ? port->inputOffset : port->outputOffset;
			detail = getChannelDetailText(port->getDevice()->getName(), channels, offset, wantsInputs);
			color.a = 1.0;
		}
		else if (!stickyPort->desiredDeviceName.empty()) {
			detail = stickyPort->desiredDeviceName + " (missing)";
			color.a = 0.7;
		}
		else {
			detail = "No device";
			color.a = 0.5;
		}
		text = detail;
	}
};


template <int NUM_MODULE_INPUTS, int NUM_MODULE_OUTPUTS>
struct SplitAudioDisplay : LedDisplay {
	using DeviceChoice = SplitAudioDeviceChoice<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>;
	static constexpr int CHANNELS = (NUM_MODULE_INPUTS > 0) ? NUM_MODULE_INPUTS : NUM_MODULE_OUTPUTS;

	void setAudioPort(audio::Port* port) {
		clearChildren();

		auto configureChoice = [&](LedDisplayChoice* choice, float x, float y, float w, float h) {
			choice->box.pos = math::Vec(x, y);
			choice->box.size = math::Vec(w, h);
			choice->textOffset = math::Vec(8.f, h * 0.68f);
			addChild(choice);
		};
		auto addVerticalSeparator = [&](float x, float y, float h) {
			LedDisplaySeparator* separator = createWidget<LedDisplaySeparator>(math::Vec(x, y));
			separator->box.size = math::Vec(0.f, h);
			addChild(separator);
		};
		auto addHorizontalSeparator = [&](float y) {
			LedDisplaySeparator* separator = createWidget<LedDisplaySeparator>(math::Vec(0.f, y));
			separator->box.size = math::Vec(box.size.x, 0.f);
			addChild(separator);
		};

		if (CHANNELS == 2) {
			const float rowHeight = box.size.y / 4.f;

			AudioDriverChoice* driverChoice = createWidget<AudioDriverChoice>(math::Vec());
			driverChoice->port = port;
			configureChoice(driverChoice, 0.f, 0.f, box.size.x, rowHeight);
			addHorizontalSeparator(rowHeight);

			DeviceChoice* deviceChoice = createWidget<DeviceChoice>(math::Vec());
			deviceChoice->port = port;
			configureChoice(deviceChoice, 0.f, rowHeight, box.size.x, rowHeight);
			addHorizontalSeparator(rowHeight * 2.f);

			AudioSampleRateChoice* sampleRateChoice = createWidget<AudioSampleRateChoice>(math::Vec());
			sampleRateChoice->port = port;
			configureChoice(sampleRateChoice, 0.f, rowHeight * 2.f, box.size.x, rowHeight);
			addHorizontalSeparator(rowHeight * 3.f);

			AudioBlockSizeChoice* blockSizeChoice = createWidget<AudioBlockSizeChoice>(math::Vec());
			blockSizeChoice->port = port;
			configureChoice(blockSizeChoice, 0.f, rowHeight * 3.f, box.size.x, rowHeight);
			return;
		}

		const float rowHeight = box.size.y / 2.f;
		const float colWidth = box.size.x / 2.f;

		AudioDriverChoice* driverChoice = createWidget<AudioDriverChoice>(math::Vec());
		driverChoice->port = port;
		configureChoice(driverChoice, 0.f, 0.f, colWidth, rowHeight);

		DeviceChoice* deviceChoice = createWidget<DeviceChoice>(math::Vec());
		deviceChoice->port = port;
		configureChoice(deviceChoice, colWidth, 0.f, colWidth, rowHeight);

		addVerticalSeparator(colWidth, 0.f, rowHeight);
		addHorizontalSeparator(rowHeight);

		AudioSampleRateChoice* sampleRateChoice = createWidget<AudioSampleRateChoice>(math::Vec());
		sampleRateChoice->port = port;
		configureChoice(sampleRateChoice, 0.f, rowHeight, colWidth, rowHeight);

		AudioBlockSizeChoice* blockSizeChoice = createWidget<AudioBlockSizeChoice>(math::Vec());
		blockSizeChoice->port = port;
		configureChoice(blockSizeChoice, colWidth, rowHeight, colWidth, rowHeight);

		addVerticalSeparator(colWidth, rowHeight, rowHeight);
	}
};


struct SplitPanel : Widget {
	std::string title;
	bool hasMasterSection = false;
	float masterSectionWidth = 0.f;
	float masterSectionDividerTop = 34.f;
	float masterSectionDividerBottom = 0.f;

	void draw(const DrawArgs& args) override {
		NVGcolor bg = settings::preferDarkPanels ? nvgRGB(42, 42, 42) : nvgRGB(235, 235, 235);
		NVGcolor border = settings::preferDarkPanels ? nvgRGB(78, 78, 78) : nvgRGB(140, 140, 140);
		NVGcolor fg = settings::preferDarkPanels ? nvgRGB(230, 230, 230) : nvgRGB(40, 40, 40);

		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
		nvgFillColor(args.vg, bg);
		nvgFill(args.vg);

		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
		nvgStrokeWidth(args.vg, 1.f);
		nvgStrokeColor(args.vg, border);
		nvgStroke(args.vg);

		if (hasMasterSection && masterSectionWidth > 0.f) {
			float x = masterSectionWidth;
			NVGcolor dividerDark = settings::preferDarkPanels ? nvgRGB(58, 58, 58) : nvgRGB(150, 150, 150);
			NVGcolor dividerLight = settings::preferDarkPanels ? nvgRGB(95, 95, 95) : nvgRGB(210, 210, 210);
			float yTop = masterSectionDividerTop;
			float yBottom = (masterSectionDividerBottom > 0.f) ? masterSectionDividerBottom : (box.size.y - 12.f);

			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x - 1.5f, yTop);
			nvgLineTo(args.vg, x - 1.5f, yBottom);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStrokeColor(args.vg, dividerLight);
			nvgStroke(args.vg);

			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x, yTop);
			nvgLineTo(args.vg, x, yBottom);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStrokeColor(args.vg, dividerDark);
			nvgStroke(args.vg);

			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x + 1.5f, yTop);
			nvgLineTo(args.vg, x + 1.5f, yBottom);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStrokeColor(args.vg, dividerLight);
			nvgStroke(args.vg);
		}

		std::string fontPath = asset::system("res/fonts/Nunito-Bold.ttf");
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (font) {
			nvgSave(args.vg);
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 16.f);
			nvgTextLetterSpacing(args.vg, 0.f);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, fg);
			nvgText(args.vg, box.size.x / 2.f, 10.f, title.c_str(), NULL);

			nvgFontSize(args.vg, 10.f);
			nvgFillColor(args.vg, settings::preferDarkPanels ? nvgRGB(170, 170, 170) : nvgRGB(90, 90, 90));
			nvgText(args.vg, box.size.x / 2.f, 24.f, "Sticky device reconnect", NULL);
			nvgRestore(args.vg);
		}

		Widget::draw(args);
	}
};


struct ChannelNumberLabel : Widget {
	std::string text;

	void draw(const DrawArgs& args) override {
		std::string fontPath = asset::system("res/fonts/Nunito-Bold.ttf");
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;

		nvgSave(args.vg);
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 11.f);
		nvgTextLetterSpacing(args.vg, 0.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, settings::preferDarkPanels ? nvgRGB(210, 210, 210) : nvgRGB(70, 70, 70));
		nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, text.c_str(), NULL);
		nvgRestore(args.vg);
	}
};


struct SmallTextLabel : Widget {
	std::string text;
	float fontSize = 9.f;
	NVGcolor color = nvgRGB(70, 70, 70);
	bool leftAlign = false;
	bool rightAlign = false;

	void draw(const DrawArgs& args) override {
		std::string fontPath = asset::system("res/fonts/Nunito-Bold.ttf");
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;

		nvgSave(args.vg);
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextLetterSpacing(args.vg, 0.f);
		int align = NVG_ALIGN_CENTER;
		if (leftAlign)
			align = NVG_ALIGN_LEFT;
		else if (rightAlign)
			align = NVG_ALIGN_RIGHT;
		nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, color);
		float textX = box.size.x * 0.5f;
		if (leftAlign)
			textX = 0.f;
		else if (rightAlign)
			textX = box.size.x;
		nvgText(args.vg, textX, box.size.y * 0.5f, text.c_str(), NULL);
		nvgRestore(args.vg);
	}
};

template <typename TBase = GrayModuleLightWidget>
struct TMagentaLight : TBase {
	TMagentaLight() {
		this->addBaseColor(nvgRGB(0xd9, 0x3d, 0xff));
	}
};
using MagentaLight = TMagentaLight<>;

template <typename TBase = GrayModuleLightWidget>
struct VuSimpleLight : TBase {
	VuSimpleLight() {
		this->box.size = mm2px(math::Vec(2.2, 2.2));
	}
};


template <int NUM_MODULE_INPUTS, int NUM_MODULE_OUTPUTS>
struct SplitAudioModule : Module {
	static constexpr int CHANNELS = (NUM_MODULE_INPUTS > 0) ? NUM_MODULE_INPUTS : NUM_MODULE_OUTPUTS;
	static constexpr bool HAS_GAIN_UI = (CHANNELS == 8);
	static constexpr int VU_SEGMENTS = HAS_GAIN_UI ? 9 : 4;

	enum ParamIds {
		MASTER_GAIN_PARAM,
		METER_TAP_PARAM,
		CLIP_RESET_PARAM,
		CLIP_RESET_TIME_PARAM,
		ENUMS(CHANNEL_GAIN_PARAMS, CHANNELS),
		NUM_PARAMS
	};
	enum InputIds {
		ENUMS(AUDIO_INPUTS, NUM_MODULE_INPUTS),
		NUM_INPUTS
	};
	enum OutputIds {
		ENUMS(AUDIO_OUTPUTS, NUM_MODULE_OUTPUTS),
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(CLIP_LIGHTS, CHANNELS),
		ENUMS(VU_LIGHTS, CHANNELS * VU_SEGMENTS),
		NUM_LIGHTS
	};

	SplitAudioPort<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS> port;
	dsp::ClockDivider lightDivider;
	dsp::VuMeter2 vuMeters[CHANNELS];
	dsp::SchmittTrigger clipResetTrigger;
	bool clipLatched[CHANNELS] = {};
	float clipResetTimers[CHANNELS] = {};

	static float dbToGain(float db) {
		return std::pow(10.f, db / 20.f);
	}

	float getMasterGainDb() {
		return params[MASTER_GAIN_PARAM].getValue();
	}

	float getChannelGainDb(int channel) {
		return params[CHANNEL_GAIN_PARAMS + channel].getValue();
	}

	float getTotalGainDb(int channel) {
		return getMasterGainDb() + getChannelGainDb(channel);
	}

	float applyGain(float sample, int channel) {
		return sample * dbToGain(getTotalGainDb(channel));
	}

	bool isMeterPostGain() {
		return params[METER_TAP_PARAM].getValue() < 0.5f;
	}

	float getClipSample(float preGainSample, float postGainSample) {
		// Audio In modules expose hardware inputs as module outputs, so they use
		// NUM_MODULE_OUTPUTS > 0 and should indicate ADC/full-scale clipping
		// before digital boost.
		if (NUM_MODULE_OUTPUTS > 0)
			return preGainSample;
		// Audio Out modules expose hardware outputs as module inputs, so they use
		// NUM_MODULE_INPUTS > 0 and should indicate DAC/full-scale clipping after
		// output gain.
		return postGainSample;
	}

	float getClipAutoResetSeconds() {
		if (!HAS_GAIN_UI)
			return 0.f;
		int mode = (int) std::round(params[CLIP_RESET_TIME_PARAM].getValue());
		switch (mode) {
			case 0:
				return 3.f;
			case 1:
				return 10.f;
			default:
				return 0.f;
		}
	}

	void clearClip(int channel) {
		clipLatched[channel] = false;
		clipResetTimers[channel] = 0.f;
	}

	void clearClips() {
		for (int i = 0; i < CHANNELS; i++)
			clearClip(i);
	}

	void triggerClip(int channel) {
		clipLatched[channel] = true;
		clipResetTimers[channel] = getClipAutoResetSeconds();
	}

	void detectClip(int channel, float sample) {
		if (!HAS_GAIN_UI)
			return;
		if (std::fabs(sample) >= 1.f)
			triggerClip(channel);
	}

	void processClipTimers(float sampleTime) {
		if (!HAS_GAIN_UI)
			return;

		float autoResetSeconds = getClipAutoResetSeconds();
		if (autoResetSeconds <= 0.f)
			return;

		for (int i = 0; i < CHANNELS; i++) {
			if (!clipLatched[i])
				continue;
			if (clipResetTimers[i] <= 0.f || clipResetTimers[i] > autoResetSeconds)
				clipResetTimers[i] = autoResetSeconds;
			clipResetTimers[i] -= sampleTime;
			if (clipResetTimers[i] <= 0.f)
				clearClip(i);
		}
	}

	SplitAudioModule() : port(this) {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(MASTER_GAIN_PARAM, -12.f, 12.f, 0.f, "Master gain", " dB");
		getParamQuantity(MASTER_GAIN_PARAM)->snapEnabled = true;
		configSwitch(METER_TAP_PARAM, 0.f, 1.f, 1.f, "Meter tap", {"Post-gain", "Pre-gain"});
		configButton(CLIP_RESET_PARAM, "Reset clip indicators");
		configSwitch(CLIP_RESET_TIME_PARAM, 0.f, 2.f, 2.f, "Clip indicator reset mode", {"3 seconds", "10 seconds", "Manual"});
		for (int i = 0; i < CHANNELS; i++) {
			configParam(CHANNEL_GAIN_PARAMS + i, -12.f, 12.f, 0.f, string::f("Channel %d gain", i + 1), " dB");
			getParamQuantity(CHANNEL_GAIN_PARAMS + i)->snapEnabled = true;
		}
		for (int i = 0; i < NUM_MODULE_INPUTS; i++)
			configInput(AUDIO_INPUTS + i, string::f("To device output %d", i + 1));
		for (int i = 0; i < NUM_MODULE_OUTPUTS; i++)
			configOutput(AUDIO_OUTPUTS + i, string::f("From device input %d", i + 1));
		for (int i = 0; i < CHANNELS; i++) {
			configLight(CLIP_LIGHTS + i, string::f("Channel %d clip indicator", i + 1));
			for (int seg = 0; seg < VU_SEGMENTS; seg++) {
				configLight(VU_LIGHTS + i * VU_SEGMENTS + seg, string::f("Channel %d meter segment %d", i + 1, seg + 1));
			}
		}
		lightDivider.setDivision(512);
	}

	~SplitAudioModule() {
		port.setDriverId(-1);
	}

	void onReset() override {
		port.setDriverId(-1);
		port.setDeviceId(-1);
		port.clearLock();
		port.autoReconnect = true;
		for (int i = 0; i < CHANNELS; i++)
			vuMeters[i].reset();
		clearClips();
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		port.engineInputBuffer.clear();
		port.engineOutputBuffer.clear();
	}

	void process(const ProcessArgs& args) override {
		if (HAS_GAIN_UI && clipResetTrigger.process(params[CLIP_RESET_PARAM].getValue()))
			clearClips();
		processClipTimers(args.sampleTime);

		if (NUM_MODULE_INPUTS > 0 && port.deviceNumOutputs > 0) {
			dsp::Frame<SplitAudioPort<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>::ENGINE_INPUT_CHANNELS> inputFrame = {};
			for (int i = 0; i < port.deviceNumOutputs; i++) {
				float v = 0.f;
				if (inputs[AUDIO_INPUTS + i].isConnected())
					v = inputs[AUDIO_INPUTS + i].getVoltageSum() / 10.f;
				else if (NUM_MODULE_INPUTS == 2 && i == 1 && inputs[AUDIO_INPUTS + 0].isConnected())
					v = inputs[AUDIO_INPUTS + 0].getVoltageSum() / 10.f;
				float gainedV = applyGain(v, i);
				float meterSample = isMeterPostGain() ? gainedV : v;
				float clipSample = getClipSample(v, gainedV);
				inputFrame.samples[i] = gainedV;
				vuMeters[i].process(args.sampleTime, meterSample);
				detectClip(i, clipSample);
			}
			for (int i = port.deviceNumOutputs; i < CHANNELS; i++)
				vuMeters[i].process(args.sampleTime, 0.f);
			if (!port.engineInputBuffer.full())
				port.engineInputBuffer.push(inputFrame);
		}
		else if (NUM_MODULE_INPUTS > 0) {
			for (int i = 0; i < CHANNELS; i++)
				vuMeters[i].process(args.sampleTime, 0.f);
		}

		if (NUM_MODULE_OUTPUTS > 0 && !port.engineOutputBuffer.empty()) {
			dsp::Frame<SplitAudioPort<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>::ENGINE_OUTPUT_CHANNELS> outputFrame = port.engineOutputBuffer.shift();
			for (int i = 0; i < NUM_MODULE_OUTPUTS; i++) {
				float rawV = outputFrame.samples[i];
				float gainedV = applyGain(rawV, i);
				float meterSample = isMeterPostGain() ? gainedV : rawV;
				float clipSample = getClipSample(rawV, gainedV);
				outputs[AUDIO_OUTPUTS + i].setVoltage(10.f * gainedV);
				vuMeters[i].process(args.sampleTime, meterSample);
				detectClip(i, clipSample);
			}
		}
		else {
			for (int i = 0; i < NUM_MODULE_OUTPUTS; i++) {
				outputs[AUDIO_OUTPUTS + i].setVoltage(0.f);
				vuMeters[i].process(args.sampleTime, 0.f);
			}
		}

		if (lightDivider.process()) {
			for (int i = 0; i < CHANNELS; i++) {
				lights[CLIP_LIGHTS + i].setBrightness(clipLatched[i] ? 1.f : 0.f);
				if (HAS_GAIN_UI) {
					lights[VU_LIGHTS + i * VU_SEGMENTS + 0].setBrightness(vuMeters[i].getBrightness(0.f, 0.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 1].setBrightness(vuMeters[i].getBrightness(-1.f, -0.5f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 2].setBrightness(vuMeters[i].getBrightness(-3.f, -1.5f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 3].setBrightness(vuMeters[i].getBrightness(-6.f, -3.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 4].setBrightness(vuMeters[i].getBrightness(-9.f, -6.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 5].setBrightness(vuMeters[i].getBrightness(-12.f, -9.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 6].setBrightness(vuMeters[i].getBrightness(-18.f, -12.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 7].setBrightness(vuMeters[i].getBrightness(-24.f, -18.f));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 8].setBrightness(vuMeters[i].getBrightness(-36.f, -24.f));
				}
				else {
					lights[VU_LIGHTS + i * VU_SEGMENTS + 0].setBrightness(vuMeters[i].getBrightness(0, 0));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 1].setBrightness(vuMeters[i].getBrightness(-6, -3));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 2].setBrightness(vuMeters[i].getBrightness(-18, -9));
					lights[VU_LIGHTS + i * VU_SEGMENTS + 3].setBrightness(vuMeters[i].getBrightness(-36, -18));
				}
			}
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "audio", port.toJsonLocked());
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* audioJ = json_object_get(rootJ, "audio");
		if (audioJ)
			port.fromJsonLocked(audioJ);
	}
};


template <int NUM_MODULE_INPUTS, int NUM_MODULE_OUTPUTS>
struct SplitAudioWidget : ModuleWidget {
	using TModule = SplitAudioModule<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>;
	std::chrono::steady_clock::time_point nextReconnectCheck = std::chrono::steady_clock::now();
	std::vector<ChannelNumberLabel*> channelLabels;

	std::string getChannelLabelText(TModule* module, int channel) {
		if (!module)
			return string::f("%d", channel + 1);

		const bool isHardwareOutput = (NUM_MODULE_INPUTS > 0);
		int baseChannel = isHardwareOutput ? module->port.outputOffset : module->port.inputOffset;
		int supportedChannels = isHardwareOutput ? module->port.deviceNumOutputs : module->port.deviceNumInputs;

		if (module->port.hasLiveDevice()) {
			if (channel < supportedChannels)
				return string::f("%d", baseChannel + channel + 1);
			return "N/A";
		}

		if (!module->port.desiredDeviceName.empty())
			return string::f("%d", baseChannel + channel + 1);

		return string::f("%d", channel + 1);
	}

	SplitAudioWidget(TModule* module) {
		setModule(module);

		int hp = moduleWidthHp(TModule::CHANNELS);
		box.size = Vec(hp * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);

		SplitPanel* panel = new SplitPanel;
		panel->title = panelTitle(NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS);
		panel->hasMasterSection = TModule::HAS_GAIN_UI;
		panel->masterSectionWidth = TModule::HAS_GAIN_UI ? (6.f * RACK_GRID_WIDTH) : 0.f;
		panel->box.size = box.size;
		addChild(panel);

		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const bool isHardwareOutput = (NUM_MODULE_INPUTS > 0);
		const float displayHeightMm = (TModule::CHANNELS == 2) ? 29.021f : (56.f / 3.f);
		const float displayTopMm = isHardwareOutput ? 88.0f : 13.039f;
		const float displayBottomMm = displayTopMm + displayHeightMm;
		SplitAudioDisplay<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>* display = createWidget<SplitAudioDisplay<NUM_MODULE_INPUTS, NUM_MODULE_OUTPUTS>>(mm2px(Vec(0.0, displayTopMm)));
		display->box.size = Vec(box.size.x, mm2px(displayHeightMm));
		display->setAudioPort(module ? &module->port : NULL);
		addChild(display);
		if (TModule::HAS_GAIN_UI) {
			panel->masterSectionDividerTop = isHardwareOutput ? 34.f : mm2px(displayBottomMm + 4.f);
			panel->masterSectionDividerBottom = isHardwareOutput ? mm2px(displayTopMm - 4.f) : (box.size.y - 12.f);
		}

		float portY = mm2px(isHardwareOutput ? 23.0f : 105.5f);
		float numberY = isHardwareOutput ? mm2px(35.0f) : mm2px(82.0f);
		float gainY = mm2px(isHardwareOutput ? 67.0f : 56.0f);
		float topMeterY = mm2px(isHardwareOutput ? 23.0f : 80.0f);
		float meterSpacing = mm2px(4.2f);
		float clipLightY = topMeterY - mm2px(5.8f);
		const float masterStripWidth = TModule::HAS_GAIN_UI ? (6.f * RACK_GRID_WIDTH) : 0.f;
		if (TModule::HAS_GAIN_UI) {
			if (isHardwareOutput) {
				portY = mm2px(15.5f);
				numberY = mm2px(22.5f);
				gainY = mm2px(30.5f);
				topMeterY = mm2px(52.0f);
				meterSpacing = mm2px(3.725f);
			}
			else {
				clipLightY = mm2px(47.5f);
				topMeterY = mm2px(53.0f);
				meterSpacing = mm2px(3.825f);
				gainY = mm2px(103.0f);
				numberY = mm2px(111.0f);
				portY = mm2px(118.0f);
			}
			if (isHardwareOutput)
				clipLightY = mm2px(45.5f);
		}
		const float meterBottomY = topMeterY + meterSpacing * (TModule::VU_SEGMENTS - 1);
		const float channelAreaWidth = box.size.x - masterStripWidth;
		const float baseChannelMargin = mm2px((TModule::CHANNELS == 16) ? 6.5f : 8.2f);
		const float leftChannelMargin = masterStripWidth + baseChannelMargin;
		const float rightChannelMargin = mm2px((TModule::CHANNELS == 16) ? 6.5f : 7.0f);
		const float xSpacing = (TModule::CHANNELS > 1) ? (channelAreaWidth - baseChannelMargin - rightChannelMargin) / (TModule::CHANNELS - 1) : 0.f;

		if (TModule::HAS_GAIN_UI) {
			const float masterX = masterStripWidth * 0.5f;
			const float masterTextX = 2.f;
			const float masterTextWidth = masterX - masterTextX - 10.f;
			const float masterGainY = gainY;
			const float clipResetY = clipLightY;
			const float clipModeY = topMeterY + meterSpacing * 2.5f;
			const float meterSwitchY = topMeterY + meterSpacing * 6.9f;

			ChannelNumberLabel* masterLabel = createWidget<ChannelNumberLabel>(Vec(0.f, numberY - 7.f));
			masterLabel->box.size = Vec(masterStripWidth, 14.f);
			masterLabel->text = "MAIN";
			addChild(masterLabel);

			addParam(createParamCentered<RoundLargeBlackKnob>(Vec(masterX, masterGainY), module, TModule::MASTER_GAIN_PARAM));

			SmallTextLabel* preLabel = createWidget<SmallTextLabel>(Vec(masterX + 10.f, meterSwitchY - 15.f));
			preLabel->box.size = Vec(28.f, 10.f);
			preLabel->text = "PRE";
			preLabel->fontSize = 8.5f;
			preLabel->leftAlign = true;
			preLabel->color = settings::preferDarkPanels ? nvgRGB(176, 176, 176) : nvgRGB(96, 96, 96);
			addChild(preLabel);

			SmallTextLabel* postLabel = createWidget<SmallTextLabel>(Vec(masterX + 10.f, meterSwitchY + 5.f));
			postLabel->box.size = Vec(32.f, 10.f);
			postLabel->text = "POST";
			postLabel->fontSize = 8.5f;
			postLabel->leftAlign = true;
			postLabel->color = settings::preferDarkPanels ? nvgRGB(176, 176, 176) : nvgRGB(96, 96, 96);
			addChild(postLabel);

			addParam(createParamCentered<CKSS>(Vec(masterX, meterSwitchY), module, TModule::METER_TAP_PARAM));

			SmallTextLabel* autoLabel = createWidget<SmallTextLabel>(Vec(masterTextX, clipModeY - 11.f));
			autoLabel->box.size = Vec(masterTextWidth, 10.f);
			autoLabel->text = "Auto";
			autoLabel->fontSize = 8.5f;
			autoLabel->rightAlign = true;
			autoLabel->color = settings::preferDarkPanels ? nvgRGB(198, 198, 198) : nvgRGB(82, 82, 82);
			addChild(autoLabel);

			SmallTextLabel* autoResetLabel = createWidget<SmallTextLabel>(Vec(masterTextX, clipModeY + 2.f));
			autoResetLabel->box.size = Vec(masterTextWidth, 10.f);
			autoResetLabel->text = "Reset";
			autoResetLabel->fontSize = 8.5f;
			autoResetLabel->rightAlign = true;
			autoResetLabel->color = settings::preferDarkPanels ? nvgRGB(198, 198, 198) : nvgRGB(82, 82, 82);
			addChild(autoResetLabel);

			SmallTextLabel* auto3Label = createWidget<SmallTextLabel>(Vec(masterX + 10.f, clipModeY - 12.f));
			auto3Label->box.size = Vec(28.f, 9.f);
			auto3Label->text = "OFF";
			auto3Label->fontSize = 8.0f;
			auto3Label->leftAlign = true;
			auto3Label->color = settings::preferDarkPanels ? nvgRGB(176, 176, 176) : nvgRGB(96, 96, 96);
			addChild(auto3Label);

			SmallTextLabel* auto10Label = createWidget<SmallTextLabel>(Vec(masterX + 10.f, clipModeY - 1.f));
			auto10Label->box.size = Vec(28.f, 9.f);
			auto10Label->text = "10s";
			auto10Label->fontSize = 8.0f;
			auto10Label->leftAlign = true;
			auto10Label->color = settings::preferDarkPanels ? nvgRGB(176, 176, 176) : nvgRGB(96, 96, 96);
			addChild(auto10Label);

			SmallTextLabel* autoOffLabel = createWidget<SmallTextLabel>(Vec(masterX + 10.f, clipModeY + 10.f));
			autoOffLabel->box.size = Vec(28.f, 9.f);
			autoOffLabel->text = "3s";
			autoOffLabel->fontSize = 8.0f;
			autoOffLabel->leftAlign = true;
			autoOffLabel->color = settings::preferDarkPanels ? nvgRGB(176, 176, 176) : nvgRGB(96, 96, 96);
			addChild(autoOffLabel);

			addParam(createParamCentered<CKSSThree>(Vec(masterX, clipModeY), module, TModule::CLIP_RESET_TIME_PARAM));

			SmallTextLabel* manualLabel = createWidget<SmallTextLabel>(Vec(masterTextX, clipResetY - 11.f));
			manualLabel->box.size = Vec(masterTextWidth, 10.f);
			manualLabel->text = "Manual";
			manualLabel->fontSize = 8.5f;
			manualLabel->rightAlign = true;
			manualLabel->color = settings::preferDarkPanels ? nvgRGB(198, 198, 198) : nvgRGB(82, 82, 82);
			addChild(manualLabel);

			SmallTextLabel* resetLabel = createWidget<SmallTextLabel>(Vec(masterTextX, clipResetY + 2.f));
			resetLabel->box.size = Vec(masterTextWidth, 10.f);
			resetLabel->text = "Reset";
			resetLabel->fontSize = 8.5f;
			resetLabel->rightAlign = true;
			resetLabel->color = settings::preferDarkPanels ? nvgRGB(198, 198, 198) : nvgRGB(82, 82, 82);
			addChild(resetLabel);

			addParam(createParamCentered<TL1105>(Vec(masterX, clipResetY), module, TModule::CLIP_RESET_PARAM));
		}

		for (int i = 0; i < TModule::CHANNELS; i++) {
			Vec pos = Vec(leftChannelMargin + i * xSpacing, portY);
			if (NUM_MODULE_INPUTS > 0)
				addInput(createInputCentered<ThemedPJ301MPort>(pos, module, TModule::AUDIO_INPUTS + i));
			if (NUM_MODULE_OUTPUTS > 0)
				addOutput(createOutputCentered<ThemedPJ301MPort>(pos, module, TModule::AUDIO_OUTPUTS + i));

			ChannelNumberLabel* label = createWidget<ChannelNumberLabel>(Vec(pos.x - 10.f, numberY - 7.f));
			label->box.size = Vec(20.f, 14.f);
			label->text = getChannelLabelText(module, i);
			addChild(label);
			channelLabels.push_back(label);

			if (TModule::HAS_GAIN_UI)
				addChild(createLightCentered<SmallLight<RedLight>>(Vec(pos.x, clipLightY), module, TModule::CLIP_LIGHTS + i));
			if (TModule::HAS_GAIN_UI) {
				for (int seg = 0; seg < TModule::VU_SEGMENTS; seg++) {
					Vec lightPos = Vec(pos.x, topMeterY + meterSpacing * seg);
					int lightId = TModule::VU_LIGHTS + i * TModule::VU_SEGMENTS + seg;
					if (seg == 0)
						addChild(createLightCentered<VuSimpleLight<RedLight>>(lightPos, module, lightId));
					else if (seg == 1)
						addChild(createLightCentered<VuSimpleLight<MagentaLight>>(lightPos, module, lightId));
					else if (seg < 4)
						addChild(createLightCentered<VuSimpleLight<BlueLight>>(lightPos, module, lightId));
					else
						addChild(createLightCentered<VuSimpleLight<GreenLight>>(lightPos, module, lightId));
				}
			}
			else {
				addChild(createLightCentered<SmallSimpleLight<RedLight>>(Vec(pos.x, topMeterY), module, TModule::VU_LIGHTS + i * TModule::VU_SEGMENTS + 0));
				addChild(createLightCentered<SmallSimpleLight<YellowLight>>(Vec(pos.x, meterBottomY - meterSpacing * 2.f), module, TModule::VU_LIGHTS + i * TModule::VU_SEGMENTS + 1));
				addChild(createLightCentered<SmallSimpleLight<GreenLight>>(Vec(pos.x, meterBottomY - meterSpacing), module, TModule::VU_LIGHTS + i * TModule::VU_SEGMENTS + 2));
				addChild(createLightCentered<SmallSimpleLight<GreenLight>>(Vec(pos.x, meterBottomY), module, TModule::VU_LIGHTS + i * TModule::VU_SEGMENTS + 3));
			}

			if (TModule::HAS_GAIN_UI) {
				addParam(createParamCentered<BefacoTinyKnob>(Vec(pos.x, gainY), module, TModule::CHANNEL_GAIN_PARAMS + i));
			}
		}
	}

	void step() override {
		TModule* module = getModule<TModule>();
		if (module) {
			auto now = std::chrono::steady_clock::now();
			if (now >= nextReconnectCheck) {
				module->port.pollConnectionState();
				if (module->port.hasLiveDevice())
					module->port.syncLockFromDevice();
				module->port.reconnectIfAvailable();
				nextReconnectCheck = now + std::chrono::seconds(1);
			}
			for (size_t i = 0; i < channelLabels.size(); i++) {
				channelLabels[i]->text = getChannelLabelText(module, (int) i);
			}
		}
		ModuleWidget::step();
	}

	void appendContextMenu(Menu* menu) override {
		TModule* module = getModule<TModule>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);

		if (!module->port.desiredDeviceName.empty())
			menu->addChild(createMenuLabel("Locked device: " + module->port.desiredDeviceName));

		menu->addChild(createBoolMenuItem("Auto-reconnect", "",
			[=]() { return module->port.autoReconnect; },
			[=](bool enabled) { module->port.autoReconnect = enabled; }
		));

		menu->addChild(createMenuItem("Reconnect now", "",
			[=]() { module->port.reconnectIfAvailable(); }
		));

		menu->addChild(createMenuItem("Forget locked device", "",
			[=]() { module->port.clearLock(); }
		));

		menu->addChild(createMenuItem("Disconnect and forget", "",
			[=]() {
				module->port.clearLock();
				module->port.setDeviceId(-1);
			}
		));

		menu->addChild(createBoolMenuItem("Master audio module", "",
			[=]() { return module->port.isMaster(); },
			[=](bool master) { module->port.setMaster(master); }
		));
	}
};


using AudioIn2 = SplitAudioModule<0, 2>;
using AudioIn8 = SplitAudioModule<0, 8>;
using AudioIn16 = SplitAudioModule<0, 16>;
using AudioOut2 = SplitAudioModule<2, 0>;
using AudioOut8 = SplitAudioModule<8, 0>;
using AudioOut16 = SplitAudioModule<16, 0>;

using AudioIn2Widget = SplitAudioWidget<0, 2>;
using AudioIn8Widget = SplitAudioWidget<0, 8>;
using AudioIn16Widget = SplitAudioWidget<0, 16>;
using AudioOut2Widget = SplitAudioWidget<2, 0>;
using AudioOut8Widget = SplitAudioWidget<8, 0>;
using AudioOut16Widget = SplitAudioWidget<16, 0>;


} // namespace


Model* modelAudioIn2 = createModel<AudioIn2, AudioIn2Widget>("AudioIn2");
Model* modelAudioIn8 = createModel<AudioIn8, AudioIn8Widget>("AudioIn8");
Model* modelAudioIn16 = createModel<AudioIn16, AudioIn16Widget>("AudioIn16");
Model* modelAudioOut2 = createModel<AudioOut2, AudioOut2Widget>("AudioOut2");
Model* modelAudioOut8 = createModel<AudioOut8, AudioOut8Widget>("AudioOut8");
Model* modelAudioOut16 = createModel<AudioOut16, AudioOut16Widget>("AudioOut16");
