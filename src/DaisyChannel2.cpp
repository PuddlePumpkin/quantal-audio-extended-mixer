#include "QuantalAudio.hpp"

struct DaisyMessage {
	int channels;
    float voltages_l[16];
    float voltages_r[16];

    DaisyMessage() {
        // Init defaults
        channels = 1;
        for (int c = 0; c < 16; c++) {
            voltages_l[c] = 0.0f;
            voltages_r[c] = 0.0f;
        }
    }
};

struct DaisyChannel2 : Module {
    enum ParamIds {
        CH_LVL_PARAM,
        MUTE_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CH_INPUT_1, // Left
        CH_INPUT_2, // Right
        LVL_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        CH_OUTPUT_1, // Left
        CH_OUTPUT_2, // Right
        NUM_OUTPUTS
    };
    enum LightsIds {
        MUTE_LIGHT,
        LINK_LIGHT_L,
        LINK_LIGHT_R,
        NUM_LIGHTS
    };

    // Hypothetically the max number of channels that could be chained
    // Needs to match the divisor in the daisy master class
    float DAISY_DIVISOR = 16.f;
    bool muted = false;
    bool link_l = 0.f;
    bool link_r = 0.f;
    dsp::SchmittTrigger muteTrigger;

    DaisyMessage daisyInputMessage[2][1];
    DaisyMessage daisyOutputMessage[2][1];

    DaisyChannel2() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(CH_LVL_PARAM, 0.0f, 1.0f, 1.0f, "Channel level", " dB", -10, 20);
        configButton(MUTE_PARAM, "Mute");

        configInput(CH_INPUT_1, "Channel L");
        configInput(CH_INPUT_2, "Channel R");
        configInput(LVL_CV_INPUT, "Level CV");

        configOutput(CH_OUTPUT_1, "Channel L");
        configOutput(CH_OUTPUT_2, "Channel R");

		configLight(LINK_LIGHT_L, "Daisy chain link input");
		configLight(LINK_LIGHT_R, "Daisy chain link output");

        // Set the expander messages
		leftExpander.producerMessage = daisyInputMessage[0];
		leftExpander.consumerMessage = daisyInputMessage[1];
		rightExpander.producerMessage = daisyOutputMessage[0];
		rightExpander.consumerMessage = daisyOutputMessage[1];
    }

    json_t *dataToJson() override {
        json_t *rootJ = json_object();

        // mute
        json_object_set_new(rootJ, "muted", json_boolean(muted));

        return rootJ;
    }

    void dataFromJson(json_t *rootJ) override {
        // mute
        json_t *mutedJ = json_object_get(rootJ, "muted");
        if (mutedJ)
            muted = json_is_true(mutedJ);
    }

    void process(const ProcessArgs &args) override {
        if (muteTrigger.process(params[MUTE_PARAM].getValue())) {
            muted = !muted;
        }

        float signals_l[16] = {};
        float signals_r[16] = {};
        float daisySignals_l[16] = {};
        float daisySignals_r[16] = {};
        int channels = 1;
        int maxChannels = 1;
        int chainChannels = 1;

        // Get inputs from this channel strip
        if (!muted) {
            float gain = params[CH_LVL_PARAM].getValue();

            channels = std::max(inputs[CH_INPUT_1].getChannels(), inputs[CH_INPUT_2].getChannels());
            inputs[CH_INPUT_1].readVoltages(signals_l);
            inputs[CH_INPUT_2].readVoltages(signals_r);
            for (int c = 0; c < channels; c++) {
                signals_l[c] *= std::pow(gain, 2.f);
                signals_r[c] *= std::pow(gain, 2.f);
            }

            if (inputs[LVL_CV_INPUT].isConnected()) {
                float _cv = clamp(inputs[LVL_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
                for (int c = 0; c < channels; c++) {
                    signals_l[c] *= _cv;
                    signals_r[c] *= _cv;
                }
            }
        }

        // Set output for this channel strip
        outputs[CH_OUTPUT_1].setChannels(channels);
        outputs[CH_OUTPUT_1].writeVoltages(signals_l);

        outputs[CH_OUTPUT_2].setChannels(channels);
        outputs[CH_OUTPUT_2].writeVoltages(signals_r);

        // Get daisy-chained data from left-side linked module
        if (leftExpander.module && leftExpander.module->model == modelDaisyChannel2) {
            DaisyMessage *msgFromModule = (DaisyMessage *)(leftExpander.module->rightExpander.consumerMessage);
            chainChannels = msgFromModule->channels;
            for (int c = 0; c < chainChannels; c++) {
                daisySignals_l[c] = msgFromModule->voltages_l[c];
                daisySignals_r[c] = msgFromModule->voltages_r[c];
            }
            link_l = 0.1f;
        } else {
            link_l = 0.0f;
        }

        maxChannels = std::max(chainChannels, channels);

        // Set daisy-chained output to right-side linked module
        if (rightExpander.module && (rightExpander.module->model == modelDaisyMaster2 || rightExpander.module->model == modelDaisyChannel2)) {
            DaisyMessage *msgToModule = (DaisyMessage *)(rightExpander.consumerMessage);

            // Make the voltage small to the chain by dividing by the divisor;
            for (int c = 0; c < maxChannels; c++) {
                daisySignals_l[c] += signals_l[c] / DAISY_DIVISOR;
                daisySignals_r[c] += signals_r[c] / DAISY_DIVISOR;
            }

            msgToModule->channels = maxChannels;
            for (int c = 0; c < maxChannels; c++) {
                msgToModule->voltages_l[c] = daisySignals_l[c];
                msgToModule->voltages_r[c] = daisySignals_r[c];
            }

            link_r = 0.1f;
        } else {
            link_r = 0.0f;
        }

        // Set lights
        lights[MUTE_LIGHT].value = (muted);
		lights[LINK_LIGHT_L].setBrightness(link_l);
		lights[LINK_LIGHT_R].setBrightness(link_r);
    }
};

struct DaisyChannelWidget2 : ModuleWidget {
    DaisyChannelWidget2(DaisyChannel2 *module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/DaisyChannel2.svg")));

        // Screws
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Channel Input/Output
        addInput(createInput<PJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 50.0), module, DaisyChannel2::CH_INPUT_1));
        addInput(createInput<PJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 76.0), module, DaisyChannel2::CH_INPUT_2));
        addOutput(createOutput<PJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 290.0), module, DaisyChannel2::CH_OUTPUT_1));
        addOutput(createOutput<PJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 316.0), module, DaisyChannel2::CH_OUTPUT_2));

        // Level & CV
        addParam(createParam<LEDSliderGreen>(Vec(RACK_GRID_WIDTH - 10.5, 147.4), module, DaisyChannel2::CH_LVL_PARAM));
        addInput(createInput<PJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 115.0), module, DaisyChannel2::LVL_CV_INPUT));

        // Mute
        addParam(createParam<LEDButton>(Vec(RACK_GRID_WIDTH - 9.0, 230.0), module, DaisyChannel2::MUTE_PARAM));
        addChild(createLight<MediumLight<RedLight>>(Vec(RACK_GRID_WIDTH - 4.5, 234.25f), module, DaisyChannel2::MUTE_LIGHT));

        // Link lights
		addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH - 4, 361.0f), module, DaisyChannel2::LINK_LIGHT_L));
		addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH + 4, 361.0f), module, DaisyChannel2::LINK_LIGHT_R));
    }
};

Model *modelDaisyChannel2 = createModel<DaisyChannel2, DaisyChannelWidget2>("DaisyChannel2");
