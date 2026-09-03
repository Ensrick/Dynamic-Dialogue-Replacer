#include "Dialogue/DialogueManager.h"
#include "Hooks/Hooks.h"
#include "Papyrus.h"

void SKSEMessageHandler(SKSE::MessagingInterface::Message* message) noexcept
{
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        DialogueManager::GetSingleton()->Init();
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    const auto plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto InitLogger = [&plugin]() -> bool {
#ifndef NDEBUG
        auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
        auto path = logger::log_directory();
        if (!path)
            return false;
        *path /= std::format("{}.log", plugin->GetName());
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif
        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
#ifndef NDEBUG
        log->set_level(spdlog::level::trace);
        log->flush_on(spdlog::level::trace);
#else
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
#endif
        spdlog::set_default_logger(std::move(log));
#ifndef NDEBUG
        spdlog::set_pattern("%s(%#): [%T] [%^%l%$] %v"s);
#else
        spdlog::set_pattern("[%T] [%^%l%$] %v"s);
#endif

        logger::info("{} v{}", plugin->GetName(), plugin->GetVersion());
        return true;
    };

    if (!InitLogger()) {
        return false;
    }

    if (a_skse->IsEditor()) {
        logger::critical("Loaded in editor, marking as incompatible");
        return false;
    }

    const auto runtime = a_skse->RuntimeVersion();
    if (runtime != SKSE::RUNTIME_SSE_1_7_104) {
        logger::critical("Unsupported runtime version {}; this build requires 1.7.104.0", runtime.string());
        return false;
    }

    SKSE::Init(a_skse);

    const auto msging = SKSE::GetMessagingInterface();
    const auto papyrus = SKSE::GetPapyrusInterface();
    if (!msging || !papyrus) {
        logger::critical("Required SKSE interfaces are unavailable");
        return false;
    }

    if (!DDR::Hooks::Install()) {
        logger::critical("Hook installation failed before any persistent callback was registered");
        return false;
    }

    if (!msging->RegisterListener(SKSEMessageHandler)) {
        SKSE::stl::report_and_fail("Failed to register the DDR message listener after hooks were installed");
    }
    if (!papyrus->Register(DDR::Papyrus::RegisterFunctions)) {
        SKSE::stl::report_and_fail("Failed to register DDR Papyrus functions after hooks were installed");
    }

    logger::info("{} loaded", plugin->GetName());

    return true;
}
