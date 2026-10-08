#include "PluginChain.h"

namespace vb
{

PluginChain::PluginChain()
{
    scratch.setSize (kMaxPluginChannels, currentBlock);
    channelPointers.resize (kMaxPluginChannels);
}

PluginChain::~PluginChain()
{
    clear();
}

//==============================================================================
void PluginChain::prepare (double sampleRate, int maxBlockSize)
{
    const juce::ScopedLock sl (lock);

    currentRate = sampleRate;
    currentBlock = juce::jmax (1, maxBlockSize);
    scratch.setSize (kMaxPluginChannels, currentBlock, false, true, false);
    midi.ensureSize (256);

    for (auto& slot : slots)
        prepareSlot (*slot);

    prepared = true;
}

void PluginChain::release()
{
    const juce::ScopedLock sl (lock);

    for (auto& slot : slots)
        slot->plugin->releaseResources();

    prepared = false;
}

void PluginChain::prepareSlot (Slot& slot) const
{
    auto& p = *slot.plugin;
    p.setNonRealtime (false);
    p.setRateAndBufferSizeDetails (currentRate, currentBlock);
    p.prepareToPlay (currentRate, currentBlock);
    slot.numInputs = p.getTotalNumInputChannels();
    slot.numOutputs = p.getTotalNumOutputChannels();
}

void PluginChain::configureLayout (juce::AudioPluginInstance& plugin)
{
    using Set = juce::AudioChannelSet;

    auto tryMainBuses = [&plugin] (const Set& in, const Set& out)
    {
        auto layout = plugin.getBusesLayout();

        for (int i = 0; i < layout.inputBuses.size(); ++i)
            layout.inputBuses.getReference (i) = (i == 0 ? in : Set::disabled());

        for (int i = 0; i < layout.outputBuses.size(); ++i)
            layout.outputBuses.getReference (i) = (i == 0 ? out : Set::disabled());

        return plugin.setBusesLayout (layout);
    };

    // Vocal chains are stereo here (mono mics are duplicated to L/R), but plenty
    // of vocal plugins are mono-only, so fall back gracefully. Side-chain and aux
    // buses are disabled because nothing feeds them.
    if (! tryMainBuses (Set::stereo(), Set::stereo())
        && ! tryMainBuses (Set::mono(), Set::stereo())
        && ! tryMainBuses (Set::mono(), Set::mono()))
    {
        // Keep whatever the plugin defaults to; process() copes with any count.
    }
}

//==============================================================================
void PluginChain::process (juce::AudioBuffer<float>& buffer)
{
    const juce::ScopedLock sl (lock);

    if (! prepared || slots.empty())
        return;

    const int numSamples = juce::jmin (buffer.getNumSamples(), scratch.getNumSamples());
    float* left = buffer.getWritePointer (0);
    float* right = buffer.getWritePointer (1);

    for (auto& slotPtr : slots)
    {
        auto& slot = *slotPtr;
        if (slot.bypassed.load (std::memory_order_relaxed) || slot.plugin->isSuspended())
            continue;

        const int numChannels = juce::jlimit (1, kMaxPluginChannels, juce::jmax (slot.numInputs, slot.numOutputs));

        for (int ch = 0; ch < numChannels; ++ch)
            channelPointers.set (ch, scratch.getWritePointer (ch));

        float* s0 = channelPointers[0];

        if (slot.numInputs >= 2)
        {
            juce::FloatVectorOperations::copy (s0, left, numSamples);
            juce::FloatVectorOperations::copy (channelPointers[1], right, numSamples);
        }
        else
        {
            // Mono-input plugin: feed it the mid signal.
            juce::FloatVectorOperations::copyWithMultiply (s0, left, 0.5f, numSamples);
            juce::FloatVectorOperations::addWithMultiply (s0, right, 0.5f, numSamples);
            if (numChannels > 1)
                juce::FloatVectorOperations::clear (channelPointers[1], numSamples);
        }

        for (int ch = 2; ch < numChannels; ++ch)
            juce::FloatVectorOperations::clear (channelPointers[ch], numSamples);

        juce::AudioBuffer<float> view (channelPointers.getRawDataPointer(), numChannels, numSamples);
        midi.clear();
        slot.plugin->processBlock (view, midi);

        juce::FloatVectorOperations::copy (left, s0, numSamples);
        juce::FloatVectorOperations::copy (right, slot.numOutputs >= 2 ? channelPointers[1] : s0, numSamples);
    }
}

//==============================================================================
void PluginChain::addPlugin (std::unique_ptr<juce::AudioPluginInstance> plugin,
                             const juce::PluginDescription& description,
                             bool bypassed, const juce::MemoryBlock* state)
{
    if (plugin == nullptr)
        return;

    auto slot = std::make_unique<Slot>();
    configureLayout (*plugin);

    if (state != nullptr && state->getSize() > 0)
        plugin->setStateInformation (state->getData(), (int) state->getSize());

    slot->description = description;
    slot->plugin = std::move (plugin);
    slot->bypassed = bypassed;
    slot->numInputs = slot->plugin->getTotalNumInputChannels();
    slot->numOutputs = slot->plugin->getTotalNumOutputChannels();

    // Heavy lifting (allocations, plugin init) happens before we take the lock.
    if (prepared)
        prepareSlot (*slot);

    {
        const juce::ScopedLock sl (lock);
        slots.push_back (std::move (slot));
    }

    sendChangeMessage();
}

void PluginChain::removePlugin (int index)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return;

