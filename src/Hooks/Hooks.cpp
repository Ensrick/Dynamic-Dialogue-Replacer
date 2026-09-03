#include "Hooks.h"

#include "DialogueMenuEx.h"

#include <REL/Pattern.h>

namespace
{
    [[nodiscard]] bool IsReadableAddress(std::uintptr_t a_address, std::size_t a_size, std::string_view a_name)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (a_address == 0 || VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
            logger::critical("{} does not resolve to committed memory", a_name);
            return false;
        }

        const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto regionEnd = regionBegin + memory.RegionSize;
        if (regionEnd < regionBegin || a_address < regionBegin || a_address >= regionEnd || a_size > (regionEnd - a_address)) {
            logger::critical("{} crosses a memory-region boundary", a_name);
            return false;
        }

        const auto protection = memory.Protect & 0xFFu;
        const bool readable = protection == PAGE_READONLY ||
                              protection == PAGE_READWRITE ||
                              protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READ ||
                              protection == PAGE_EXECUTE_READWRITE ||
                              protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (memory.Protect & PAGE_GUARD) != 0) {
            logger::critical("{} does not resolve to readable memory", a_name);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool IsExecutableAddress(std::uintptr_t a_address, std::string_view a_name)
    {
        MEMORY_BASIC_INFORMATION memory{};
        if (a_address == 0 || VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
            logger::critical("{} does not resolve to committed memory", a_name);
            return false;
        }

        const auto protection = memory.Protect & 0xFFu;
        const bool executable = protection == PAGE_EXECUTE ||
                                protection == PAGE_EXECUTE_READ ||
                                protection == PAGE_EXECUTE_READWRITE ||
                                protection == PAGE_EXECUTE_WRITECOPY;
        if (!executable) {
            logger::critical("{} does not resolve to executable memory", a_name);
        }
        return executable;
    }

    [[nodiscard]] bool IsCallSite(std::uintptr_t a_address, std::string_view a_name)
    {
        if (!IsExecutableAddress(a_address, a_name)) {
            return false;
        }
        if (!REL::make_pattern<"E8">().match(a_address)) {
            logger::critical("{} does not begin with the expected E8 call opcode", a_name);
            return false;
        }
        return true;
    }
}

namespace RE
{
    int64_t AddTopic(RE::MenuTopicManager* a_this, RE::TESTopic* a_topic, int64_t a_3, int64_t a_4)
    {
        // Audited AE 1.7.104 address-library ID.
        using func_t = decltype(&RE::AddTopic);
        REL::Relocation<func_t> func{ REL::ID(35303) };
        return func(a_this, a_topic, a_3, a_4);
    }
}  // namespace RE

namespace DDR
{
    bool Hooks::Install()
    {
        // This build intentionally uses the 1.7.104 AE IDs and offsets rather
        // than silently accepting another runtime through variant fallbacks.
        REL::Relocation<std::uintptr_t> target{ REL::ID(35249) };
        const auto targetAddress = target.address();
        const auto setSubtitleSite = targetAddress + 0x61;
        const auto constructResponseSite = targetAddress + 0xDE;
        const auto addTopicSite = REL::Relocation<std::uintptr_t>{ REL::ID(35287) }.address() + 0x154;
        const auto addTopicSecondarySite = REL::Relocation<std::uintptr_t>{ REL::ID(35304) }.address() + 0x6C;

        REL::Relocation<std::uintptr_t> dialogueMenuVtable{ RE::VTABLE_DialogueMenu[0] };
        const auto processMessageSlot = dialogueMenuVtable.address() + (sizeof(std::uintptr_t) * 0x4);
        if (!IsReadableAddress(processMessageSlot, sizeof(std::uintptr_t), "DialogueMenu::ProcessMessage vtable slot")) {
            logger::critical("Hook preflight failed before any game code was modified");
            return false;
        }
        const auto processMessageAddress = *reinterpret_cast<const std::uintptr_t*>(processMessageSlot);

        if (!IsExecutableAddress(targetAddress, "PopulateTopicInfo") ||
            !IsCallSite(setSubtitleSite, "SetSubtitle call site") ||
            !IsCallSite(constructResponseSite, "ConstructResponse call site") ||
            !IsCallSite(addTopicSite, "AddTopic primary call site") ||
            !IsCallSite(addTopicSecondarySite, "AddTopic secondary call site") ||
            !IsExecutableAddress(processMessageAddress, "DialogueMenu::ProcessMessage vtable entry")) {
            logger::critical("Hook preflight failed before any game code was modified");
            return false;
        }

        SKSE::AllocTrampoline(64);
        auto& trampoline = SKSE::GetTrampoline();

        _PopulateTopicInfo = reinterpret_cast<PopulateTopicInfoType>(targetAddress);
        auto status = DetourTransactionBegin();
        if (status != NO_ERROR) {
            logger::critical("DetourTransactionBegin failed with error {}", status);
            return false;
        }

        const auto abortTransaction = []() {
            const auto abortStatus = DetourTransactionAbort();
            if (abortStatus != NO_ERROR) {
                logger::critical("DetourTransactionAbort failed with error {}", abortStatus);
            }
        };

        status = DetourUpdateThread(GetCurrentThread());
        if (status != NO_ERROR) {
            logger::critical("DetourUpdateThread failed with error {}", status);
            abortTransaction();
            return false;
        }

        status = DetourAttach(
            reinterpret_cast<PVOID*>(&_PopulateTopicInfo),
            reinterpret_cast<PVOID>(&PopulateTopicInfo));
        if (status != NO_ERROR) {
            logger::critical("DetourAttach failed with error {}", status);
            abortTransaction();
            return false;
        }

        status = DetourTransactionCommit();
        if (status != NO_ERROR) {
            logger::critical("DetourTransactionCommit failed with error {}", status);
            return false;
        }

        // From this point onward a hook is live. CommonLib failures must take
        // the configured log-only fatal path; returning would unload the DLL
        // while the game still holds pointers into it.
        _SetSubtitle = trampoline.write_call<5>(setSubtitleSite, SetSubtitle);
        _ConstructResponse = trampoline.write_call<5>(constructResponseSite, ConstructResponse);
        _AddTopic = trampoline.write_call<5>(addTopicSite, AddTopic);
        trampoline.write_call<5>(addTopicSecondarySite, AddTopic);

        DialogueMenuEx::Install();

        logger::info("Installed hooks");
        return true;
    }

