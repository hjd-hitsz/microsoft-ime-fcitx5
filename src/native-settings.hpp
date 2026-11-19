#pragma once

#include <windows.h>

#include <array>

namespace ime {

// Registry options used by the x64 Microsoft Chinese Keyboard engine.
constexpr wchar_t NativeSettingsKey[] = L"Software\\Microsoft\\BingKeyboard\\CHS";
constexpr std::array<const wchar_t *, 4> NativeSettingNames{
    L"EnableAutoCorrection", L"EnableFuzzyInput", L"FuzzyPairSetting", L"EnableSelfLearning",
};

struct NativeSettings {
    DWORD autoCorrection = 1;
    DWORD fuzzyEnabled = 0;
    DWORD fuzzyPairs = 0;
    DWORD selfLearning = 1;
};

inline DWORD readNativeDword(const wchar_t *name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, NativeSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size)
        == ERROR_SUCCESS ? value : fallback;
}

inline NativeSettings readNativeSettings() {
    const NativeSettings defaults;
    return {
        readNativeDword(NativeSettingNames[0], defaults.autoCorrection),
        readNativeDword(NativeSettingNames[1], defaults.fuzzyEnabled),
        readNativeDword(NativeSettingNames[2], defaults.fuzzyPairs),
        readNativeDword(NativeSettingNames[3], defaults.selfLearning),
    };
}

inline LONG writeNativeSettings(const NativeSettings &settings) {
    const auto before = readNativeSettings();
    HKEY key;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, NativeSettingsKey, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;

    const std::array<DWORD, 4> values{settings.autoCorrection, settings.fuzzyEnabled, settings.fuzzyPairs, settings.selfLearning};
    const std::array<DWORD, 4> previous{before.autoCorrection, before.fuzzyEnabled, before.fuzzyPairs, before.selfLearning};
    size_t written = 0;
    for (; written < values.size(); ++written) {
        result = RegSetValueExW(key, NativeSettingNames[written], 0, REG_DWORD,
                                reinterpret_cast<const BYTE *>(&values[written]), sizeof(DWORD));
        if (result != ERROR_SUCCESS) break;
    }
    if (result != ERROR_SUCCESS) {
        for (size_t index = 0; index < written; ++index) {
            RegSetValueExW(key, NativeSettingNames[index], 0, REG_DWORD,
                           reinterpret_cast<const BYTE *>(&previous[index]), sizeof(DWORD));
        }
    }
    RegCloseKey(key);
    return result;
}

}
