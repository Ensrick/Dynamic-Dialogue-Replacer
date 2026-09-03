#include <SKSE/SKSE.h>

// Use the legacy binary-compatible declaration builder so every unused entry
// in compatibleVersions is explicitly zero-filled rather than defaulting to
// an additional compatibility version.
extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData version;
    version.PluginVersion({ 1, 4, 1, 0 });
    version.PluginName("DynamicDialogueReplacer");
    version.AuthorName("KrisV-777");
    version.CompatibleVersions({ SKSE::RUNTIME_SSE_1_7_104 });
    return version;
}();