    std::unique_ptr<Slot> removed;
    {
        const juce::ScopedLock sl (lock);
        removed = std::move (slots[(size_t) index]);
        slots.erase (slots.begin() + index);
    }

    removed->plugin->releaseResources();
    removed.reset();
    sendChangeMessage();
}

void PluginChain::movePlugin (int fromIndex, int toIndex)
{
    if (! juce::isPositiveAndBelow (fromIndex, size()) || ! juce::isPositiveAndBelow (toIndex, size()) || fromIndex == toIndex)
        return;

    {
        const juce::ScopedLock sl (lock);
        auto moving = std::move (slots[(size_t) fromIndex]);
        slots.erase (slots.begin() + fromIndex);
        slots.insert (slots.begin() + toIndex, std::move (moving));
    }

    sendChangeMessage();
}

void PluginChain::setBypassed (int index, bool shouldBypass)
{
    if (auto* slot = getSlot (index))
    {
        slot->bypassed = shouldBypass;
        sendChangeMessage();
    }
}

void PluginChain::clear()
{
    std::vector<std::unique_ptr<Slot>> old;
    {
        const juce::ScopedLock sl (lock);
        old.swap (slots);
    }

    for (auto& slot : old)
        slot->plugin->releaseResources();

    old.clear();
    sendChangeMessage();
}

int PluginChain::getTotalLatencySamples() const
{
    int total = 0;
    for (auto& slot : slots)
        if (! slot->bypassed.load())
            total += slot->plugin->getLatencySamples();
    return total;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> PluginChain::createStateXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("CHAIN");

    for (auto& slot : slots)
    {
        auto* e = xml->createNewChildElement ("SLOT");
        e->setAttribute ("bypassed", slot->bypassed.load());
        e->addChildElement (slot->description.createXml().release());

        juce::MemoryBlock state;
        slot->plugin->getStateInformation (state);
        e->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
    }

    return xml;
}

void PluginChain::restoreFromXml (const juce::XmlElement& xml, juce::AudioPluginFormatManager& formats, juce::StringArray& errors)
{
    clear();

    for (auto* e : xml.getChildWithTagNameIterator ("SLOT"))
    {
        auto* descXml = e->getChildByName ("PLUGIN");
        if (descXml == nullptr)
            continue;

        juce::PluginDescription desc;
        if (! desc.loadFromXml (*descXml))
            continue;

        juce::String error;
        auto instance = formats.createPluginInstance (desc, currentRate, currentBlock, error);

        if (instance == nullptr)
        {
            errors.add (desc.name + ": " + (error.isNotEmpty() ? error : juce::String ("failed to load")));
            continue;
        }

        juce::MemoryBlock state;
        if (auto* stateXml = e->getChildByName ("STATE"))
            state.fromBase64Encoding (stateXml->getAllSubText());

        addPlugin (std::move (instance), desc, e->getBoolAttribute ("bypassed"), &state);
    }
}

} // namespace vb
