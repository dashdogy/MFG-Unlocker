#pragma once

// pluginabi.1: version 2 of the plugin API table, as our OptiScaler fork had it before upstream merged the API
// (upstream plugin-api f24ebc05 plus ComboEx and ItemDeactivatedAfterEdit appended). Upstream never shipped version 2,
// so a version 2 table of exactly this size is this layout; optiscaler_menu_tab adapts it to upstream's.

#include "optiscaler_plugin.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define OPTI_PLUGIN_FORK2_API_VERSION 2

    typedef struct OptiPluginApiFork2
    {
        uint32_t size;
        uint32_t version;

        bool (*RegisterTab)(OptiPlugin* plugin, const char* name, const char* description, OptiDrawTabFn draw,
                            void* userData);

        void (*Section)(const char* title);
        void (*Separator)(void);
        void (*SameLine)(void);
        void (*Spacing)(void);
        void (*Disabled)(bool disabled);
        void (*ItemTooltip)(const char* text);

        void (*Text)(const char* text);
        void (*TextDisabled)(const char* text);
        void (*TextColored)(float r, float g, float b, float a, const char* text);

        bool (*Button)(const char* label);
        bool (*Checkbox)(const char* label, bool* value);
        bool (*SliderFloat)(const char* label, float* value, float min, float max, const char* format);
        bool (*SliderInt)(const char* label, int32_t* value, int32_t min, int32_t max, const char* format);
        bool (*Combo)(const char* label, int32_t* current, const char* const* items, int32_t itemCount);
        bool (*InputText)(const char* label, char* buffer, size_t bufferSize);
        bool (*ColorEdit)(const char* label, float* rgba, bool withAlpha);

        float (*GetMenuScale)(void);
        bool (*IsMenuOpen)(void);

        bool (*GetBool)(OptiPlugin* plugin, const char* key, bool defaultValue);
        int32_t (*GetInt)(OptiPlugin* plugin, const char* key, int32_t defaultValue);
        float (*GetFloat)(OptiPlugin* plugin, const char* key, float defaultValue);
        bool (*GetString)(OptiPlugin* plugin, const char* key, char* buffer, size_t bufferSize,
                          const char* defaultValue);

        void (*SetBool)(OptiPlugin* plugin, const char* key, bool value);
        void (*SetInt)(OptiPlugin* plugin, const char* key, int32_t value);
        void (*SetFloat)(OptiPlugin* plugin, const char* key, float value);
        void (*SetString)(OptiPlugin* plugin, const char* key, const char* value);
        void (*ResetValue)(OptiPlugin* plugin, const char* key);

        bool (*SaveSettings)(OptiPlugin* plugin);

        void (*Log)(OptiPlugin* plugin, OptiLogLevel level, const char* message);

        // Version 2: bit i of disabledItems greys out items[i] (the first 32 items)
        bool (*ComboEx)(const char* label, int32_t* current, const char* const* items, int32_t itemCount,
                        uint32_t disabledItems);
        bool (*ItemDeactivatedAfterEdit)(void);

    } OptiPluginApiFork2;

#ifdef __cplusplus
}

// Both layouts agree up to ItemTooltip and from IsMenuOpen to Log; between them upstream has ItemDeactivatedAfterEdit
// where the fork had GetMenuScale.
static_assert(offsetof(OptiPluginApiFork2, ItemTooltip) == offsetof(OptiPluginApi, ItemTooltip), "fork2 layout");
static_assert(offsetof(OptiPluginApiFork2, Text) == offsetof(OptiPluginApi, ItemDeactivatedAfterEdit), "fork2 layout");
static_assert(offsetof(OptiPluginApiFork2, IsMenuOpen) == offsetof(OptiPluginApi, IsMenuOpen), "fork2 layout");
static_assert(offsetof(OptiPluginApiFork2, Log) == offsetof(OptiPluginApi, Log), "fork2 layout");
static_assert(sizeof(OptiPluginApiFork2) == offsetof(OptiPluginApiFork2, Log) + 3 * sizeof(void*), "fork2 layout");
#endif
