#include "QuantalAudioExtendedMixer.hpp"
#include "Daisy.hpp"

struct DaisyChannel2 : Module {
    enum ParamIds {
        CH_LVL_PARAM,
        MUTE_PARAM,
        PAN_PARAM,
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

    bool muted = false;
    float link_l = 0.f;
    float link_r = 0.f;
    dsp::ClockDivider lightDivider;

    DaisyMessage daisyInputMessage[2][1];
    DaisyMessage daisyOutputMessage[2][1];

    DaisyChannel2() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(CH_LVL_PARAM, 0.0f, 1.0f, 1.0f, "Channel level", " dB", -10, 20);
        configParam(PAN_PARAM, -1.0f, 1.0f, 0.0f, "Panning", "%", 0.f, 100.f);
        configSwitch(MUTE_PARAM, 0.f, 1.f, 0.f, "Mute", {"Not muted", "Muted"});

        configInput(CH_INPUT_1, "Channel L");
        configInput(CH_INPUT_2, "Channel R");
        configInput(LVL_CV_INPUT, "Level CV");

        configOutput(CH_OUTPUT_1, "Channel L");
        configOutput(CH_OUTPUT_2, "Channel R");

        configLight(LINK_LIGHT_L, "Daisy chain link input");
        configLight(LINK_LIGHT_R, "Daisy chain link output");

        // Set the expander messages
        leftExpander.producerMessage = &daisyInputMessage[0];
        leftExpander.consumerMessage = &daisyInputMessage[1];
        rightExpander.producerMessage = &daisyOutputMessage[0];
        rightExpander.consumerMessage = &daisyOutputMessage[1];

        lightDivider.setDivision(512);
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
        muted = params[MUTE_PARAM].getValue() > 0.f;

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
            float pan = params[PAN_PARAM].getValue();

            channels = std::max(inputs[CH_INPUT_1].getChannels(), inputs[CH_INPUT_2].getChannels());

            inputs[CH_INPUT_1].readVoltages(signals_l);
            if (inputs[CH_INPUT_2].isConnected()) {
                inputs[CH_INPUT_2].readVoltages(signals_r);
            } else {
                // Copy signals from ch1 into ch2
                inputs[CH_INPUT_1].readVoltages(signals_r);
            }

            for (int c = 0; c < channels; c++) {
                signals_l[c] *= std::pow(gain, 2.f) * std::cos(M_PI * (pan + 1) / 4);
                signals_r[c] *= std::pow(gain, 2.f) * std::sin(M_PI * (pan + 1) / 4);
            }

            if (inputs[LVL_CV_INPUT].isConnected()) {
                for (int c = 0; c < channels; c++) {
                    float _cv = clamp(inputs[LVL_CV_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);
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
        if (leftExpander.module && (
            leftExpander.module->model == modelDaisyChannel2
            || leftExpander.module->model == modelDaisyChannelVu
            || leftExpander.module->model == modelDaisyChannelSends2
            || leftExpander.module->model == modelDaisyChannelSends3
            || leftExpander.module->model == modelDaisyBlank1
            || leftExpander.module->model == modelDaisyBlank2
        )) {
            DaisyMessage *msgFromModule = (DaisyMessage *)(leftExpander.consumerMessage);
            chainChannels = msgFromModule->channels;
            for (int c = 0; c < chainChannels; c++) {
                daisySignals_l[c] = msgFromModule->voltages_l[c];
                daisySignals_r[c] = msgFromModule->voltages_r[c];
            }
            link_l = 0.8f;
        } else {
            link_l = 0.0f;
        }

        maxChannels = std::max(chainChannels, channels);

        // Set daisy-chained output to right-side linked module
        if (rightExpander.module && (
            rightExpander.module->model == modelDaisyMaster2
            || rightExpander.module->model == modelDaisyChannel2
            || rightExpander.module->model == modelDaisyChannelVu
            || rightExpander.module->model == modelDaisyChannelSends2
            || rightExpander.module->model == modelDaisyChannelSends3
            || rightExpander.module->model == modelDaisyBlank1
            || rightExpander.module->model == modelDaisyBlank2
        )) {
            DaisyMessage *msgToModule = (DaisyMessage *)(rightExpander.module->leftExpander.producerMessage);

            // Write this module's output to the producer message
            msgToModule->single_channels = channels;
            for (int c = 0; c < channels; c++) {
                msgToModule->single_voltages_l[c] = signals_l[c];
                msgToModule->single_voltages_r[c] = signals_r[c];
            }

            // Combine this module's signal with daisy-chain
            for (int c = 0; c < maxChannels; c++) {
                daisySignals_l[c] += signals_l[c] / DAISY_DIVISOR;
                daisySignals_r[c] += signals_r[c] / DAISY_DIVISOR;
            }

            // Write daisy-chain signal to producer message
            msgToModule->channels = maxChannels;
            for (int c = 0; c < maxChannels; c++) {
                msgToModule->voltages_l[c] = daisySignals_l[c];
                msgToModule->voltages_r[c] = daisySignals_r[c];
            }

            rightExpander.module->leftExpander.messageFlipRequested = true;

            link_r = 0.8f;
        } else {
            link_r = 0.0f;
        }

        // Set lights
        if (lightDivider.process()) {
            lights[MUTE_LIGHT].value = (muted);
            lights[LINK_LIGHT_L].setBrightness(link_l);
            lights[LINK_LIGHT_R].setBrightness(link_r);
        }
    }
};

struct DaisyChannelWidget2 : ModuleWidget {
    DaisyChannelWidget2(DaisyChannel2 *module) {
        setModule(module);
        //setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/DaisyChannel2.svg")));
        setPanel(createPanel(asset::plugin(pluginInstance, "res/DaisyChannel2.svg"), asset::plugin(pluginInstance, "res/DaisyChannel2-dark.svg")));
        // Screws
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Channel Input/Output
        addInput(createInput<ThemedPJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 45.0), module, DaisyChannel2::CH_INPUT_1));
        addInput(createInput<ThemedPJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 71.0), module, DaisyChannel2::CH_INPUT_2));
        addOutput(createOutput<ThemedPJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 290.0), module, DaisyChannel2::CH_OUTPUT_1));
        addOutput(createOutput<ThemedPJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 316.0), module, DaisyChannel2::CH_OUTPUT_2));

        // Level & CV
        addInput(createInput<ThemedPJ301MPort>(Vec(RACK_GRID_WIDTH - 12.5, 110.0), module, DaisyChannel2::LVL_CV_INPUT));
        addParam(createParam<LEDSliderGreen>(Vec(RACK_GRID_WIDTH - 10.5, 138.4), module, DaisyChannel2::CH_LVL_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(RACK_GRID_WIDTH - 0, 240.0), module, DaisyChannel2::PAN_PARAM));

        // Mute
        addParam(createLightParam<VCVLightLatch<MediumSimpleLight<RedLight>>>(Vec(RACK_GRID_WIDTH - 9.0, 254.0), module, DaisyChannel2::MUTE_PARAM, DaisyChannel2::MUTE_LIGHT));

        // Link lights
        addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH - 4, 361.0f), module, DaisyChannel2::LINK_LIGHT_L));
        addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH + 4, 361.0f), module, DaisyChannel2::LINK_LIGHT_R));
    }
};

Model *modelDaisyChannel2 = createModel<DaisyChannel2, DaisyChannelWidget2>("DaisyChannel2");
