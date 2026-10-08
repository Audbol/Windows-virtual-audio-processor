#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace vb
{

/**
    Serial chain of hosted plugins (VST3) processing a stereo voice signal.

    Threading: process() runs on the audio thread and holds 'lock' for the
    duration of the block (like juce::AudioProcessorPlayer). Every structural
    change on the message thread prepares the plugin *outside* the lock and
    only takes the lock for the pointer shuffle, so edits cause at most one
    skipped block, and plugin instantiation never blocks audio.
*/
class PluginChain : public juce::ChangeBroadcaster
{
public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::PluginDescription description;
        std::atomic<bool> bypassed { false };
        int numInputs = 2, numOutputs = 2;
    };

    PluginChain();
    ~PluginChain() override;

    //==============================================================================
    // Audio thread / device lifecycle
    void prepare (double sampleRate, int maxBlockSize);
    void release();
    void process (juce::AudioBuffer<float>& stereoBuffer);

    //==============================================================================
    // Message thread
    /** Configures buses, applies optional saved state, prepares, then inserts at the end.
        'description' should be the one the instance was created from (the scanned one):
        an instance's own getPluginDescription() may point inside the .vst3 bundle,
        which can't be used to re-create it later. */
    void addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin,
                    const juce::PluginDescription& description,
                    bool bypassed = false,
                    const juce::MemoryBlock* state = nullptr);
    void removePlugin (int index);
    void movePlugin (int fromIndex, int toIndex);
    void setBypassed (int index, bool shouldBypass);
    void clear();

    int size() const                              { return (int) slots.size(); }
    Slot* getSlot (int index) const               { return juce::isPositiveAndBelow (index, size()) ? slots[(size_t) index].get() : nullptr; }

    /** Sum of the latency reported by all active plugins, in samples. */
    int getTotalLatencySamples() const;

    std::unique_ptr<juce::XmlElement> createStateXml() const;

    /** Rebuilds the chain. Plugins that fail to load are skipped and listed in 'errors'. */
    void restoreFromXml (const juce::XmlElement& xml, juce::AudioPluginFormatManager& formats, juce::StringArray& errors);

private:
    static void configureLayout (juce::AudioPluginInstance& plugin);
    void prepareSlot (Slot& slot) const;

    juce::CriticalSection lock;
    std::vector<std::unique_ptr<Slot>> slots;   // modified only under 'lock'

    double currentRate = 48000.0;
    int currentBlock = 512;
    bool prepared = false;

    static constexpr int kMaxPluginChannels = 32;
    juce::AudioBuffer<float> scratch;
    juce::Array<float*> channelPointers;
    juce::MidiBuffer midi;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace vb
