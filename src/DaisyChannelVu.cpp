#include "QuantalAudioExtendedMixer.hpp"
#include "Daisy.hpp"

static const int VU_LIGHT_COUNT = 32;

/** Returns the sum of all voltages. */
float _getVoltageSum(int channels, float voltages[16]) {
    float sum = 0.f;
    for (int c = 0; c < channels; c++) {
        sum += voltages[c];
    }
    return sum;
}

struct DaisyChannelVu : Module {
    enum ParamIds {
        NUM_PARAMS
    };
    enum InputIds {
        NUM_INPUTS
    };
    enum OutputIds {
        NUM_OUTPUTS
    };
    enum LightsIds {
        LINK_LIGHT_L,
        LINK_LIGHT_R,
        ENUMS(VU_LIGHTS_L, VU_LIGHT_COUNT + 8 + 4),
        ENUMS(VU_LIGHTS_R, VU_LIGHT_COUNT + 8 + 4),
        NUM_LIGHTS
    };

    float link_l = 0.f;
    float link_r = 0.f;

    dsp::ClockDivider lightDivider;
    dsp::VuMeter2 vuMeter[2];

    DaisyMessage daisyInputMessage[2][1];
    DaisyMessage daisyOutputMessage[2][1];

    DaisyChannelVu() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configLight(LINK_LIGHT_L, "Daisy chain link input");
        configLight(LINK_LIGHT_R, "Daisy chain link output");

        // Set the expander messages
        leftExpander.producerMessage = &daisyInputMessage[0];
        leftExpander.consumerMessage = &daisyInputMessage[1];
        rightExpander.producerMessage = &daisyOutputMessage[0];
        rightExpander.consumerMessage = &daisyOutputMessage[1];

        lightDivider.setDivision(512);
    }

    void process(const ProcessArgs &args) override {

        float signals_l[16] = {};
        float signals_r[16] = {};
        float daisySignals_l[16] = {};
        float daisySignals_r[16] = {};
        int chainChannels = 1;

        // Get daisy-chained data from left-side linked module
        if (leftExpander.module && (
            leftExpander.module->model == modelDaisyChannel2
            || leftExpander.module->model == modelDaisyChannelVu
            || leftExpander.module->model == modelDaisyChannelSends2
            || leftExpander.module->model == modelDaisyChannelSends3
            || leftExpander.module->model == modelDaisyMaster2
            || leftExpander.module->model == modelDaisyBlank1
            || leftExpander.module->model == modelDaisyBlank2
        )) {
            DaisyMessage *msgFromModule = (DaisyMessage *)(leftExpander.consumerMessage);
            chainChannels = msgFromModule->channels;
            for (int c = 0; c < chainChannels; c++) {
                daisySignals_l[c] = msgFromModule->voltages_l[c];
                daisySignals_r[c] = msgFromModule->voltages_r[c];
            }

            // Get the immediate signal from single channel strip
            for (int c = 0; c < msgFromModule->single_channels; c++) {
                signals_l[c] = msgFromModule->single_voltages_l[c];
                signals_r[c] = msgFromModule->single_voltages_r[c];
            }
            vuMeter[0].process(args.sampleTime, _getVoltageSum(chainChannels, signals_l) / 10.f);
            vuMeter[1].process(args.sampleTime, _getVoltageSum(chainChannels, signals_r) / 10.f);

            link_l = 0.8f;
        } else {
            vuMeter[0].process(args.sampleTime, 0.0f);
            vuMeter[1].process(args.sampleTime, 0.0f);
            link_l = 0.0f;
        }

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

            msgToModule->channels = chainChannels;
            for (int c = 0; c < chainChannels; c++) {
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
            for (int i = VU_LIGHT_COUNT + 8 + 3; i >= 0; i--) {
                lights[VU_LIGHTS_L + i].setBrightness(vuMeter[0].getBrightness(-60.f + 1.5f * i + 1, -60 + 1.5f * i));
                lights[VU_LIGHTS_R + i].setBrightness(vuMeter[1].getBrightness(-60.f + 1.5f * i + 1, -60 + 1.5f * i));
            }
            lights[LINK_LIGHT_L].setBrightness(link_l);
            lights[LINK_LIGHT_R].setBrightness(link_r);
        }
    }
};

struct DaisyChannelVuWidget : ModuleWidget {
    DaisyChannelVuWidget(DaisyChannelVu *module) {
        setModule(module);
        //setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/DaisyChannelVu.svg")));
        setPanel(createPanel(asset::plugin(pluginInstance, "res/DaisyChannelVu.svg"), asset::plugin(pluginInstance, "res/DaisyChannelVu-dark.svg")));

        // Screws
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Link lights
        addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH/2 - 3, 361.0f), module, DaisyChannelVu::LINK_LIGHT_L));
        addChild(createLightCentered<TinyLight<YellowLight>>(Vec(RACK_GRID_WIDTH/2 + 3, 361.0f), module, DaisyChannelVu::LINK_LIGHT_R));

        for (int i = 0; i < VU_LIGHT_COUNT; i++) {
            addChild(createLightCentered<VCVSliderLight<GreenLight>>(Vec(RACK_GRID_WIDTH/2 - 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_L + i));
            addChild(createLightCentered<VCVSliderLight<GreenLight>>(Vec(RACK_GRID_WIDTH/2 + 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_R + i));
        }
        for (int i = VU_LIGHT_COUNT; i < VU_LIGHT_COUNT + 8; i++) {
            addChild(createLightCentered<VCVSliderLight<YellowLight>>(Vec(RACK_GRID_WIDTH/2 - 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_L + i));
            addChild(createLightCentered<VCVSliderLight<YellowLight>>(Vec(RACK_GRID_WIDTH/2 + 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_R + i));
        }
        for (int i = VU_LIGHT_COUNT + 8; i < VU_LIGHT_COUNT + 12; i++) {
            addChild(createLightCentered<VCVSliderLight<RedLight>>(Vec(RACK_GRID_WIDTH/2 - 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_L + i));
            addChild(createLightCentered<VCVSliderLight<RedLight>>(Vec(RACK_GRID_WIDTH/2 + 3.f, 339.f - i * 7), module, DaisyChannelVu::VU_LIGHTS_R + i));
        }
    }
};

Model *modelDaisyChannelVu = createModel<DaisyChannelVu, DaisyChannelVuWidget>("DaisyChannelVu");
