#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"
#include "PropertyMetadata.h"

namespace ShaderLab::Effects
{
    // Describes a built-in D2D effect available in the registry.
    // Used by the node graph editor to present the effect palette and
    // by the factory method to create fully-configured EffectNodes.
    struct EffectDescriptor
    {
        GUID        clsid{};
        std::wstring name;
        std::wstring category;      // e.g. "Blur", "Color", "Composition"

        // Pin layout.
        std::vector<Graph::PinDescriptor> inputPins;
        std::vector<Graph::PinDescriptor> outputPins;

        // Default property values (key = property name or index string).
        std::map<std::wstring, Graph::PropertyValue> defaultProperties;

        // UI metadata for the properties panel (key must match defaultProperties key).
        std::map<std::wstring, PropertyMetadata> propertyMetadata;
    };

    // Singleton catalog of all registered built-in D2D effects.
    // Populated once at startup with the common D2D effects.
    class EffectRegistry
    {
    public:
        // Access the global registry instance.
        static EffectRegistry& Instance();

        // Look up a descriptor by name (case-insensitive match).
        const EffectDescriptor* FindByName(std::wstring_view name) const;

        // Look up a descriptor by CLSID.
        const EffectDescriptor* FindByClsid(const GUID& clsid) const;

        // Get all descriptors (for UI palette / combo boxes).
        const std::vector<EffectDescriptor>& All() const { return m_descriptors; }

        // Get descriptors filtered by category.
        std::vector<const EffectDescriptor*> ByCategory(std::wstring_view category) const;

        // Get the list of unique category names.
        std::vector<std::wstring> Categories() const;

        // Create a fully-configured EffectNode from a descriptor.
        // The node has type=BuiltInEffect, CLSID set, pins configured,
        // and default properties populated.
        static Graph::EffectNode CreateNode(const EffectDescriptor& desc);

        // Create an Output node (no CLSID, single input pin, no output).
        static Graph::EffectNode CreateOutputNode();

    private:
        EffectRegistry();
        void RegisterBuiltInEffects();

        // Helper to register a single effect.
        void Register(EffectDescriptor desc);

        std::vector<EffectDescriptor> m_descriptors;
    };
}