    int64_t Hooks::PopulateTopicInfo(int64_t a_1, RE::TESTopic* a_2, RE::TESTopicInfo* a_3, RE::Character* a_speaker, RE::TESTopicInfo::TESResponse* a_5)
    {
        _response.responseNumber = a_5->responseNumber;
        if (_response.responseNumber == 1) {
            _response.response = DialogueManager::GetSingleton()->FindReplacementResponse(a_speaker, a_3, a_5);
            _response.speaker = a_speaker;
        }
        if (_response.response && _response.response->ShouldCut(_response.responseNumber)) {
            delete a_5->next;
            a_5->next = nullptr;
        }
        return _PopulateTopicInfo(a_1, a_2, a_3, a_speaker, a_5);
    }

    char* Hooks::SetSubtitle(RE::DialogueResponse* a_response, char* a_text, int32_t a_3)
    {
        std::string text;
        if (_response.response && _response.response->HasReplacementSubtitle(_response.responseNumber)) {
            const auto replace = _response.response->GetSubtitle(_response.responseNumber);
            logger::info("replacing subtitle {} with {}", a_text, replace);
            text = replace;
        } else {
            text = a_text;
        }
        DialogueManager::GetSingleton()->ApplyLuaScripts(text, _response.speaker, LuaScript::Type::Response);
        return _SetSubtitle(a_response, text.data(), a_3);
    }

    bool Hooks::ConstructResponse(RE::TESTopicInfo::TESResponse* a_response, char* a_filePath, RE::BGSVoiceType* a_voiceType, RE::TESTopic* a_topic, RE::TESTopicInfo* a_topicInfo)
    {
        if (!_ConstructResponse(a_response, a_filePath, a_voiceType, a_topic, a_topicInfo)) {
            return false;
        }
        if (_response.response && _response.response->HasReplacementVoiceFile(_response.responseNumber)) {
            std::string filePath{ a_filePath };
            const auto path = _response.response->GetVoiceFilePath(a_topic, a_topicInfo, a_voiceType, a_response->responseNumber);
            logger::info("replacing voice file {} with {}", filePath, path);
            *a_filePath = NULL;
            strcat_s(a_filePath, 0x104ui64, path.c_str());
        }
        return true;
    }

    int64_t Hooks::AddTopic(RE::MenuTopicManager* a_this, RE::TESTopic* a_topic, RE::TESTopic* a_activeTopic, uint64_t a_4)
    {
        const auto parentId = a_activeTopic ? a_activeTopic->GetFormID() : 0;
        const auto topicId = a_topic->GetFormID();
        const auto target = a_this->speaker.get().get();
        auto topicEdits = DialogueManager::GetSingleton()->FindReplacementTopic(parentId, topicId, target, true);
        if (topicEdits.empty()) {
            return _AddTopic(a_this, a_topic, a_activeTopic, a_4);
        }
        bool hasValidResponse = false;
        auto currInfo = a_topic->topicInfos;
        for (auto i = a_topic->numTopicInfos; i > 0; (i--, currInfo++)) {
            if (currInfo && *currInfo) {
                if ((*currInfo)->objConditions.IsTrue(target, RE::PlayerCharacter::GetSingleton())) {
                    hasValidResponse = true;
                    break;
                }
            }
        }
        bool firstPass = !a_this->dialogueList || a_this->dialogueList->empty();
        for (auto&& it : topicEdits) {
            if (!hasValidResponse && it->VerifyExistingConditions()) {
                continue;
            }
            if (firstPass) {
                // Inject additional topics
                for (const auto& injectTopic : it->GetInjections()) {
                    _AddTopic(a_this, injectTopic, a_activeTopic, a_4);
                }
            }
            if (it->AffectsInfoTopic(a_topic)) {
                // Hide topic, consumes the topic
                if (it->IsHidden()) {
                    return 0;
                }
                // Replace active sub-topic
                if (const auto replace = it->GetReplacingTopic()) {
                    a_topic = replace;
                }
            }
            if (!it->ShouldProceed()) {
                break;
            }
        }
        return _AddTopic(a_this, a_topic, a_activeTopic, a_4);
    }

}  // namespace DDR
